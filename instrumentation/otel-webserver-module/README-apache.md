# Apache module quick reference

Extract to `/opt`, run `sudo /opt/opentelemetry-webserver-sdk/install.sh`, edit
`conf/opentelemetry_module.conf` (collector + service identity), then add this
to your JBCS Apache configuration:

```apache
Include /opt/opentelemetry-webserver-sdk/conf/opentelemetry_module.conf
```

Run your JBCS `httpd -t`, then restart its service. Load the module only once.
The supplied configuration uses generic example service identities, enables
the additions, traces all routes, uses
**W3C only**, starts Apache as a root using any valid incoming trace ID, injects
Apache's own span context toward
the backend, and separates **Authorization only** from request headers JSON.

Every directive below has the prefix **`ApacheModule`**. Example:
`OtelPropagatorType w3c` means `ApacheModuleOtelPropagatorType w3c`.

| Directive suffix | Value / use |
|---|---|
| `Enabled` | `ON` / `OFF`: tracing. |
| `ServiceName` | Service name; required. |
| `ServiceNamespace` | Service namespace; required. |
| `ServiceInstanceId` | Instance identity; required. |
| `WebserverContext` | `namespace name instance`: shorthand for all three identities. |
| `OtelSpanExporter` | `otlp` or `osstream` (console). |
| `OtelExporterEndpoint` | Collector `host:4317`; required. |
| `OtelExporterHeaders` | `key=value,key2=value2`: collector metadata. |
| `OtelSslEnabled` | `ON` / `OFF`: collector TLS. |
| `OtelSslCertificatePath` | PEM CA bundle path; required with TLS ON. |
| `OtelSpanProcessor` | `batch` or `simple`. |
| `OtelSampler` | `always_on` or `always_off`; other samplers are unimplemented. |
| `OtelPropagatorType` | `w3c`, `b3` (multi-header), `w3c,b3`, or `b3,w3c`. |
| `OtelPropagatorExtract` | `ON`: adopt incoming parent. `OFF`: reuse a valid incoming trace ID as a root, with no parent; otherwise generate an ID. Default `ON`. |
| `OtelPropagatorInject` | `ON` / `OFF`: write the current client-span context to outgoing propagation headers. Default `ON`; OFF preserves incoming headers. |
| `OtelMaxQueueSize` | Integer; accepted but not applied in this version. |
| `OtelScheduledDelayMillis` | Milliseconds; accepted but not applied. |
| `OtelExportTimeoutMillis` | Milliseconds; accepted but not applied. |
| `OtelMaxExportBatchSize` | Integer; accepted but not applied. |
| `ResolveBackends` | `ON` / `OFF`: resolve instrumented backends. |
| `ReportAllInstrumentedModules` | `ON`: instrumented INTERNAL stages plus proxy CLIENT; `OFF`: handler stages plus proxy CLIENT. |
| `TraceAsError` | `ON`: diagnostics at error level; `OFF`: normal trace level. |
| `CaptureRequestHeaders` | `ON` / `OFF`: combined and separated request attributes. |
| `CaptureResponseHeaders` | `ON` / `OFF`: combined and separated response attributes. |
| `CaptureQueryParameters` | `ON` / `OFF`: `url.query` (omitted when OFF). |
| `SeparateRequestHeaders` | Comma-separated names; default empty; `""` clears. |
| `SeparateResponseHeaders` | Comma-separated names; default empty; `""` clears. |
| `TraceRoutes` | Comma-separated paths and all descendants; trailing slashes allowed. Omit or use `/` for all routes. |
| `SegmentType` | `FIRST`, `LAST`, `CUSTOM`; bypassed by Apache's method-only HTTP name. |
| `SegmentParameter` | Segment count or indexes; also bypassed for Apache root names. |
| `MaskCookie` | `ON` / `OFF`; legacy setting, currently unused. |
| `CookieMatchPattern` | Cookie pattern; currently unused. |
| `MaskSmUser` | `ON` / `OFF`; currently unused. |
| `Delimiter` | URL-segment separator; currently unused. |
| `Segment` | Comma-separated URL segments; currently unused. |
| `Matchfilter` | URL filter type; currently unused. |
| `Matchpattern` | URL filter pattern; currently unused. |

Propagation extraction and injection are independent of header capture and of
each other. Extract OFF reuses a valid trace ID from the configured propagator
without adopting its parent span, sampling flags or vendor tracestate. With no
valid incoming context it generates an ID normally. Inject OFF leaves incoming headers
unchanged, so Apache may still forward them to a proxy backend. It does not
strip them. Inject ON replaces incoming W3C/B3 trace-context headers with the
selected propagator's current context. Stale `tracestate` and alternate formats
are removed; ordinary application headers remain. With Extract OFF / Inject ON,
only Apache's own span context reaches the backend. Neither
switch disables local spans. Both switches accept only ON/OFF and inherit into
directory/location configurations. No custom parent-span header is generated
by Apache.

For HTTP(S) proxy requests, Apache reasserts the intended client context at
mod_proxy's final header step, after earlier proxy hooks. With `TraceAsError ON`,
the Apache error log includes `proxy egress context` and any changed
`traceparent` it restored. These entries show headers immediately before proxy
serialization; an agent intercepting the network/TLS write can still change
them afterward. Avoid competing APM tools owning the same outgoing context.

For a proxy backend such as Kong, outgoing `traceparent` identifies an Apache
client span. The supplied example uses Extract OFF and Inject ON: Apache starts
a root span with the incoming trace ID (or generates one) and the backend can
become its child. With Extract ON instead,
that client span belongs to the trace received by
Apache; the backend should extract it and use its span ID as its parent. The
custom `Parent_Span_Id` field is not part of W3C propagation.
With extraction OFF, Apache's main span and outgoing `traceparent` share the
trace ID; the outgoing span ID identifies its client span. The Apache root has
`parent_span_id=0000000000000000` and its own new span ID. Captured request
header attributes still show the incoming parent's span ID. Other roots already
exported by Dynatrace can coexist in that trace; this does not replace them or
make Apache the sole trace owner in Grafana.

Every traced response includes exactly one `x-trace-id` containing the current
Apache trace ID, including errors and when injection or header capture is OFF.
An upstream `x-trace-id` is replaced. Disabled/excluded requests have no Apache
trace ID to return.

Header lists match case-insensitively; quote lists containing spaces. Listed
headers move out of combined JSON into standard `http.request.header.<lowercase>`
or `http.response.header.<lowercase>` attributes: one value is a plain string; repeated values use string arrays. This single-value display intentionally differs from the
OpenTelemetry header convention, which specifies arrays for all values.
Missing headers produce no attribute. Lists replace inherited lists; both
lists default to empty. Only listed headers get separate header attributes.
Unselected headers remain in custom `http.request.headers` and
`http.response.headers` JSON strings. Capture OFF suppresses both forms
without disabling propagation or `x-trace-id`.

HTTP span semantics follow the [OpenTelemetry HTTP conventions](https://opentelemetry.io/docs/specs/semconv/http/http-spans/).
Apache's incoming span is SERVER. Its name is the known HTTP method (for
example `GET`), or `HTTP` for an unknown method. Raw paths are attributes,
not span names or invented route templates. `OTEL_INSTRUMENTATION_HTTP_KNOWN_METHODS`
can override the known-method list using comma-separated, case-sensitive names.

The SERVER span records these standard fields when available:

```text
http.request.method
http.request.method_original    # only for an unknown/noncanonical method
http.response.status_code
url.path
url.query                     # raw query string; omitted when capture is OFF
url.scheme
server.address
server.port
network.protocol.version
client.address
network.peer.address
network.peer.port
user_agent.original
error.type                    # HTTP 5xx or an observed connection failure
```

Your custom `http.request.headers` / `http.response.headers` pretty JSON and
explicitly selected `http.request.header.*` / `http.response.header.*` fields
stay on the SERVER span. Capture controls affect these custom/selected fields;
standard User-Agent and network attributes remain independent.

The actual `mod_proxy` HTTP(S) handler is wrapped once as a CLIENT span named
with the HTTP method. This reuses the proxy operation, without an overlapping
extra span. It starts immediately before the actual handler callback and ends
after it returns. Its attributes are:

```text
http.request.method
http.request.method_original    # only when necessary
server.address                # backend hostname/IP
server.port                   # backend port
url.full                      # complete backend URL
http.response.status_code     # only when a backend response was received
network.protocol.version      # outgoing HTTP/1.x request version when observed
network.peer.address          # actual connected backend IP, when exposed
network.peer.port             # actual connected port, when exposed
error.type                    # backend HTTP 4xx/5xx, timeout or proxy_error
```

The CLIENT span is a child of the SERVER span and supplies the outgoing
propagation context, so an instrumented backend can become its child. Backend
status comes from `proxy-status`; Apache-generated failures do not invent a
backend status. HTTP 1xx/2xx/3xx leave span status unset; SERVER 4xx is unset,
CLIENT 4xx is error, and 5xx is error. Other instrumented Apache stages are
INTERNAL spans, with module/stage names and no copied HTTP attributes.
`ReportAllInstrumentedModules OFF` still records the real proxy CLIENT span.
No `http.upstream.*` or `http.proxy.*` aliases are emitted.

Port/status attributes are integers. Query strings are not decoded into JSON.
`url.full` redacts URL credentials. Identifiable `X-Amz-Signature`,
`X-Amz-Credential`, `X-Amz-Security-Token`, `sig` and `X-Goog-Signature` query
values are replaced by `REDACTED` in URL attributes; actual traffic is unchanged.
Custom captured header values retain the requested raw behavior.

For IP proxy targets, a changed `RequestHeader set Host` hostname identifies
the logical backend in the trace. Explicit Host ports win, otherwise the target
port is retained. Configured DNS names stay intact; an IP-only configuration
keeps its IP without guessing via reverse DNS. Connected IP/port are recorded
separately when Apache exposes them. Paths are not joined or normalized.

When stripping `/api`, match slashes on both sides of your proxy mapping:
`ProxyPass /api/ https://backend.example/` forwards
`/api/items/123` as `/items/123`. A mapping from `/api` to a
backend ending in `/` can create `//` in the actual backend request.
For a server/vhost RewriteRule, use
`RewriteRule ^/api(/.*)$ https://backend.example$1 [P,L]`: the capture
already starts with `/`, so do not add another slash before `$1`.
