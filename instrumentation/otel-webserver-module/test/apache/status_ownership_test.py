#!/usr/bin/env python3
"""Real HTTP status ownership tests. Requires test-only mod_status_fixture.so."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import uuid

CONTAINER = sys.argv[1]
CONFIG = "/etc/httpd/conf.d/opentelemetry.conf"
BACKEND = r'''
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
import json
class Echo(BaseHTTPRequestHandler):
    def do_GET(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        last = self.path.split('?')[0].rsplit('/', 1)[-1]
        status = int(last) if last.isdigit() else 200
        data = json.dumps({'method': self.command, 'body': body,
                           'traceparent': self.headers.get('traceparent')})
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    do_POST = do_GET
    do_PUT = do_GET
    do_PATCH = do_GET
    def log_message(self, *args): pass
HTTPServer(('127.0.0.1', 8765), Echo).serve_forever()
'''


def docker(*args):
    return subprocess.check_output(["docker", *args], text=True, stderr=subprocess.STDOUT)


def field(block, key):
    match = re.search(r"(?m)^\s*" + re.escape(key) + r"\s*:\s*(\S+)", block)
    return match.group(1) if match else None


original = docker("exec", CONTAINER, "cat", CONFIG)
base = "\n".join(line for line in original.splitlines() if not line.lower().startswith((
    "apachemoduleotelpropagator", "apachemoduletraceroutes", "apachemodulereportallinstrumentedmodules")))
base = "LoadModule status_fixture_module modules/mod_status_fixture.so\n" + base
base += """
ApacheModuleOtelPropagatorType w3c
ApacheModuleOtelPropagatorExtract OFF
ApacheModuleOtelPropagatorInject ON
ApacheModuleReportAllInstrumentedModules ON
ProxyPass /proxy/ http://127.0.0.1:8765/
ProxyPass /recovered/ http://127.0.0.1:8765/
ProxyPass /unavailable/ http://127.0.0.1:8766/
"""

with tempfile.TemporaryDirectory(prefix="otel-status-") as directory:
    config = Path(directory, "opentelemetry.conf")
    backend = Path(directory, "backend.py")
    backend.write_text(BACKEND)
    docker("cp", str(backend), CONTAINER + ":/tmp/otel-status-backend.py")
    captures = []
    try:
        config.write_text(base)
        docker("cp", str(config), CONTAINER + ":" + CONFIG)
        docker("exec", CONTAINER, "httpd", "-t")
        docker("restart", CONTAINER)
        docker("exec", "-d", CONTAINER, "python", "/tmp/otel-status-backend.py")
        for _ in range(30):
            try:
                docker("exec", CONTAINER, "curl", "-fsS", "http://localhost:8765/")
                break
            except subprocess.CalledProcessError:
                time.sleep(0.1)
        count = 0
        for method in ["GET", "POST", "PUT", "PATCH"]:
            cases = [("/proxy/" + str(status), status, status >= 500, status >= 400)
                     for status in [200, 201, 302, 400, 401, 403, 404, 429, 499, 500, 502, 503]]
            cases += [("/recovered/503", 200, True, True),
                      ("/internal-failure", 200, True, None),
                      ("/unavailable/echo", 503, True, True)]
            for path, status, server_error, client_error in cases:
                marker = uuid.uuid4().hex
                since = docker("exec", CONTAINER, "date", "-u", "+%Y-%m-%dT%H:%M:%S.%NZ").strip()
                args = ["exec", CONTAINER, "curl", "-sS", "-D", "-", "-X", method,
                        "-H", "traceparent: 00-" + marker + "-2222222222222222-01"]
                if method != "GET":
                    args += ["--data-binary", "test-body"]
                wire = docker(*args, "http://localhost" + path + "?status-test=" + marker)
                headers, body = wire.replace("\r\n", "\n").split("\n\n", 1)
                assert int(headers.splitlines()[0].split()[1]) == status, wire
                for _ in range(50):
                    logs = docker("logs", "--since", since, CONTAINER)
                    blocks = logs.split("{\n  name")[1:]
                    roots = [b for b in blocks if field(b, "span kind") == "Server" and marker in b]
                    if roots:
                        break
                    time.sleep(0.1)
                assert len(roots) == 1, logs
                root = roots[0]
                trace = field(root, "trace_id")
                children = [b for b in blocks if field(b, "trace_id") == trace and field(b, "span kind") != "Server"]
                clients = [b for b in children if field(b, "span kind") == "Client"]
                internals = [b for b in children if field(b, "span kind") == "Internal"]
                assert field(root, "status") == ("Error" if server_error else "Unset"), root
                assert field(root, "http.response.status_code") == str(status), root
                assert (field(root, "error.type") is not None) == server_error, root
                assert re.findall(r"(?im)^x-trace-id: ([0-9a-f]{32})$", headers) == [trace]
                if client_error is None:
                    assert not clients, clients
                    failed = [b for b in internals if field(b, "status") == "Error"]
                    assert len(failed) == 1 and "mod_status_fixture.c_handler" in failed[0], logs
                    assert field(failed[0], "error.type") == "500", failed[0]
                else:
                    assert len(clients) == 1, logs
                    client = clients[0]
                    assert field(client, "status") == ("Error" if client_error else "Unset"), client
                    assert field(client, "parent_span_id") == field(root, "span_id"), client
                    if path.startswith("/unavailable/"):
                        assert field(client, "http.response.status_code") is None, client
                        assert field(client, "error.type") == "proxy_error", client
                    else:
                        backend_status = path.rsplit("/", 1)[-1]
                        assert field(client, "http.response.status_code") == backend_status, client
                        echo = json.loads(body)
                        assert echo["method"] == method and echo["body"] == ("" if method == "GET" else "test-body"), echo
                        assert echo["traceparent"] == "00-" + trace + "-" + field(client, "span_id") + "-01", echo
                    assert all(field(b, "status") == "Unset" for b in internals), logs
                assert all(field(b, "http.response.status_code") is None for b in internals), logs
                print("PASS: %s %s wire=%s SERVER=%s CLIENT=%s" %
                      (method, path, status, field(root, "status"), field(clients[0], "status") if clients else "absent"), flush=True)
                if method == "GET" and path in ["/proxy/404", "/proxy/503", "/recovered/503", "/internal-failure"]:
                    captures.append(wire + "\n" + "\n".join("{\n  name" + b for b in [root] + children))
                count += 1
        if len(sys.argv) > 2:
            Path(sys.argv[2]).write_text("\n\n".join(captures))
        print("PASS: %d real requests verified" % count, flush=True)
    finally:
        config.write_text(original)
        docker("cp", str(config), CONTAINER + ":" + CONFIG)
        docker("restart", CONTAINER)
