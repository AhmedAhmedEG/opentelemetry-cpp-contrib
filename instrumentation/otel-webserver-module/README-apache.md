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
