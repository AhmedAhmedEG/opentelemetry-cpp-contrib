#!/usr/bin/env python3
"""Exercise configured route names with GET, POST, PUT and PATCH on real Apache."""
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
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        data = json.dumps({'method': self.command, 'path': self.path, 'body': body,
                           'traceparent': self.headers.get('traceparent')})
        self.send_response(200)
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


def value(block, key):
    return re.search(r"(?m)^\s*" + re.escape(key) + r"\s*:\s*(\S+)", block).group(1)


def attributes(block):
    return set(re.findall(r"(?m)^\t([^:\n]+):", block.split("  attributes    :")[1].split("  events")[0]))


original = docker("exec", CONTAINER, "cat", CONFIG)
base = "\n".join(line for line in original.splitlines() if not line.lower().startswith((
    "apachemoduleotelpropagator", "apachemoduletraceroutes", "apachemodulereportallinstrumentedmodules")))
base += """
ApacheModuleOtelPropagatorType w3c
ApacheModuleOtelPropagatorExtract OFF
ApacheModuleOtelPropagatorInject ON
ApacheModuleReportAllInstrumentedModules ON
ProxyPass /services/ http://127.0.0.1:8765/dxl/
ProxyPass /first/ http://127.0.0.1:8765/general/
ProxyPass /first/special/ http://127.0.0.1:8765/unreachable/
ProxyPass /second/special/ http://127.0.0.1:8765/specific/
ProxyPass /second/ http://127.0.0.1:8765/general/
ProxyPassMatch ^/rx/([0-9]+)/(.*)$ http://127.0.0.1:8765/r/$1/$2
<Location /location/>
    ProxyPass http://127.0.0.1:8765/loc/
</Location>
<Location /location/special/>
    ProxyPass http://127.0.0.1:8765/chosen/
</Location>
<Location /explicit/>
    ProxyPass http://127.0.0.1:8765/explicit/
    ApacheModuleServerSpanTarget /configured/{id}
    ApacheModuleClientSpanTarget configured-backend/items/{id}
</Location>
<Location /explicit/automatic/>
    ApacheModuleServerSpanTarget ""
    ApacheModuleClientSpanTarget ""
</Location>
RewriteEngine ON
RewriteRule ^/rewrite/(.*)$ http://127.0.0.1:8765/rewritten/$1 [P,L]
<Location /rewrite/>
    ApacheModuleServerSpanTarget /rewrite/{path}
    ApacheModuleClientSpanTarget rewrite-backend/rewritten/{path}
</Location>
<VirtualHost *:80>
    ServerName localhost
    ServerAlias inherited.example
    RewriteEngine ON
    RewriteOptions Inherit
</VirtualHost>
<VirtualHost *:80>
    ServerName isolated.example
    ProxyPassInherit Off
    ProxyPass /vhost/ http://127.0.0.1:8765/tenant/
</VirtualHost>
"""
server_attributes = {"http.request.method", "http.response.status_code", "server.address", "server.port",
                     "url.path", "url.query", "url.scheme", "network.protocol.version", "client.address",
                     "network.peer.address", "network.peer.port", "user_agent.original",
                     "http.request.headers", "http.response.headers", "http.request.header.authorization"}
client_attributes = {"http.request.method", "http.response.status_code", "server.address", "server.port",
                     "url.full", "network.protocol.version", "network.peer.address", "network.peer.port"}

with tempfile.TemporaryDirectory(prefix="otel-route-names-") as directory:
    config = Path(directory) / "opentelemetry.conf"
    backend = Path(directory) / "backend.py"
    backend.write_text(BACKEND, encoding="utf-8")
    docker("cp", str(backend), CONTAINER + ":/tmp/otel-span-names-backend.py")
    try:
        config.write_text(base, encoding="utf-8")
        docker("cp", str(config), CONTAINER + ":" + CONFIG)
        docker("exec", CONTAINER, "httpd", "-t")
        docker("restart", CONTAINER)
        docker("exec", "-d", CONTAINER, "python", "/tmp/otel-span-names-backend.py")
        for _ in range(30):
            try:
                docker("exec", CONTAINER, "curl", "-fsS", "http://localhost:8765/")
                break
            except subprocess.CalledProcessError:
                time.sleep(0.1)
        cases = [
            ("/services/", "/services/{path}", "127.0.0.1:8765/dxl/{path}", "/dxl/"),
            ("/location/", "/location/{path}", "127.0.0.1:8765/loc/{path}", "/loc/"),
            ("/location/special/", "/location/special/{path}", "127.0.0.1:8765/chosen/{path}", "/chosen/"),
            ("/first/special/", "/first/{path}", "127.0.0.1:8765/general/{path}", "/general/special/"),
            ("/second/special/", "/second/special/{path}", "127.0.0.1:8765/specific/{path}", "/specific/"),
            ("/rx/123/", "/rx/{param1}/{path}", "127.0.0.1:8765/r/{param1}/{path}", "/r/123/"),
            ("/rx/456/", "/rx/{param1}/{path}", "127.0.0.1:8765/r/{param1}/{path}", "/r/456/"),
            ("/explicit/", "/configured/{id}", "configured-backend/items/{id}", "/explicit/"),
            ("/explicit/automatic/", "", "", "/explicit/automatic/"),
            ("/rewrite/", "/rewrite/{path}", "rewrite-backend/rewritten/{path}", "/rewritten/"),
            ("/services/", "/services/{path}", "127.0.0.1:8765/dxl/{path}", "/dxl/", "inherited.example"),
            ("/vhost/", "/vhost/{path}", "127.0.0.1:8765/tenant/{path}", "/tenant/", "isolated.example"),
        ]
        count = 0
        for case in cases:
            prefix, server_target, client_target, backend_prefix = case[:4]
            host = case[4] if len(case) > 4 else "localhost"
            for method in ["GET", "POST", "PUT", "PATCH"]:
                for item in ["111", "999"]:
                    marker = uuid.uuid4().hex
                    query = "span-names=" + marker
                    since = docker("exec", CONTAINER, "date", "-u", "+%Y-%m-%dT%H:%M:%S.%NZ").strip()
                    args = ["exec", CONTAINER, "curl", "-sS", "-D", "-", "-X", method,
                            "-H", "Authorization: Bearer name-test", "-H", "Host: " + host]
                    body = "payload-" + item if method != "GET" else ""
                    if body:
                        args += ["--data-binary", body]
                    wire = docker(*args, "http://localhost" + prefix + item + "?" + query)
                    headers, response = wire.replace("\r\n", "\n").split("\n\n", 1)
                    echo = json.loads(response)
                    assert echo["method"] == method and echo["body"] == body, echo
                    assert echo["path"] == backend_prefix + item + "?" + query, echo
                    for _ in range(50):
                        logs = docker("logs", "--since", since, CONTAINER)
                        blocks = logs.split("{\n  name")[1:]
                        server = next((b for b in blocks if "span kind     : Server" in b and marker in b), None)
                        clients = [b for b in blocks if "span kind     : Client" in b]
                        if server and len(clients) == 1:
                            break
                        time.sleep(0.1)
                    assert server and len(clients) == 1, logs
                    client = clients[0]
                    expected_server = method + (" " + server_target if server_target else "")
                    expected_client = method + (" " + client_target if client_target else "")
                    assert server.splitlines()[0].split(":", 1)[1].strip() == expected_server, server
                    assert client.splitlines()[0].split(":", 1)[1].strip() == expected_client, client
                    assert attributes(server) == server_attributes, attributes(server)
                    assert attributes(client) == client_attributes, attributes(client)
                    assert value(client, "parent_span_id") == value(server, "span_id"), client
                    assert echo["traceparent"].split("-")[2] == value(client, "span_id"), echo
                    assert re.findall(r"(?im)^x-trace-id: ([0-9a-f]{32})$", headers) == [value(server, "trace_id")]
                    if len(sys.argv) > 2 and prefix == "/services/" and item == "111" and host == "localhost":
                        trace = value(server, "trace_id")
                        same_trace = ["{\n  name" + b for b in blocks if "trace_id      : " + trace in b]
                        Path(sys.argv[2], method.lower() + "-route-trace.log").write_text(
                            wire + "\n\n" + "\n".join(same_trace), encoding="utf-8")
                    count += 1
            print("PASS: " + prefix + " GET/POST/PUT/PATCH; stable names across different IDs; attributes unchanged", flush=True)
        print("PASS: %d requests verified with matching response and upstream trace context" % count, flush=True)
    finally:
        config.write_text(original, encoding="utf-8")
        docker("cp", str(config), CONTAINER + ":" + CONFIG)
        docker("restart", CONTAINER)
