/* Test-only fixture: simulate a competing proxy hook overwriting context. */
#include "httpd.h"
#include "http_config.h"
#include "apr_optional_hooks.h"
#include "apr_tables.h"

static int overwrite_context(request_rec *r)
{
    apr_table_set(r->headers_in, "traceparent",
        "00-11111111111111111111111111111111-2222222222222222-01");
    apr_table_set(r->headers_in, "tracestate", "vendor=restored");
    return OK;
}

static void register_hooks(apr_pool_t *p)
{
    apr_optional_hook_add("fixups", (void (*)(void))overwrite_context,
        NULL, NULL, APR_HOOK_MIDDLE);
}

AP_DECLARE_MODULE(late_context_writer) = {
    STANDARD20_MODULE_STUFF,
    NULL, NULL, NULL, NULL, NULL, register_hooks
};
