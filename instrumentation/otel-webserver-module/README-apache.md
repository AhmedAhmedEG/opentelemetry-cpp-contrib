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
**W3C only**, ignores incoming trace context, injects Apache's new context toward
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
| `OtelPropagatorExtract` | `ON` / `OFF`: accept incoming propagation headers as the server-span parent. Default `ON`. |
| `OtelPropagatorInject` | `ON` / `OFF`: write the current client-span context to outgoing propagation headers. Default `ON`; OFF preserves incoming headers. |
| `OtelMaxQueueSize` | Integer; accepted but not applied in this version. |
| `OtelScheduledDelayMillis` | Milliseconds; accepted but not applied. |
| `OtelExportTimeoutMillis` | Milliseconds; accepted but not applied. |
| `OtelMaxExportBatchSize` | Integer; accepted but not applied. |
| `ResolveBackends` | `ON` / `OFF`: resolve instrumented backends. |
| `ReportAllInstrumentedModules` | `ON`: all instrumented stages; `OFF`: handler stages. |
| `TraceAsError` | `ON`: diagnostics at error level; `OFF`: normal trace level. |
| `CaptureRequestHeaders` | `ON` / `OFF`: combined and separated request attributes. |
| `CaptureResponseHeaders` | `ON` / `OFF`: combined and separated response attributes. |
| `CaptureQueryParameters` | `ON` / `OFF`: decoded `http.query.parameters`. |
| `SeparateRequestHeaders` | Comma-separated names; default empty; `""` clears. |
| `SeparateResponseHeaders` | Comma-separated names; default empty; `""` clears. |
| `TraceRoutes` | Comma-separated paths and all descendants; trailing slashes allowed. Omit or use `/` for all routes. |
| `SegmentType` | `FIRST`, `LAST`, `CUSTOM`; bypassed by Apache's METHOD/path name. |
| `SegmentParameter` | Segment count or indexes; also bypassed for Apache root names. |
| `MaskCookie` | `ON` / `OFF`; legacy setting, currently unused. |
| `CookieMatchPattern` | Cookie pattern; currently unused. |
| `MaskSmUser` | `ON` / `OFF`; currently unused. |
| `Delimiter` | URL-segment separator; currently unused. |
| `Segment` | Comma-separated URL segments; currently unused. |
| `Matchfilter` | URL filter type; currently unused. |
| `Matchpattern` | URL filter pattern; currently unused. |

Propagation extraction and injection are independent of header capture and of
each other. Extract OFF starts a new trace; Inject OFF leaves incoming headers
unchanged, so Apache may still forward them to a proxy backend. It does not
strip them. Inject ON replaces incoming W3C/B3 trace-context headers with the
selected propagator's current context. Stale `tracestate` and alternate formats
are removed; ordinary application headers remain. With Extract OFF / Inject ON,
only Apache's new trace context reaches the backend. Neither
switch disables local spans. Both switches accept only ON/OFF and inherit into
directory/location configurations. No custom parent-span header is generated
by Apache.

For a proxy backend such as Kong, outgoing `traceparent` identifies an Apache
client span. The supplied example uses Extract OFF and Inject ON: Apache starts
a new trace and the backend can become its child. With Extract ON instead,
that client span belongs to the trace received by
Apache; the backend should extract it and use its span ID as its parent. The
custom `Parent_Span_Id` field is not part of W3C propagation.
With extraction OFF, Apache's main span and outgoing `traceparent` share the
new trace ID; the outgoing span ID identifies its client span. Captured request
header attributes still show the original incoming `traceparent`, which can
have a different trace ID. Compare the main span's top-level `trace_id`.

Header lists match case-insensitively; quote lists containing spaces. Listed
headers move out of the combined JSON into `http.request.headers.<lowercase>`
or `http.response.headers.<lowercase>`. Repeated values use a JSON array string;
missing headers produce no attribute. Lists replace inherited lists. Only
listed headers get per-header attributes; other captured headers appear solely
in the combined JSON. Apache no longer emits legacy
`http.request.header.<original-name>` attributes. Both lists default to empty.
Header capture OFF leaves trace propagation active.

Root names are `METHOD /full/path`, excluding the query. HTTP status, URL,
route, and available upstream attributes need no extra directive.
`http.upstream.status` uses mod_proxy_http's `proxy-status` note (legacy
`upstream_status` / `proxy_status` notes or environment are fallback sources).
Absent upstream values are omitted: a local response or failed connection
does not invent a backend status. `http.upstream.response_time` is removed;
use span duration. `http.upstream.target` and `http.proxy.target` copy Apache's
actual proxy URL, including its path; the module does not concatenate paths
or collapse intentional double slashes.

When stripping `/api`, match slashes on both sides of your proxy mapping:
`ProxyPass /api/ https://backend.example/` forwards
`/api/items/123` as `/items/123`. A mapping from `/api` to a
backend ending in `/` can create `//` in the actual backend request.
For a server/vhost RewriteRule, use
`RewriteRule ^/api(/.*)$ https://backend.example$1 [P,L]`: the capture
already starts with `/`, so do not add another slash before `$1`.
