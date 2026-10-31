/* Test-only recoverable failures; never included in the shipped SDK. */
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_filter.h"
#include <string.h>

static apr_status_t recover_backend(ap_filter_t *f, apr_bucket_brigade *bb)
{
    ap_filter_t *next = f->next;
    if (!strncmp(f->r->uri, "/recovered/", 11)) {
        f->r->status = HTTP_OK;
        f->r->status_line = NULL;
    }
    ap_remove_output_filter(f);
    return ap_pass_brigade(next, bb);
}

static void insert_recovery(request_rec *r)
{
    if (!strncmp(r->uri, "/recovered/", 11))
        ap_add_output_filter("STATUS_FIXTURE", NULL, r, r->connection);
}

static int internal_failure(request_rec *r)
{
    if (strcmp(r->uri, "/internal-failure")) return DECLINED;
    /* An INTERNAL handler fails, then recovers before sending HTTP 200. */
    r->status = HTTP_INTERNAL_SERVER_ERROR;
    ap_run_insert_filter(r);
    r->status = HTTP_OK;
    r->status_line = NULL;
    ap_set_content_type(r, "text/plain");
    ap_rputs("Recovered internal failure\n", r);
    return OK;
}

static void register_hooks(apr_pool_t *p)
{
    ap_register_output_filter("STATUS_FIXTURE", recover_backend, NULL, AP_FTYPE_PROTOCOL - 2);
    ap_hook_insert_filter(insert_recovery, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(internal_failure, NULL, NULL, APR_HOOK_FIRST);
}

module AP_MODULE_DECLARE_DATA status_fixture_module = {
    STANDARD20_MODULE_STUFF, NULL, NULL, NULL, NULL, NULL, register_hooks
};
