#!/usr/bin/env python3
'Test Location HTTPS proxy headers and a test-only competing proxy writer.\n\nUsage: python proxy_egress_test.py [container]\nRequires the standard propagation fixture, mod_ssl and the compiled\nmod_late_context_writer.so installed in /etc/httpd/modules. Restores config.'
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
INCOMING = "00-11111111111111111111111111111111-2222222222222222-01"
BACKEND = r'''
import json
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
class Echo(BaseHTTPRequestHandler):
    def do_GET(self):
        data = json.dumps(list(self.headers.items()) + [('X-Echo-Path', self.path)])
        status = 503 if self.path.startswith('/status/503') else 200
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def log_message(self, *args):
        pass
HTTPServer(('127.0.0.1', 8765), Echo).serve_forever()
'''


def docker(*args):
    return subprocess.check_output(["docker", *args], encoding="utf-8", stderr=subprocess.STDOUT)


def span_records(logs):
    records = []
    for block in logs.split("{\n  name") [1:]:
        values = dict(re.findall(r"(?m)^\s*(trace_id|span_id|parent_span_id|span kind)\s*:\s*(\S+)", block))
        if values:
            records.append(values)
    return records


original = docker("exec", CONTAINER, "cat", CONFIG)
base = "\n".join(line for line in original.splitlines() if not line.lower().startswith((
    "apachemoduleotelpropagator", "apachemoduletraceroutes", "apachemodulereportallinstrumentedmodules")))
base += "\nApacheModuleReportAllInstrumentedModules ON\n"
base += "ProxyPass /services/unavailable/ http://127.0.0.1:8766/\n"
base += "ProxyPass /services/ http://127.0.0.1:8765/\n"
base += "ProxyPass /badjoin http://127.0.0.1:8765/\n"

with tempfile.TemporaryDirectory(prefix="otel-propagation-test-") as directory:
    config_file = Path(directory) / "opentelemetry.conf"
    backend_file = Path(directory) / "otel-header-echo.py"
    backend_file.write_text(BACKEND, encoding="utf-8")
    docker("cp", str(backend_file), CONTAINER + ":/tmp/otel-header-echo.py")

    def apply(extra):
        config_file.write_text(base + extra, encoding="utf-8")
        docker("cp", str(config_file), CONTAINER + ":" + CONFIG)
        docker("exec", CONTAINER, "httpd", "-t")
        docker("restart", CONTAINER)
        docker("exec", "-d", CONTAINER, "python", "/tmp/otel-header-echo.py")
        for _ in range(30):
            try:
                docker("exec", CONTAINER, "curl", "-k", "-fsS", "https://127.0.0.1:8765/")
                return
            except subprocess.CalledProcessError:
                time.sleep(0.1)
        raise AssertionError("Backend failed to start")


    base = '\n'.join(line for line in base.splitlines() if not line.startswith('ProxyPass ')) + '\n'
    docker('exec', CONTAINER, 'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
           '-keyout', '/tmp/otel-backend.key', '-out', '/tmp/otel-backend.crt',
           '-days', '1', '-subj', '/CN=backend.example')
    secure_backend = BACKEND.replace('import json', 'import json, ssl').replace(
        "HTTPServer(('127.0.0.1', 8765), Echo).serve_forever()",
        "server = HTTPServer(('127.0.0.1', 8765), Echo)\nserver.socket = ssl.wrap_socket(server.socket, server_side=True, certfile='/tmp/otel-backend.crt', keyfile='/tmp/otel-backend.key')\nserver.serve_forever()")
    backend_file.write_text(secure_backend)
    docker('cp', str(backend_file), CONTAINER + ':/tmp/otel-header-echo.py')
    routing = """
SSLProxyEngine ON
# Self-signed fixture only; use verified backend certificates in production.
SSLProxyVerify none
SSLProxyCheckPeerName off
SSLProxyCheckPeerCN off
<Location /services/>
    RequestHeader unset Cookie
    RequestHeader set Host "gateway.example"
    ProxyPass https://127.0.0.1:8765/
    ProxyPassReverse https://127.0.0.1:8765/
</Location>
"""
    results = []
    try:
        for competing, inject, preserve_host, backend_host in [
                (False, True, False, 'gateway.example'), (False, True, True, 'gateway.example'),
                (True, True, True, 'gateway.example'), (True, False, True, 'gateway.example'),
                (False, True, True, 'gateway.example:9443')]:
            extra = '\nApacheModuleTraceRoutes /services\nApacheModuleOtelPropagatorType w3c\n'
            extra += 'ApacheModuleOtelPropagatorExtract OFF\nApacheModuleOtelPropagatorInject ' + ('ON' if inject else 'OFF') + '\n'
            if competing:
                extra += 'LoadModule late_context_writer_module modules/mod_late_context_writer.so\n'
            apply(extra + 'ProxyPreserveHost ' + ('ON' if preserve_host else 'OFF') + '\n' + routing.replace('\"gateway.example\"', '\"' + backend_host + '\"'))
            for index in range(5):
                since = docker('exec', CONTAINER, 'date', '-u', '+%Y-%m-%dT%H:%M:%S.%NZ').strip()
                marker = uuid.uuid4().hex
                target = '/services/dxl/poq/stuff?egress=' + marker
                output = docker('exec', CONTAINER, 'curl', '-fsS', '-H', 'traceparent: ' + INCOMING,
                                '-H', 'tracestate: vendor=input', '-H', 'Cookie: fixture=1',
                                '-H', 'X-dynaTrace-RequestState: fixture', 'http://localhost' + target)
                pairs = {key.lower(): value for key, value in json.loads(output)}
                assert 'cookie' not in pairs and pairs['host'] == (backend_host if preserve_host else '127.0.0.1:8765'), pairs
                assert pairs['x-forwarded-host'] == backend_host, pairs
                assert pairs['x-echo-path'].startswith('/dxl/poq/stuff?'), pairs
                for _ in range(50):
                    logs = docker('logs', '--since', since, CONTAINER)
                    if 'url.query: ' + target.split('?', 1)[1] in logs:
                        break
                    time.sleep(0.1)
                expected_url = 'https://' + (backend_host if ':' in backend_host else backend_host + ':8765') + pairs['x-echo-path']
                match = re.search(r'(?m)^\s*url\.full: (.*)$', logs)
                assert match and match.group(1) == expected_url, logs
                spans = span_records(logs)
                root = next(span for span in spans if span.get('span kind') == 'Server')
                wire = pairs['traceparent']
                client = next((span for span in spans if span.get('span_id') == wire.split('-')[2]), None)
                valid = bool(root and client and root['trace_id'] == wire.split('-')[1]
                             and client['parent_span_id'] == root['span_id'] and 'tracestate' not in pairs)
                if not inject:
                    valid = wire == INCOMING and pairs.get('tracestate') == 'vendor=restored'
                result = {'backend_host': backend_host, 'upstream_url': match.group(1), 'competing_writer': competing, 'inject': inject, 'preserve_host': preserve_host, 'request_index': index,
                          'root': root, 'wire': wire, 'tracestate': pairs.get('tracestate'),
                          'client': client, 'valid': valid}
                results.append(result)
                print(json.dumps(result), flush=True)
            error_log = docker('exec', CONTAINER, 'cat', '/var/log/httpd/error_log')
            Path('out/rhel8-x86_64/proxy-egress-error.log').write_text(error_log)
        Path('out/rhel8-x86_64/proxy-egress-results.json').write_text(json.dumps(results, indent=2))
        assert all(result['valid'] for result in results), 'Late header overwrite reached the HTTPS backend'
        print('PASS: Location HTTPS proxy, Host override, Cookie removal, competing writer, and Inject OFF', flush=True)
    finally:
        config_file.write_text(original, encoding='utf-8')
        docker('cp', str(config_file), CONTAINER + ':' + CONFIG)
        docker('restart', CONTAINER)
        docker('exec', CONTAINER, 'httpd', '-t')
