#!/usr/bin/env python3
"""Exercise header separation in a running Docker Apache test container.

Usage: python header_separation_test.py [container-name]
The container must use the osstream exporter with unbuffered stdout.
Its original OpenTelemetry configuration is restored after the checks.
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


def docker(*args):
    return subprocess.check_output(
        ["docker", *args], stderr=subprocess.STDOUT, encoding="utf-8"
    )


def combined(logs, direction):
    match = re.search(
        r"(?m)^\s*http\." + direction + r"\.headers: (\{.*?^\})",
        logs,
        re.DOTALL,
    )
    assert match, "Missing combined " + direction + " headers"
    return {key.lower(): value for key, value in json.loads(match.group(1)).items()}


def attribute(logs, key):
    match = re.search(r"(?m)^\s*" + re.escape(key) + r": (.*)$", logs)
    return match.group(1) if match else None


original = docker("exec", CONTAINER, "cat", CONFIG)
base = "\n".join(
    line for line in original.splitlines()
    if not line.lower().startswith("apachemoduleseparate")
)
fixtures = """
Header always set X-Response-ID response-id
Header always set X-Keep-Response keep-response
Header always add Set-Cookie "a=1; Expires=Tue, 01 Jan 2030 00:00:00 GMT"
Header always add Set-Cookie "b=2"
"""
selection = """
ApacheModuleSeparateRequestHeaders " aUtHoRiZaTiOn, X-Request-ID, x-request-id, X-Missing "
ApacheModuleSeparateResponseHeaders " SET-cookie, x-response-id, X-Missing "
"""


def apply_and_request(config, path="/services/index.html", expected_status="200"):
    since = docker("exec", CONTAINER, "date", "-u", "+%Y-%m-%dT%H:%M:%S.%NZ").strip()
    config_file.write_text(config, encoding="utf-8")
    docker("cp", str(config_file), CONTAINER + ":" + CONFIG)
    docker("restart", CONTAINER)
    docker("exec", CONTAINER, "httpd", "-t")
    marker = uuid.uuid4().hex
    target = path + "?header-test=" + marker
    output = docker(
        "exec", CONTAINER, "curl", "-sS", "-w", "\n%{http_code}\n",
        "-H", "aUtHoRiZaTiOn: Bearer fixture-token",
        "-H", "X-Request-ID: request-id",
        "-H", "X-Keep-Request: keep-request",
        "-H", "traceparent: 00-11111111111111111111111111111111-2222222222222222-01",
        "http://localhost" + target,
    )
    assert output.strip().endswith(expected_status), output
    for _ in range(50):
        time.sleep(0.1)
        logs = docker("logs", "--since", since, CONTAINER)
        if "url.query: header-test=" + marker in logs:
            return logs
    raise AssertionError("Request span was not exported")


def check_selected(logs):
    request = combined(logs, "request")
    response = combined(logs, "response")
    assert "authorization" not in request and "x-request-id" not in request
    assert request["x-keep-request"] == "keep-request"
    assert "set-cookie" not in response and "x-response-id" not in response
    assert response["x-keep-response"] == "keep-response"
    assert attribute(logs, "http.request.header.x-keep-request") is None
    assert attribute(logs, "http.response.header.x-keep-response") is None
    assert attribute(logs, "http.request.header.authorization") == "[Bearer fixture-token]"
    assert attribute(logs, "http.request.header.x-request-id") == "[request-id]"
    assert attribute(logs, "http.response.header.x-response-id") == "[response-id]"
    assert attribute(logs, "http.response.header.set-cookie") == "[a=1; Expires=Tue, 01 Jan 2030 00:00:00 GMT,b=2]"
    assert attribute(logs, "http.request.header.x-missing") is None
    assert attribute(logs, "http.response.header.x-missing") is None
    assert attribute(logs, "http.request.header.X-Request-ID") is None
    assert "trace_id      : 11111111111111111111111111111111" in logs


with tempfile.TemporaryDirectory(prefix="otel-header-test-") as directory:
    config_file = Path(directory) / "opentelemetry.conf"
    try:
        logs = apply_and_request(base + fixtures)
        assert attribute(logs, "http.request.header.authorization") is None
        assert combined(logs, "request")["authorization"] == "Bearer fixture-token"
        assert attribute(logs, "http.request.header.x-request-id") is None
        assert attribute(logs, "http.response.header.set-cookie") is None
        assert attribute(logs, "http.response.header.x-response-id") is None
        assert "set-cookie" in combined(logs, "response")
        assert not re.search(r"(?m)^\s*http\.(request|response)\.header\.[^:]+:", logs)
        print("PASS: both lists default to empty; Authorization stays in combined JSON")

        logs = apply_and_request(base + fixtures + """
ApacheModuleSeparateRequestHeaders X-Request-ID
ApacheModuleSeparateResponseHeaders X-Response-ID
""")
        assert combined(logs, "request")["authorization"] == "Bearer fixture-token"
        assert attribute(logs, "http.request.header.authorization") is None
        assert attribute(logs, "http.request.header.x-request-id") == "[request-id]"
        assert "set-cookie" in combined(logs, "response")
        assert attribute(logs, "http.response.header.x-response-id") == "[response-id]"
        selected = set(re.findall(r"(?m)^\s*(http\.(?:request|response)\.header\.[^:]+):", logs))
        assert selected == {"http.request.header.x-request-id", "http.response.header.x-response-id"}, selected
        print("PASS: explicit lists replace defaults independently")

        check_selected(apply_and_request(base + fixtures + selection))
        print("PASS: request/response separation, case, whitespace, duplicates, missing headers, repeated cookies")

        check_selected(apply_and_request(base + fixtures + selection, "/services/missing-headers", "404"))
        print("PASS: selected response headers on error responses")

        location = """
<Location /services/index.html>
    ApacheModuleSeparateRequestHeaders ""
    ApacheModuleSeparateResponseHeaders ""
</Location>
"""
        logs = apply_and_request(base + fixtures + selection + location)
        assert combined(logs, "request")["authorization"] == "Bearer fixture-token"
        assert combined(logs, "request")["x-request-id"] == "request-id"
        assert "set-cookie" in combined(logs, "response")
        assert attribute(logs, "http.request.header.authorization") is None
        assert attribute(logs, "http.response.header.set-cookie") is None
        print("PASS: child configuration overrides and explicit empty lists")

        location = """
<Location /services/index.html>
    ApacheModuleCaptureQueryParameters ON
</Location>
"""
        check_selected(apply_and_request(base + fixtures + selection + location))
        print("PASS: child configuration inherits header lists")

        disabled = (base + fixtures + selection).replace(
            "ApacheModuleCaptureRequestHeaders ON", "ApacheModuleCaptureRequestHeaders OFF"
        ).replace("ApacheModuleCaptureResponseHeaders ON", "ApacheModuleCaptureResponseHeaders OFF")
        logs = apply_and_request(disabled)
        assert not re.search(r"(?m)^\s*http\.(request|response)\.headers?(?:\.[^:]+)?:", logs)
        assert "trace_id      : 11111111111111111111111111111111" in logs
        print("PASS: capture OFF suppresses header attributes and retains propagation")
    finally:
        config_file.write_text(original, encoding="utf-8")
        docker("cp", str(config_file), CONTAINER + ":" + CONFIG)
        docker("restart", CONTAINER)
        docker("exec", CONTAINER, "httpd", "-t")
