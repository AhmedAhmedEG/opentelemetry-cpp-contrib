#!/usr/bin/env python3
"""Verify proxy wire headers, span ancestry and scoped routes in the test container.

Usage: python propagation_routes_test.py [container-name]
Requires mod_proxy_http, curl, Python and the unbuffered osstream exporter.
Restores the original configuration; the fixture backend dies on restart.
"""
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
                docker("exec", CONTAINER, "curl", "-fsS", "http://127.0.0.1:8765/")
                return
            except subprocess.CalledProcessError:
                time.sleep(0.1)
        raise AssertionError("Backend failed to start")

    def request(path="/services/echo", headers=(), traced=True):
        since = docker("exec", CONTAINER, "date", "-u", "+%Y-%m-%dT%H:%M:%S.%NZ").strip()
        marker = uuid.uuid4().hex
        target = path + "?propagation-test=" + marker
        args = ["exec", CONTAINER, "curl", "-sS"]
        for header in headers:
            args += ["-H", header]
        response = docker(*args, "http://localhost" + target)
        for _ in range(50 if traced else 5):
            time.sleep(0.1)
            logs = docker("logs", "--since", since, CONTAINER)
            if "http.target: " + target in logs:
                assert traced, "Excluded route was traced: " + path
                return response, span_records(logs), logs
        assert not traced, "No exported span for " + path
        return response, [], logs

    try:
        for extract, inject in [(True, True), (True, False), (False, True), (False, False)]:
            apply("\nApacheModuleTraceRoutes /services\nApacheModuleOtelPropagatorType w3c\n"
                  "ApacheModuleOtelPropagatorExtract " + ("ON" if extract else "OFF") + "\n"
                  "ApacheModuleOtelPropagatorInject " + ("ON" if inject else "OFF") + "\n")
            for incoming in [True, False]:
                response, spans, logs = request(headers=["TrAcEpArEnT: " + INCOMING] if incoming else [])
                pairs = [(key.lower(), value) for key, value in json.loads(response)]
                parents = [value for key, value in pairs if key == "traceparent"]
                assert not any(key == "parent_span_id" for key, _ in pairs), pairs
                root = next(span for span in spans if span["span kind"] == "Server")
                assert re.search(r"(?m)^\s*http\.upstream\.status: 200$", logs), logs
                assert "http.upstream.response_time:" not in logs
                assert root["parent_span_id"] == ("2222222222222222" if extract and incoming else "0000000000000000"), root
                if extract and incoming:
                    assert root["trace_id"] == "11111111111111111111111111111111", root
                else:
                    assert root["trace_id"] != "11111111111111111111111111111111", root
                if inject:
                    assert len(parents) == 1 and re.fullmatch(r"00-[0-9a-f]{32}-[0-9a-f]{16}-01", parents[0]), pairs
                    _, trace_id, span_id, _ = parents[0].split("-")
                    parent = next(span for span in spans if span["span_id"] == span_id)
                    assert parent["trace_id"] == trace_id == root["trace_id"], parent
                    assert parent["span kind"] == "Client" and parent["parent_span_id"] == root["span_id"], parent
                else:
                    assert parents == ([INCOMING] if incoming else []), pairs
            print("PASS: proxy wire parent and extraction/injection " + str((extract, inject)))

        apply("\nApacheModuleTraceRoutes /services\nApacheModuleOtelPropagatorType b3\n")
        _, spans, _ = request(headers=["x-b3-traceid: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                      "X-B3-sPaNiD: bbbbbbbbbbbbbbbb", "x-b3-sampled: 1"])
        root = next(span for span in spans if span["span kind"] == "Server")
        assert root["trace_id"] == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" and root["parent_span_id"] == "bbbbbbbbbbbbbbbb", root
        print("PASS: mixed-case B3 extraction remains supported")

        apply("\nApacheModuleTraceRoutes /services/\nApacheModuleOtelPropagatorType w3c\n")
        for path in ["/services", "/services/", "/services/a/b"]:
            request(path)
        for path in ["/services-other", "/untraced/index.html"]:
            request(path, traced=False)
        print("PASS: trailing-slash scoped routes include all descendants and exclude sibling prefixes")

        apply("\nApacheModuleTraceRoutes /\nApacheModuleOtelPropagatorType w3c\n")
        request("/untraced/index.html")
        print("PASS: root route includes every path")

        apply("\nApacheModuleTraceRoutes /services\nApacheModuleOtelPropagatorType w3c\n"
              "ApacheModuleOtelPropagatorExtract OFF\nApacheModuleOtelPropagatorInject OFF\n"
              "<Location /services/echo>\nApacheModuleOtelPropagatorExtract ON\n"
              "ApacheModuleOtelPropagatorInject ON\n</Location>\n")
        response, spans, _ = request(headers=["traceparent: " + INCOMING])
        root = next(span for span in spans if span["span kind"] == "Server")
        assert root["trace_id"] == "11111111111111111111111111111111"
        assert dict(json.loads(response))["traceparent"] != INCOMING
        print("PASS: location configuration overrides propagation switches")

        for suffix in ["Extract", "Inject"]:
            config_file.write_text(base + "\nApacheModuleOtelPropagator" + suffix + " typo\n", encoding="utf-8")
            docker("cp", str(config_file), CONTAINER + ":" + CONFIG)
            try:
                docker("exec", CONTAINER, "httpd", "-t")
            except subprocess.CalledProcessError as error:
                assert "must be ON or OFF" in error.output
            else:
                raise AssertionError("Invalid propagation switch accepted")
        print("PASS: malformed propagation switches fail configuration validation")

        apply("\nApacheModuleTraceRoutes /\nApacheModuleOtelPropagatorType w3c\n"
              "ApacheModuleOtelPropagatorExtract OFF\nApacheModuleOtelPropagatorInject ON\n")
        response, _, logs = request("/services/dxl/poq/stuff")
        received_path = dict(json.loads(response))["X-Echo-Path"]
        assert received_path.startswith("/dxl/poq/stuff?") and "//" not in received_path, received_path
        for key in ["http.upstream.target", "http.proxy.target"]:
            match = re.search(r"(?m)^\s*" + re.escape(key) + r": (.*)$", logs)
            assert match and match.group(1) == "http://127.0.0.1:8765" + received_path, logs
        print("PASS: stripped /services prefix preserves the exact nested backend URL without adding slashes")

        _, _, logs = request("/services/status/503")
        assert re.search(r"(?m)^\s*http\.upstream\.status: 503$", logs), logs
        assert "http.upstream.response_time:" not in logs
        print("PASS: real upstream 503 response is recorded")

        for path in ["/services/unavailable/echo", "/local-missing"]:
            _, _, logs = request(path)
            assert "http.upstream.status:" not in logs and "http.upstream.response_time:" not in logs, logs
        print("PASS: failed connections and local responses omit nonexistent upstream status")

        response, _, logs = request("/badjoin/dxl/poq/stuff")
        received_path = dict(json.loads(response))["X-Echo-Path"]
        assert received_path.startswith("//dxl/poq/stuff?"), received_path
        assert "http.upstream.target: http://127.0.0.1:8765" + received_path in logs
        print("PASS: mismatched ProxyPass slashes reproduce // on the wire (telemetry faithfully reports it)")
    finally:
        config_file.write_text(original, encoding="utf-8")
        docker("cp", str(config_file), CONTAINER + ":" + CONFIG)
        docker("restart", CONTAINER)
        docker("exec", CONTAINER, "httpd", "-t")
