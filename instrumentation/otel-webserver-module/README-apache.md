# Apache module quick reference

Extract to `/opt`, run `sudo /opt/opentelemetry-webserver-sdk/install.sh`, edit
`conf/opentelemetry_module.conf` (collector + service identity), then add this
to your JBCS Apache configuration:

```apache
Include /opt/opentelemetry-webserver-sdk/conf/opentelemetry_module.conf
```

Run your JBCS `httpd -t`, then restart its service. Load the module only once.
The supplied configuration enables the additions, traces all routes, uses
**W3C only**, and separates **Authorization only** from request headers JSON.

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
| `OtelPropagatorInject` | `ON` / `OFF`: write the current client-span context to outgoing propagation headers. Default `ON`. |
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
| `TraceRoutes` | Comma-separated exact paths/subpath prefixes; omit for all routes. |
| `SegmentType` | `FIRST`, `LAST`, `CUSTOM`; bypassed by Apache's METHOD/path name. |
| `SegmentParameter` | Segment count or indexes; also bypassed for Apache root names. |
| `MaskCookie` | `ON` / `OFF`; legacy setting, currently unused. |
| `CookieMatchPattern` | Cookie pattern; currently unused. |
| `MaskSmUser` | `ON` / `OFF`; currently unused. |
| `Delimiter` | URL-segment separator; currently unused. |
| `Segment` | Comma-separated URL segments; currently unused. |
| `Matchfilter` | URL filter type; currently unused. |
| `Matchpattern` | URL filter pattern; currently unused. |

Header lists match case-insensitively; quote lists containing spaces. Propagation
uses the configured standard propagator headers only; no custom parent-span
header is generated. Listed
headers move out of the combined JSON into `http.request.headers.<lowercase>`
or `http.response.headers.<lowercase>`. Repeated values use a JSON array string;
missing headers produce no attribute. Lists replace inherited lists. Other
request headers also retain the legacy `http.request.header.<original-name>`
attributes. Header capture OFF leaves trace propagation active.

Root names are `METHOD /full/path`, excluding the query. HTTP status, URL,
route, and available upstream attributes need no extra directive. Upstream
status/timing values depend on Apache request notes/environment supplied by
other modules; they are not measured by this plugin.
