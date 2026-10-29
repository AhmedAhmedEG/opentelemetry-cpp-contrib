#!/usr/bin/env python3
"""Check response trace IDs independently of capture, sampling and propagation.

Usage: python response_trace_id_test.py [container-name]
Uses the AlmaLinux Apache fixture and restores its configuration afterward.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import uuid

CONTAINER = sys.argv[1] if len(sys.argv) > 1 else "otel-apache-rhel8-test"
CONFIG = "/etc/httpd/conf.d/opentelemetry.conf"


def docker(*args):
    return subprocess.check_output(["docker", *args], encoding="utf-8", stderr=subprocess.STDOUT)


original = docker("exec", CONTAINER, "cat", CONFIG)
overrides = ("apachemoduleotelpropagator", "apachemodulecapture", "apachemoduletraceroutes",
             "apachemoduleotelsampler", "apachemodulereportallinstrumentedmodules")
base = "\n".join(line for line in original.splitlines() if not line.lower().startswith(overrides))
base += """
ApacheModuleOtelPropagatorType w3c
ApacheModuleOtelPropagatorExtract OFF
ApacheModuleTraceRoutes /
ApacheModuleReportAllInstrumentedModules OFF
# Simulate values supplied by an upstream response or mod_headers.
Header set X-Trace-ID upstream-success
Header always add X-Trace-ID upstream-always
"""

with tempfile.TemporaryDirectory(prefix="otel-response-trace-") as directory:
    config = Path(directory) / "opentelemetry.conf"
    try:
        for inject, capture, sampled in [(True, True, True), (False, False, True), (False, False, False)]:
            text = base + "\nApacheModuleOtelPropagatorInject " + ("ON" if inject else "OFF")
            text += "\nApacheModuleOtelSampler " + ("always_on" if sampled else "always_off")
            for direction in ["RequestHeaders", "ResponseHeaders", "QueryParameters"]:
                text += "\nApacheModuleCapture" + direction + (" ON" if capture else " OFF")
            config.write_text(text + "\n", encoding="utf-8")
            docker("cp", str(config), CONTAINER + ":" + CONFIG)
            docker("exec", CONTAINER, "httpd", "-t")
            docker("restart", CONTAINER)
            since = docker("exec", CONTAINER, "date", "-u", "+%Y-%m-%dT%H:%M:%S.%NZ").strip()
            for method, path, incoming in [("GET", "/services/index.html", True),
                                           ("HEAD", "/services/index.html", True),
                                           ("GET", "/services/missing-response", False)]:
                trace_id = uuid.uuid4().hex
                args = ["exec", CONTAINER, "curl", "-sS", "-D", "-", "-o", "/dev/null"]
                if method == "HEAD":
                    args += ["-I"]
                if incoming:
                    args += ["-H", "traceparent: 00-" + trace_id + "-2222222222222222-01"]
                response = docker(*args, "http://localhost" + path)
                values = re.findall(r"(?im)^x-trace-id: ([^\r\n]+)", response)
                assert len(values) == 1 and re.fullmatch(r"[0-9a-f]{32}", values[0]), response
                if incoming:
                    assert values == [trace_id], response
                if sampled:
                    for _ in range(50):
                        logs = docker("logs", "--since", since, CONTAINER)
                        blocks = logs.split("{\n  name")[1:]
                        if any("trace_id      : " + values[0] in block and "span kind     : Server" in block
                               for block in blocks):
                            break
                        time.sleep(0.1)
                    else:
                        raise AssertionError("Response ID has no matching Apache server span: " + response)
            print("PASS: response trace ID for GET/HEAD/404, spoofed values replaced; inject=%s capture=%s sampled=%s"
                  % (inject, capture, sampled), flush=True)
    finally:
        config.write_text(original, encoding="utf-8")
        docker("cp", str(config), CONTAINER + ":" + CONFIG)
        docker("restart", CONTAINER)
