#!/usr/bin/env python3
"""Verify HTTP SERVER/CLIENT and Apache INTERNAL semantics in the Docker fixture."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import uuid

CONTAINER = sys.argv[1] if len(sys.argv) > 1 else "otel-apache-rhel8-test"
CONFIG = "/etc/httpd/conf.d/opentelemetry.conf"
BACKEND = r'''
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
import json
class Echo(BaseHTTPRequestHandler):
    def do_GET(self):
        data = json.dumps({'path': self.path, 'protocol': self.request_version, 'host': self.headers.get('Host')})
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    do_PROPFIND = do_GET
    def log_message(self, *args): pass
HTTPServer(('127.0.0.1', 8765), Echo).serve_forever()
'''


def docker(*args):
    return subprocess.check_output(["docker", *args], encoding="utf-8", stderr=subprocess.STDOUT)


original = docker("exec", CONTAINER, "cat", CONFIG)
prefixes = ("apachemoduleotelpropagator", "apachemoduletraceroutes", "apachemodulereportallinstrumentedmodules")
base = "\n".join(line for line in original.splitlines() if not line.lower().startswith(prefixes))
base += """
ApacheModuleOtelPropagatorType w3c
ApacheModuleOtelPropagatorExtract OFF
ApacheModuleOtelPropagatorInject ON
ProxyPass /services/ http://127.0.0.1:8765/
ProxyPass /creds/ http://test-user:test-password@127.0.0.1:8765/
<Location /services/force1/>
    SetEnv force-proxy-request-1.0 1
</Location>
"""


def fields(block):
    return dict(re.findall(r"(?m)^\s*(trace_id|span_id|parent_span_id|span kind|status)\s*:\s*(\S+)", block))


with tempfile.TemporaryDirectory(prefix="otel-semantics-") as directory:
    config_file = Path(directory) / "opentelemetry.conf"
    backend = Path(directory) / "backend.py"
    backend.write_text(BACKEND, encoding="utf-8")
    docker("cp", str(backend), CONTAINER + ":/tmp/otel-semantics-backend.py")
    try:
        for report_all in [True, False]:
            config_file.write_text(base + "\nApacheModuleReportAllInstrumentedModules " +
                                   ("ON" if report_all else "OFF") + "\n", encoding="utf-8")
            docker("cp", str(config_file), CONTAINER + ":" + CONFIG)
            docker("exec", CONTAINER, "httpd", "-t")
            docker("restart", CONTAINER)
            docker("exec", "-d", CONTAINER, "python", "/tmp/otel-semantics-backend.py")
            for _ in range(30):
                try:
                    docker("exec", CONTAINER, "curl", "-fsS", "http://localhost:8765/")
                    break
                except subprocess.CalledProcessError:
                    time.sleep(0.1)
            cases = [("GET", "/services/echo", "1.1"), ("PROPFIND", "/services/echo", "1.1"),
                     ("GET", "/services/force1/echo", "1.0"), ("GET", "/creds/echo", "1.1"),
                     ("GET", "/services/graceful", "1.1")]
            for method, path, version in cases:
                if path.endswith("/graceful"):
                    docker("exec", CONTAINER, "httpd", "-k", "graceful")
                    time.sleep(0.2)
                marker = uuid.uuid4().hex
                query = "semantic=" + marker + "&sig=secret-value&keep=ok"
                since = docker("exec", CONTAINER, "date", "-u", "+%Y-%m-%dT%H:%M:%S.%NZ").strip()
                wire = docker("exec", CONTAINER, "curl", "-sS", "-D", "-", "-X", method,
                              "-H", "Host: frontend.example:8080", "http://localhost" + path + "?" + query)
                headers, body = wire.replace("\r\n", "\n").split("\n\n", 1)
                for _ in range(50):
                    logs = docker("logs", "--since", since, CONTAINER)
                    blocks = logs.split("{\n  name")[1:]
                    root = next((b for b in blocks if "span kind     : Server" in b and marker in b), None)
                    if root:
                        break
                    time.sleep(0.1)
                assert root, logs
                clients = [b for b in blocks if "span kind     : Client" in b]
                assert len(clients) == 1, logs
                client = clients[0]
                root_fields, client_fields = fields(root), fields(client)
                assert client_fields["parent_span_id"] == root_fields["span_id"], client
                assert client_fields["trace_id"] == root_fields["trace_id"], client
                assert re.findall(r"(?im)^x-trace-id: ([0-9a-f]{32})$", headers) == [root_fields["trace_id"]]
                name, normalized = ("GET", "GET") if method == "GET" else ("HTTP", "_OTHER")
                assert root.lstrip().startswith(": " + name + "\n"), root
                assert client.lstrip().startswith(": " + name + "\n"), client
                for block in [root, client]:
                    assert "http.request.method: " + normalized in block, block
                    assert ("http.request.method_original: PROPFIND" in block) == (method == "PROPFIND"), block
                    assert "status        : Unset" in block, block
                    attrs = block.split("  attributes    :")[1].split("  events")[0]
                    assert "sig=REDACTED" in attrs and "secret-value" not in attrs, attrs
                    assert "http.upstream." not in attrs, attrs
                assert "server.address: frontend.example" in root and "server.port: 8080" in root, root
                assert "server.address: 127.0.0.1" in client and "server.port: 8765" in client, client
                assert "url.full:" not in root and "url.full:" in client, logs
                assert "network.protocol.version: " + version in client, client
                assert "network.peer.address: 127.0.0.1" in client and "network.peer.port: 8765" in client, client
                if path.startswith("/creds/"):
                    assert "test-user" not in client and "test-password" not in client, client
                    # Apache may already remove userinfo before our handler
                    # wrapper; either omission or explicit redaction is safe.
                else:
                    echo = json.loads(body)
                    assert "sig=secret-value" in echo["path"], echo
                    assert echo["protocol"] == "HTTP/" + version, echo
                for block in blocks:
                    if "span kind     : Internal" in block:
                        attrs = block.split("  attributes    :")[1].split("  events")[0]
                        assert not re.search(r"(?m)^\t[^:\n]+:", attrs), block
                print("PASS: method=%s backend_protocol=%s credentials=%s all_stages=%s" %
                      (method, version, path.startswith("/creds/"), report_all), flush=True)
    finally:
        config_file.write_text(original, encoding="utf-8")
        docker("cp", str(config_file), CONTAINER + ":" + CONFIG)
        docker("restart", CONTAINER)
