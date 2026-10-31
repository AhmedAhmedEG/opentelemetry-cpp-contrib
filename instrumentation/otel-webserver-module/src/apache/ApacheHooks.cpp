/*
* Copyright 2022, OpenTelemetry Authors. 
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
// file header
#include "ApacheHooks.h"
// system headers
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_set>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
// apache headers
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_optional_hooks.h>
#include <apr_uri.h>
#include <arpa/inet.h>
#include <httpd.h>
#include <http_config.h> // ap_get_module_config and others
#include <http_core.h>
#include <http_log.h> // ap_log_error and others
#include <http_protocol.h> // ap_hook_get_pre_read_request and others
#include <http_request.h> // ap_hook_get_create_request and others
// module headers
#include <api/WSAgent.h>
#include <api/Payload.h>
#include "ApacheTracing.h"
#include "ApacheConfig.h"

#include "sdkwrapper/SdkConstants.h"

std::string ApacheHooks::m_aggregatorCommDir = "";
bool ApacheHooks::m_reportAllStages = false;
const std::initializer_list<const char*> ApacheHooks::httpHeaders = {
    "baggage",
    "traceparent",
    "tracestate"
};
const char* ApacheHooks::OTEL_CONFIG_KEY = "otel_configuration";
const char* ApacheHooks::OTEL_CORRELATION_HEADER_KEY = "singularityheader";
const char* ApacheHooks::OTEL_INTERACTION_HANDLE_KEY = "otel_interaction_handle_key";
const char* ApacheHooks::OTEL_REQ_HANDLE_KEY = "otel_req_handle_key";
const char* OTEL_OUTPUT_FILTER_NAME = "OTEL_EUM_AUTOINJECT";

otel::core::WSAgent wsAgent; // global variable for interface between Hooks and Core Logic

using namespace otel::core::sdkwrapper;

namespace {
const char* const OTEL_PROPAGATION_KEY = "otel_authoritative_propagation";
const char* const OTEL_RESPONSE_TRACE_KEY = "otel_response_trace_id";
const char* const OTEL_INCOMING_HOST_KEY = "otel_incoming_host";
const char* const OTEL_BACKEND_HOST_KEY = "otel_backend_host";
const char* const OTEL_UPSTREAM_URL_KEY = "otel_upstream_url";
const char* const OTEL_HTTP_CLIENT_KEY = "otel_http_client";
const char* const OTEL_SERVER_NAME_KEY = "otel_server_span_name";
int (*originalProxyHandler)(request_rec*) = NULL;

std::string httpMethod(const char* method)
{
    const std::string original = method ? method : "";
    const char* configured = std::getenv("OTEL_INSTRUMENTATION_HTTP_KNOWN_METHODS");
    const std::string known = configured ? configured : "CONNECT,DELETE,GET,HEAD,OPTIONS,PATCH,POST,PUT,TRACE,QUERY";
    std::istringstream list(known);
    std::string candidate;
    while (std::getline(list, candidate, ','))
    {
        const auto start = candidate.find_first_not_of(" \t");
        const auto end = candidate.find_last_not_of(" \t");
        if (start != std::string::npos && candidate.substr(start, end - start + 1) == original)
            return original;
    }
    return "_OTHER";
}

std::string httpSpanName(const char* method)
{
    const std::string normalized = httpMethod(method);
    return normalized == "_OTHER" ? "HTTP" : normalized;
}

std::string pathTarget(std::string prefix)
{
    if (prefix.empty()) prefix = "/";
    if (prefix.back() != '/') prefix += '/';
    return prefix + "{path}";
}

std::string regexTarget(const std::string& pattern, std::vector<std::string>& groups)
{
    std::string text = pattern;
    if (!text.empty() && text.front() == '^') text.erase(0, 1);
    if (!text.empty() && text.back() == '$') text.pop_back();
    std::string result;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '(')
        {
            const size_t end = text.find(')', i + 1);
            if (end == std::string::npos) return "match:" + pattern;
            const std::string group = text.substr(i + 1, end - i - 1);
            if (group != ".*" && group != ".+" && group != "[^/]+" &&
                group != "[0-9]+" && group != "\\d+") return "match:" + pattern;
            const std::string label = group == ".*" || group == ".+" ? "{path}" :
                "{param" + std::to_string(groups.size() + 1) + "}";
            groups.push_back(label);
            result += label;
            i = end;
        }
        else if (text[i] == '\\' && i + 1 < text.size() &&
            (text[i + 1] == '.' || text[i + 1] == '/' || text[i + 1] == '-')) result += text[++i];
        else if (strchr("[]?+*|{}^$\\", text[i])) return "match:" + pattern;
        else result += text[i];
    }
    return result;
}

bool sameProxyTarget(request_rec* request, const std::string& expected, const std::string& actual)
{
    apr_uri_t a, b;
    if (apr_uri_parse(request->pool, expected.c_str(), &a) != APR_SUCCESS ||
        apr_uri_parse(request->pool, actual.c_str(), &b) != APR_SUCCESS ||
        !a.scheme || !b.scheme || !a.hostname || !b.hostname) return false;
    const apr_port_t ap = a.port ? a.port : apr_uri_port_of_scheme(a.scheme);
    const apr_port_t bp = b.port ? b.port : apr_uri_port_of_scheme(b.scheme);
    return !apr_strnatcasecmp(a.scheme, b.scheme) && !apr_strnatcasecmp(a.hostname, b.hostname) &&
        ap == bp && !strcmp(a.path ? a.path : "/", b.path ? b.path : "/");
}

const OtelProxyNameRule* namingRule(request_rec* request, bool requireResolvedTarget)
{
    auto cfg = ApacheConfigHandlers::getConfig(request);
    const auto rules = cfg ? cfg->getProxyNameRules() : NULL;
    if (!rules) return NULL;
    const auto entries = reinterpret_cast<const OtelProxyNameRule*>(rules->elts);
    const std::string path = request->uri ? request->uri : "";
    const std::string actual = request->filename && !strncmp(request->filename, "proxy:", 6)
        ? request->filename + 6 : "";
    for (int i = 0; i < rules->nelts; ++i)
    {
        const auto& rule = entries[i];
        if (!rule.supported) continue;
        std::string expected;
        if (rule.regex)
        {
            ap_regmatch_t matches[AP_MAX_REG_MATCH];
            if (!rule.compiled || ap_regexec(rule.compiled, path.c_str(), AP_MAX_REG_MATCH, matches, 0)) continue;
            const char* expanded = ap_pregsub(request->pool, rule.target, path.c_str(), AP_MAX_REG_MATCH, matches);
            if (!expanded) return NULL;
            expected = expanded;
        }
        else
        {
            const std::string prefix(rule.prefix);
            if (path.compare(0, prefix.size(), prefix)) continue;
            expected = std::string(rule.target) + path.substr(prefix.size());
        }
        if (!strcmp(rule.target, "!")) return NULL;
        if (!actual.empty())
        {
            if (sameProxyTarget(request, expected, actual)) return &rule;
            // A different mapping or rewrite won. Never label its expanded
            // URL using a merely matching, but inactive, prefix.
            continue;
        }
        if (!requireResolvedTarget && rule.location) return &rule;
    }
    return NULL;
}

std::string upstreamUrl(request_rec* request);

std::string routingSpanName(request_rec* request, bool client, bool resolved = false)
{
    const std::string method = httpSpanName(request->method);
    auto cfg = ApacheConfigHandlers::getConfig(request);
    const char* explicitTarget = cfg ? (client ? cfg->getClientSpanTarget() : cfg->getServerSpanTarget()) : NULL;
    if (explicitTarget) return *explicitTarget ? method + " " + explicitTarget : method;
    const auto rule = namingRule(request, resolved || client);
    if (!rule) return method;
    std::vector<std::string> groups;
    const std::string serverTarget = rule->regex ? regexTarget(rule->prefix, groups) : pathTarget(rule->prefix);
    if (!client) return method + " " + serverTarget;
    apr_uri_t uri;
    if (apr_uri_parse(request->pool, rule->target, &uri) != APR_SUCCESS || !uri.hostname ||
        strpbrk(uri.hostname, "$%{}")) return method;
    std::string path = uri.path ? uri.path : "/";
    if (rule->regex)
    {
        std::string converted;
        for (size_t i = 0; i < path.size(); ++i)
        {
            if ((path[i] == '$' || path[i] == '\\') && i + 1 < path.size() &&
                path[i + 1] >= '1' && path[i + 1] <= '9')
            {
                const unsigned int index = path[++i] - '1';
                converted += index < groups.size() ? groups[index] : "{param" + std::to_string(index + 1) + "}";
            }
            else converted += path[i];
        }
        path = converted;
    }
    else path = pathTarget(path);
    // Share the logical hostname selection used by server.address/url.full.
    // Keep the configured path template: the expanded request URL contains IDs.
    // Ports remain in server.port/url.full rather than cluttering the span name.
    const std::string target = upstreamUrl(request);
    apr_uri_t logical;
    const char* hostname = uri.hostname;
    if (apr_uri_parse(request->pool, target.c_str(), &logical) == APR_SUCCESS && logical.hostname)
        hostname = logical.hostname;
    const std::string label = strchr(hostname, ':') ? "[" + std::string(hostname) + "]" : hostname;
    return method + " " + label + path;
}

std::string sanitizedQuery(const std::string& query)
{
    static const std::unordered_set<std::string> sensitive = {
        "X-Amz-Signature", "X-Amz-Credential", "X-Amz-Security-Token", "sig", "X-Goog-Signature"};
    std::string result;
    size_t start = 0;
    while (start < query.size())
    {
        const size_t end = query.find('&', start);
        const std::string parameter = query.substr(start, end == std::string::npos ? end : end - start);
        const size_t equals = parameter.find('=');
        const std::string name = parameter.substr(0, equals);
        result += sensitive.count(name) && equals != std::string::npos
            ? name + "=REDACTED" : parameter;
        if (end == std::string::npos) break;
        result += '&';
        start = end + 1;
    }
    return result;
}

std::string sanitizedUrl(std::string url)
{
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos)
    {
        const size_t end = url.find_first_of("/?#", scheme + 3);
        const size_t credentials = url.find('@', scheme + 3);
        if (credentials != std::string::npos && (end == std::string::npos || credentials < end))
            url.replace(scheme + 3, credentials - scheme - 3, "REDACTED:REDACTED");
    }
    const size_t question = url.find('?');
    if (question != std::string::npos)
    {
        const size_t fragment = url.find('#', question);
        url.replace(question + 1, fragment == std::string::npos ? std::string::npos : fragment - question - 1,
            sanitizedQuery(url.substr(question + 1, fragment == std::string::npos ? fragment : fragment - question - 1)));
    }
    return url;
}

int captureProxyConnection(request_rec* request, request_rec* backendResponse)
{
    if (!backendResponse || !backendResponse->connection) return DECLINED;
#ifdef APLOG_USE_MODULE
    const char* address = backendResponse->connection->client_ip;
    const apr_sockaddr_t* peer = backendResponse->connection->client_addr;
#else
    const char* address = backendResponse->connection->remote_ip;
    const apr_sockaddr_t* peer = backendResponse->connection->remote_addr;
#endif
    if (address) apr_table_set(request->notes, "otel_backend_peer", address);
    if (peer) apr_table_set(request->notes, "otel_backend_port", apr_itoa(request->pool, peer->port));
    return DECLINED;
}

void* requestPoolValue(request_rec* request, const char* key)
{
    void* stored = NULL;
    for (request_rec* owner = request; owner && !stored; owner = owner->prev)
        apr_pool_userdata_get(&stored, key, owner->pool);
    if (!stored && request->main)
        apr_pool_userdata_get(&stored, key, request->main->pool);
    return stored;
}

int captureBackendHost(request_rec* request)
{
    // Current mod_proxy versions remove Host from their temporary header table
    // before proxy fixups. Save it after mod_headers' regular fixups instead.
    const char* host = apr_table_get(request->headers_in, "Host");
    if (host && requestPoolValue(request, OTEL_INCOMING_HOST_KEY))
        apr_pool_userdata_set(apr_pstrdup(request->pool, host), OTEL_BACKEND_HOST_KEY,
            apr_pool_cleanup_null, request->pool);
    return DECLINED;
}

bool isIpAddress(const char* host)
{
    unsigned char address[sizeof(struct in6_addr)];
    return host && (inet_pton(AF_INET, host, address) == 1 ||
        inet_pton(AF_INET6, host, address) == 1);
}

std::string upstreamUrl(request_rec* request)
{
    if (!request->filename || strncmp(request->filename, "proxy:", 6)) return "";
    const std::string target(request->filename + 6);
    apr_uri_t uri;
    if (apr_uri_parse(request->pool, target.c_str(), &uri) != APR_SUCCESS ||
        !uri.scheme || !isIpAddress(uri.hostname)) return target;

    // A RequestHeader Host override identifies the logical backend when the
    // proxy connects to an IP. Do not substitute the caller's original Host,
    // reverse-resolve an arbitrary IP, or change the actual proxy request.
    const char* incoming = static_cast<const char*>(requestPoolValue(request, OTEL_INCOMING_HOST_KEY));
    const char* host = apr_table_get(request->headers_in, "Host");
    if (!host) host = static_cast<const char*>(requestPoolValue(request, OTEL_BACKEND_HOST_KEY));
    if (!incoming || !host || !*host || !apr_strnatcasecmp(incoming, host) ||
        strpbrk(host, "/?#@ \t\r\n")) return target;
    apr_uri_t logical;
    const std::string authority = std::string(uri.scheme) + "://" + host + "/";
    if (apr_uri_parse(request->pool, authority.c_str(), &logical) != APR_SUCCESS ||
        !logical.hostname || isIpAddress(logical.hostname)) return target;
    std::string backendAuthority(host);
    if (!logical.port_str && uri.port_str) backendAuthority += std::string(":") + uri.port_str;
    const size_t start = target.find("://");
    if (start == std::string::npos) return target;
    const size_t end = target.find_first_of("/?#", start + 3);
    return target.substr(0, start + 3) + backendAuthority +
        (end == std::string::npos ? "" : target.substr(end));
}

void clientRequestAttributes(request_rec* request,
    std::unordered_map<std::string, std::string>& attributes,
    std::unordered_map<std::string, int64_t>& numbers)
{
    const char* captured = static_cast<const char*>(requestPoolValue(request, OTEL_UPSTREAM_URL_KEY));
    const std::string target = captured ? captured : upstreamUrl(request);
    if (!target.empty()) attributes["url.full"] = sanitizedUrl(target);
    attributes["http.request.method"] = httpMethod(request->method);
    if (attributes["http.request.method"] == "_OTHER" && request->method)
        attributes["http.request.method_original"] = request->method;
    apr_uri_t uri;
    if (apr_uri_parse(request->pool, target.c_str(), &uri) == APR_SUCCESS && uri.hostname)
    {
        attributes["server.address"] = uri.hostname;
        const apr_port_t port = uri.port ? uri.port : apr_uri_port_of_scheme(uri.scheme);
        if (port) numbers["server.port"] = port;
    }
}

void setResponseTraceId(request_rec* request)
{
    void* stored = NULL;
    for (request_rec* owner = request; owner && !stored; owner = owner->prev)
        apr_pool_userdata_get(&stored, OTEL_RESPONSE_TRACE_KEY, owner->pool);
    if (!stored && request->main)
        apr_pool_userdata_get(&stored, OTEL_RESPONSE_TRACE_KEY, request->main->pool);
    if (!stored) return; // Disabled/excluded requests have no module trace.
    apr_table_unset(request->headers_out, "x-trace-id");
    apr_table_set(request->err_headers_out, "x-trace-id", static_cast<const char*>(stored));
}

void clearTraceHeaders(request_rec* request)
{
    for (const char* header : {"traceparent", "tracestate", "b3",
            "X-B3-TraceId", "X-B3-SpanId", "X-B3-Sampled", "X-B3-Flags",
            "X-B3-ParentSpanId", "Parent_Span_Id"})
        apr_table_unset(request->headers_in, header);
}
}

void ApacheHooks::registerHooks(apr_pool_t *p)
{
    ap_hook_post_config(otel_post_config, NULL, NULL, APR_HOOK_REALLY_LAST + 2);
    // mod_proxy calls this optional hook immediately before serializing
    // HTTP(S) request headers. No hard dependency on mod_proxy is introduced.
    apr_optional_hook_add("fixups", reinterpret_cast<void (*)(void)>(otel_proxy_fixups),
        NULL, NULL, APR_HOOK_REALLY_LAST + 2);
    apr_optional_hook_add("create_req", reinterpret_cast<void (*)(void)>(captureProxyConnection),
        NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_fixups(captureBackendHost, NULL, NULL, APR_HOOK_REALLY_LAST + 2);
    // Place a hook that executes when a child process is spawned
    // (commonly used for initializing modules after the server has forked)
    ap_hook_child_init(otel_child_init, NULL, NULL, APR_HOOK_MIDDLE);

    //------------------------------------------------------------------------------
    // The -2 and +2 below is a trick to ensure that our hooks are before and after
    // the majority of the modules within a stage. This is not in the official
    // apache documentation. There is no guarantee that we will instrument a module
    // if it also does this +2/-2 trick.
    //------------------------------------------------------------------------------

    // request_begin and request_end hooks
    ap_hook_header_parser(ApacheHooks::otel_hook_header_parser_begin, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_log_transaction(ApacheHooks::otel_hook_log_transaction_end, NULL, NULL, APR_HOOK_REALLY_LAST + 2);

    // Run after content filters (including mod_headers), before HTTP headers
    // are serialized. This also replaces competing values on error responses.
    // The filter gets added into the chain in otel_hook_interaction_end_insert_filter via ap_hook_insert_filter.
    ap_register_output_filter(OTEL_OUTPUT_FILTER_NAME, ApacheHooks::otel_output_filter, NULL,
        static_cast<ap_filter_type>(AP_FTYPE_PROTOCOL - 1));
    ap_hook_insert_filter(ApacheHooks::otel_hook_interaction_end_insert_filter, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_insert_error_filter(ApacheHooks::otel_hook_interaction_end_insert_filter, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);

    // TODO: For the time being put in a hook at the very beginning of every stage we are instrumenting. This is a
    // catch all to stop a interaction. We can be smarter and only put it in when we know that there is a
    // module we care about for a previous stage. This means we need to know the deterministic stage order.
    ap_hook_quick_handler(ApacheHooks::otel_hook_interaction_end_quick_handler, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_access_checker(ApacheHooks::otel_hook_interaction_end, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_check_user_id(ApacheHooks::otel_hook_interaction_end, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_auth_checker(ApacheHooks::otel_hook_interaction_end, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_type_checker(ApacheHooks::otel_hook_interaction_end, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_fixups(ApacheHooks::otel_hook_interaction_end, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_handler(ApacheHooks::otel_hook_interaction_end, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);
    ap_hook_log_transaction(ApacheHooks::otel_hook_interaction_end_log_transaction, NULL, NULL, APR_HOOK_REALLY_FIRST - 2);

    // Stage Hooks
    // TODO: Decide among the following stages at what all we need the modules to be instrumented,
    //       and define the hooks handlers for the same to start an interaction before module callback.
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_header_parser,
            ap_hook_header_parser,
            ApacheHooksForStage::otel_header_parser_hooks,
            ApacheHooksForStage::otel_header_parser_indexes,
            ApacheHooks::otel_hook_interaction_end,
            "header_parser");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_quick_handler,
            ap_hook_quick_handler,
            ApacheHooksForStage::otel_quick_handler_hooks,
            ApacheHooksForStage::otel_quick_handler_indexes,
            ApacheHooks::otel_hook_interaction_end_quick_handler,
            "quick_handler");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_access_checker,
            ap_hook_access_checker,
            ApacheHooksForStage::otel_access_checker_hooks,
            ApacheHooksForStage::otel_access_checker_indexes,
            ApacheHooks::otel_hook_interaction_end,
            "access_checker");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_check_user_id,
            ap_hook_check_user_id,
            ApacheHooksForStage::otel_check_user_id_hooks,
            ApacheHooksForStage::otel_check_user_id_indexes,
            ApacheHooks::otel_hook_interaction_end,
            "check_user_id");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_auth_checker,
            ap_hook_auth_checker,
            ApacheHooksForStage::otel_auth_checker_hooks,
            ApacheHooksForStage::otel_auth_checker_indexes,
            ApacheHooks::otel_hook_interaction_end,
            "auth_checker");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_type_checker,
            ap_hook_type_checker,
            ApacheHooksForStage::otel_type_checker_hooks,
            ApacheHooksForStage::otel_type_checker_indexes,
            ApacheHooks::otel_hook_interaction_end,
            "type_checker");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_fixups,
            ap_hook_fixups,
            ApacheHooksForStage::otel_fixups_hooks,
            ApacheHooksForStage::otel_fixups_indexes,
            ApacheHooks::otel_hook_interaction_end,
            "fixups");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_insert_filter,
            ap_hook_insert_filter,
            ApacheHooksForStage::otel_insert_filter_hooks,
            ApacheHooksForStage::otel_insert_filter_indexes,
            ApacheHooks::otel_hook_interaction_end_insert_filter,
            "insert_filter");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_handler,
            ap_hook_handler,
            ApacheHooksForStage::otel_handler_hooks,
            ApacheHooksForStage::otel_handler_indexes,
            ApacheHooks::otel_hook_interaction_end_handler,
            "handler");
    ApacheHooksForStage::insertHooksForStage(
            p,
            ap_hook_get_log_transaction,
            ap_hook_log_transaction,
            ApacheHooksForStage::otel_log_transaction_hooks,
            ApacheHooksForStage::otel_log_transaction_indexes,
            ApacheHooks::otel_hook_interaction_end,
            "log_transaction");
}

int ApacheHooks::otel_post_config(apr_pool_t*, apr_pool_t*, apr_pool_t*, server_rec*)
{
    // Replace the callback itself. Separate before/after hook entries cannot
    // reliably bracket a RUN_FIRST handler, especially with equal priorities.
    const apr_array_header_t* hooks = ap_hook_get_handler();
    if (!hooks) return OK;
    auto entries = reinterpret_cast<ap_LINK_handler_t*>(hooks->elts);
    for (int i = 0; i < hooks->nelts; ++i)
        if (entries[i].szName && !strcmp(entries[i].szName, "mod_proxy.c") &&
            entries[i].pFunc != otel_proxy_handler)
        {
            originalProxyHandler = entries[i].pFunc;
            entries[i].pFunc = otel_proxy_handler;
        }
    return OK;
}

int ApacheHooks::otel_proxy_handler(request_rec* request)
{
    if (!originalProxyHandler) return DECLINED;
    const std::string target = upstreamUrl(request);
    auto handle = static_cast<OTEL_SDK_HANDLE_REQ>(const_cast<char*>(apr_table_get(request->notes, OTEL_REQ_HANDLE_KEY)));
    if (!handle || (target.compare(0, 7, "http://") && target.compare(0, 8, "https://")))
        return originalProxyHandler(request);
    otel_stopInteraction(request, true, false);
    otel::core::InteractionPayload payload("mod_proxy.c", "handler", false);
    payload.modern_http_attributes = true;
    payload.http_client = true;
    payload.span_name = routingSpanName(request, true, true);
    const std::string serverName = routingSpanName(request, false, true);
    apr_pool_userdata_set(apr_pstrdup(request->pool, serverName.c_str()), OTEL_SERVER_NAME_KEY,
        apr_pool_cleanup_null, request->pool);
    apr_pool_userdata_set(apr_pstrdup(request->pool, target.c_str()), OTEL_UPSTREAM_URL_KEY,
        apr_pool_cleanup_null, request->pool);
    for (const char* note : {"proxy-status", "proxy_timedout", "otel_backend_peer",
            "otel_backend_port", "otel_backend_protocol", "otel_proxy_result"})
        apr_table_unset(request->notes, note);
    clientRequestAttributes(request, payload.attributes, payload.numeric_attributes);
    std::unordered_map<std::string, std::string> propagation;
    const auto started = wsAgent.startInteraction(handle, &payload, propagation);
    if (OTEL_ISFAIL(started)) return originalProxyHandler(request);
    apr_table_setn(request->notes, OTEL_HTTP_CLIENT_KEY, "1");
    if (!propagation.empty()) otel_payload_decorator(request, propagation);
    const int result = originalProxyHandler(request);
    apr_table_set(request->notes, "otel_proxy_result", apr_itoa(request->pool, result));
    apr_table_setn(request->notes, "otel_proxy_completing", "1");
    otel_stopInteraction(request, true, false);
    apr_table_unset(request->notes, "otel_proxy_completing");
    return result;
}

apr_status_t ApacheHooks::otel_output_filter(ap_filter_t* f, apr_bucket_brigade* bb)
{
    setResponseTraceId(f->r);
    if (APR_BRIGADE_EMPTY(bb))
    {
        // "An output filter should never pass an empty brigade down the filter chain."
        // https://httpd.apache.org/docs/trunk/developer/output-filters.html
        return APR_SUCCESS;
    }

    /*
        Perform the task for output filter particularly of EUM Auto-injection.
    */

    // Remove ourselves from the chain and pass everything down
    ap_remove_output_filter(f);
    return ap_pass_brigade(f->next, bb);
}

// end the interaction
void ApacheHooks::otel_stopInteraction(request_rec *r, bool isAlwaysRunStage, bool ignoreBackend)
{
    if (r && apr_table_get(r->notes, OTEL_HTTP_CLIENT_KEY) &&
        !apr_table_get(r->notes, "otel_proxy_completing")) return;
    if ((!isAlwaysRunStage && !m_reportAllStages) || !r || !r->notes || !ap_is_initial_req(r))
    {
        return;
    }
    OTEL_SDK_HANDLE_REQ reqHandlePtr = (OTEL_SDK_HANDLE_REQ)apr_table_get(r->notes, OTEL_REQ_HANDLE_KEY);
    if (reqHandlePtr)
    {
        // TODO: Work on backend naming and type
        std::string backendName;
        std::string backendType = "HTTP"; // temporary
        long errorCode = r->status;
        std::ostringstream oss("");

        if (otel_requestHasErrors(r))
        {
            oss << "HTTP ERROR CODE:" << r->status;
        }

        std::unique_ptr<otel::core::EndInteractionPayload> payload(new
            otel::core::EndInteractionPayload(backendName, backendType, errorCode, oss.str()));
        payload->modern_http_attributes = true;
        payload->http_client = apr_table_get(r->notes, OTEL_HTTP_CLIENT_KEY) != NULL;
        if (payload->http_client)
        {
            clientRequestAttributes(r, payload->attributes, payload->numeric_attributes);
            const char* status = apr_table_get(r->notes, "proxy-status");
            if (status && atoi(status) >= 100 && atoi(status) <= 599)
                payload->http_status_code = atoi(status);
            const char* result = apr_table_get(r->notes, "otel_proxy_result");
            if (apr_table_get(r->notes, "proxy_timedout")) payload->error_type = "timeout";
            else if (!payload->http_status_code && (r->status >= 400 || (result && atoi(result) >= 400)))
                payload->error_type = "proxy_error";
            const char* peer = apr_table_get(r->notes, "otel_backend_peer");
            const char* port = apr_table_get(r->notes, "otel_backend_port");
            const char* protocol = apr_table_get(r->notes, "otel_backend_protocol");
            if (peer) payload->attributes["network.peer.address"] = peer;
            if (port) payload->numeric_attributes["network.peer.port"] = atoi(port);
            if (protocol) payload->attributes["network.protocol.version"] = protocol;
        }
        else
        {
            // Attribute only a new server failure observed during this stage.
            // Existing backend errors must not mark subsequent INTERNAL spans.
            const char* initial = apr_table_get(r->notes, "otel_internal_initial_status");
            if (initial && atoi(initial) < 500 && r->status >= 500)
                payload->error_type = std::to_string(r->status);
        }
        apr_table_unset(r->notes, "otel_internal_initial_status");
        OTEL_SDK_STATUS_CODE res = wsAgent.endInteraction(reqHandlePtr, ignoreBackend, payload.get());
        apr_table_unset(r->notes, OTEL_HTTP_CLIENT_KEY);

        if (OTEL_ISFAIL(res))
        {
            ApacheTracing::writeTrace(r->server, __func__, "result code: %d", res);
        }
    }
}

// stop endpoint interaction if one currently is running
// begin the new interaction
OTEL_SDK_STATUS_CODE ApacheHooks::otel_startInteraction(
        request_rec* r,
        HookContainer::otel_endpoint_indexes endpointIndex,
        bool isAlwaysRunStage,
        bool ignoreBackend)
{
    /*
        Retrieve the stage and module name from endpointIndex and start an interaction to module.
    */
    OTEL_SDK_STATUS_CODE res = OTEL_SUCCESS;
    if (r && apr_table_get(r->notes, OTEL_HTTP_CLIENT_KEY)) return res;
    if ((!isAlwaysRunStage && !m_reportAllStages) || !r || !r->notes || !ap_is_initial_req(r))
    {
        return res;
    }

    const std::string& stage = HookContainer::getInstance().getStage(endpointIndex);
    const std::string& module = HookContainer::getInstance().getModule(endpointIndex);

    OTEL_SDK_HANDLE_REQ reqHandlePtr = (OTEL_SDK_HANDLE_REQ)apr_table_get(r->notes, OTEL_REQ_HANDLE_KEY);
    if (reqHandlePtr)
    {
        // In case a previous "end hook" is never called. End current interaction before starting new one
        otel_stopInteraction(r, isAlwaysRunStage, ignoreBackend);

        bool resolveBackends = false;
        otel_cfg* cfg = ApacheConfigHandlers::getConfig(r);
        if (cfg)
        {
            resolveBackends = cfg->getResolveBackends();
        }

        std::unique_ptr<otel::core::InteractionPayload> payload(new
            otel::core::InteractionPayload(module, stage, resolveBackends));
        payload->modern_http_attributes = true;
        // Create propagationHeaders to be populated in startInteraction.
        std::unordered_map<std::string, std::string> propagationHeaders;
        res = wsAgent.startInteraction(reqHandlePtr, payload.get(), propagationHeaders);

        if (OTEL_ISSUCCESS(res))
        {
            apr_table_set(r->notes, "otel_internal_initial_status", std::to_string(r->status).c_str());
            // remove the singularity header if any
            apr_table_unset(r->headers_in, OTEL_CORRELATION_HEADER_KEY);
            if (!propagationHeaders.empty())
            {
                otel_payload_decorator(r, propagationHeaders);
            }
            ApacheTracing::writeTrace(r->server, module.c_str(), "interaction begin successful");
        }
        else
        {
            ApacheTracing::writeTrace(r->server, module.c_str(), "Error: interaction begin result code: %d", res);
        }
    }
    return res;
}

//-----------------------------------------------------------------------------------------
// Backend Payload Decorator
// Used for following purposes:
//    1) adding the correlation for this backend request to the headers
//-----------------------------------------------------------------------------------------
void ApacheHooks::otel_payload_decorator(request_rec* request, std::unordered_map<std::string, std::string> propagationHeaders)
{
    otel_cfg* cfg = ApacheConfigHandlers::getConfig(request);
    if (cfg && !cfg->getOtelPropagatorInject())
    {
        return;
    }

    // Replace the complete wire context. A freshly generated trace often has
    // no tracestate, so merely setting returned fields leaves stale input
    // state behind. Also remove alternate formats the backend might prefer.
    clearTraceHeaders(request);
    apr_table_t* authoritative = apr_table_make(request->pool, propagationHeaders.size());

    for (auto itr = propagationHeaders.begin(); itr != propagationHeaders.end(); itr++)
    {
        // This is SDK metadata for the legacy NGINX adapter, not an HTTP header.
        if (itr->first == "Parent_Span_Id") continue;
        apr_table_set(authoritative, itr->first.c_str(), itr->second.c_str());
        // put the correlation information into the http headers
        apr_table_set(request->headers_in, itr->first.c_str(), itr->second.c_str());

        ApacheTracing::writeTrace(request->server, __func__,
                "correlation information : \"%s\" : \"%s\"",
                itr->first.c_str(),
                itr->second.c_str());
    }
    apr_pool_userdata_set(authoritative, OTEL_PROPAGATION_KEY,
        apr_pool_cleanup_null, request->pool);
}

int ApacheHooks::otel_proxy_fixups(request_rec* request)
{
    const std::string target = upstreamUrl(request);
    const char* incomingHost = static_cast<const char*>(requestPoolValue(request, OTEL_INCOMING_HOST_KEY));
    const char* configuredHost = apr_table_get(request->headers_in, "Host");
    if (!configuredHost)
        configuredHost = static_cast<const char*>(requestPoolValue(request, OTEL_BACKEND_HOST_KEY));
    ApacheTracing::writeTrace(request->server, __func__,
        "proxy target identity: incoming_host=%s configured_host=%s recorded_url=%s",
        incomingHost ? incomingHost : "", configuredHost ? configuredHost : "", target.c_str());
    if (!target.empty())
    {
        const char* value = apr_pstrdup(request->pool, target.c_str());
        apr_pool_userdata_set(value, OTEL_UPSTREAM_URL_KEY, apr_pool_cleanup_null, request->pool);
        // The root's log hook can run on the original request after a redirect.
        for (request_rec* owner = request->prev; owner; owner = owner->prev)
            apr_pool_userdata_set(apr_pstrdup(owner->pool, target.c_str()), OTEL_UPSTREAM_URL_KEY,
                apr_pool_cleanup_null, owner->pool);
    }
    // mod_proxy_http has constructed the outgoing HTTP/1.x request line by
    // this point; never copy the unrelated inbound request protocol.
    if (apr_table_get(request->notes, OTEL_HTTP_CLIENT_KEY))
        apr_table_setn(request->notes, "otel_backend_protocol",
            apr_table_get(request->subprocess_env, "force-proxy-request-1.0") ? "1.0" : "1.1");
    otel_cfg* cfg = ApacheConfigHandlers::getConfig(request);
    if (cfg && !cfg->getOtelPropagatorInject()) return DECLINED;

    void* stored = NULL;
    for (request_rec* owner = request; owner && !stored; owner = owner->prev)
        apr_pool_userdata_get(&stored, OTEL_PROPAGATION_KEY, owner->pool);
    if (!stored && request->main)
        apr_pool_userdata_get(&stored, OTEL_PROPAGATION_KEY, request->main->pool);
    if (!stored) return DECLINED; // No traced interaction: preserve pass-through.

    const apr_table_t* authoritative = static_cast<const apr_table_t*>(stored);
    const char* before = apr_table_get(request->headers_in, "traceparent");
    const char* expected = apr_table_get(authoritative, "traceparent");
    if ((before || expected) && (!before || !expected || strcmp(before, expected)))
        ApacheTracing::writeTrace(request->server, __func__,
            "restoring changed proxy traceparent: before=%s expected=%s",
            before ? before : "", expected ? expected : "");
    clearTraceHeaders(request);
    const apr_array_header_t* entries = apr_table_elts(authoritative);
    const apr_table_entry_t* table = reinterpret_cast<const apr_table_entry_t*>(entries->elts);
    for (int i = 0; i < entries->nelts; ++i)
        if (table[i].key && table[i].val)
            apr_table_set(request->headers_in, table[i].key, table[i].val);

    const char* parent = apr_table_get(request->headers_in, "traceparent");
    const char* state = apr_table_get(request->headers_in, "tracestate");
    ApacheTracing::writeTrace(request->server, __func__,
        "proxy egress context: target=%s traceparent=%s tracestate=%s",
        request->filename ? request->filename : "",
        parent ? parent : "", state ? state : "");
    return OK;
}

// Utility routine for initializing the SDK based on configuration parameters
bool ApacheHooks::initialize_opentelemetry(const request_rec *r)
{
    // Serialize the complete bootstrap, including dependencies, context
    // registration and process-config publication. WSAgent::init's own lock
    // covers only part of this sequence; concurrent first requests can
    // otherwise fail initialization and skip tracing/injection.
    static std::mutex initializationMutex;
    std::lock_guard<std::mutex> initializationLock(initializationMutex);
    // check to see if we have already been initialized by getting user data from the process pool
    void* config_data = NULL;
    apr_pool_userdata_get(&config_data, OTEL_CONFIG_KEY, r->server->process->pool);

    if (config_data != NULL)
    {
        ApacheTracing::writeTrace(r->server, __func__, "config retrieved from process memory pool");
        return true;
    }

    // we don't have a config stored in the process memory pool
    // try to initialize and store a config in the process memory pool
    ApacheTracing::writeTrace(r->server, __func__, "no config stored in the process memory pool");

    otel_cfg* our_config = ApacheConfigHandlers::getConfig(r);
    if (our_config == NULL)
    {
        ApacheTracing::writeTrace(r->server, __func__, "config was NULL");
        return false;
    }
    // log config
    ApacheConfigHandlers::traceConfig(r, our_config);

    // Check to see if the agent is enabled
    if (our_config->getOtelEnabled() == 1)
    {
        // Intialize the SDK with our configuration information
        OTEL_SDK_STATUS_CODE res = OTEL_SUCCESS;

        wsAgent.initDependency();

        // ENV RECORDS SIZE TO INCLUDE THE LOG PATH AND THE AGGREGATOR DIRECTORY
        // Update the CONFIG_COUNT in apr_pcalloc if we add another parameter to the input array!
        
        OTEL_SDK_ENV_RECORD* env_config =
                (OTEL_SDK_ENV_RECORD*) apr_pcalloc(r->pool, CONFIG_COUNT * sizeof(OTEL_SDK_ENV_RECORD));

        int ix = 0;

        // Otel Exporter Type
        env_config[ix].name = OTEL_SDK_ENV_OTEL_EXPORTER_TYPE;
        env_config[ix].value = our_config->getOtelExporterType();
        ++ix;

        // Otel Exporter Endpoint
        env_config[ix].name = OTEL_SDK_ENV_OTEL_EXPORTER_ENDPOINT;
        env_config[ix].value = our_config->getOtelExporterEndpoint();
        ++ix;

        // Otel SSL Enabled
        env_config[ix].name = OTEL_SDK_ENV_OTEL_SSL_ENABLED;
        env_config[ix].value = our_config->getOtelSslEnabled() == 1 ? "1" : "0";
        ++ix;

        // Otel Certificate Path
        env_config[ix].name = OTEL_SDK_ENV_OTEL_SSL_CERTIFICATE_PATH;
        env_config[ix].value = our_config->getOtelSslCertificatePath();
        ++ix;

        // sdk libaray name
        env_config[ix].name = OTEL_SDK_ENV_OTEL_LIBRARY_NAME;
        env_config[ix].value = "Apache";
        ++ix;

        // Otel Processor Type
        env_config[ix].name = OTEL_SDK_ENV_OTEL_PROCESSOR_TYPE;
        env_config[ix].value = our_config->getOtelProcessorType();
        ++ix;

        // Otel Sampler Type
        env_config[ix].name = OTEL_SDK_ENV_OTEL_SAMPLER_TYPE;
        env_config[ix].value = our_config->getOtelSamplerType();
        ++ix;

        env_config[ix].name = OTEL_SDK_ENV_OTEL_PROPAGATOR_TYPE;
        env_config[ix].value = our_config->getOtelPropagatorType();
        ++ix;

        // Service Namespace
        env_config[ix].name = OTEL_SDK_ENV_SERVICE_NAMESPACE;
        env_config[ix].value = our_config->getServiceNamespace();
        ++ix;

        // Service Name
        env_config[ix].name = OTEL_SDK_ENV_SERVICE_NAME;
        env_config[ix].value = our_config->getServiceName();
        ++ix;

        // Service Instance ID
        env_config[ix].name = OTEL_SDK_ENV_SERVICE_INSTANCE_ID;
        env_config[ix].value = our_config->getServiceInstanceId();
        ++ix;

        // Otel Max Queue Size
        env_config[ix].name = OTEL_SDK_ENV_MAX_QUEUE_SIZE;
        env_config[ix].value = our_config->getOtelMaxQueueSize();
        ++ix;

        // Otel Scheduled Delay
        env_config[ix].name = OTEL_SDK_ENV_SCHEDULED_DELAY;
        env_config[ix].value = our_config->getOtelScheduledDelay();
        ++ix;

        // Otel Max Export Batch Size
        env_config[ix].name = OTEL_SDK_ENV_EXPORT_BATCH_SIZE;
        env_config[ix].value = our_config->getOtelMaxExportBatchSize();
        ++ix;

        // Otel Export Timeout
        env_config[ix].name = OTEL_SDK_ENV_EXPORT_TIMEOUT;
        env_config[ix].value = our_config->getOtelExportTimeout();
        ++ix;

        // Segment Type
        env_config[ix].name = OTEL_SDK_ENV_SEGMENT_TYPE;
        env_config[ix].value = our_config->getSegmentType();
        ++ix;

        // Segment Parameter
        env_config[ix].name = OTEL_SDK_ENV_SEGMENT_PARAMETER;
        env_config[ix].value = our_config->getSegmentParameter();
        ++ix;

        // Segment Parameter
        env_config[ix].name = OTEL_SDK_ENV_OTEL_EXPORTER_OTLPHEADERS;
        env_config[ix].value = our_config->getOtelExporterOtlpHeaders();
        ++ix;

        // !!!
        // Remember to update the apr_pcalloc call size if we add another parameter to the input array!
        // !!!

        for (auto it = ApacheConfigHandlers::m_webServerContexts.begin(); it != ApacheConfigHandlers::m_webServerContexts.end(); ++it)
        {
            otel::core::WSContextConfig cfg;
            cfg.serviceNamespace = it->second->m_serviceNamespace;
            cfg.serviceName = it->second->m_serviceName;
            cfg.serviceInstanceId = it->second->m_serviceInstanceId;

            wsAgent.addWSContextToCore(it->first.c_str(), &cfg);
            ApacheTracing::writeTrace(r->server, __func__, "Adding Context: %s ServiceNamespace: %s ServiceName: %s ServiceInstanceId: %s",
                it->first.c_str(),
                it->second->m_serviceNamespace.c_str(),
                it->second->m_serviceName.c_str(),
                it->second->m_serviceInstanceId.c_str());
        }

        res = wsAgent.init(env_config, ix);
        if (OTEL_ISSUCCESS(res))
        {
            ApacheTracing::writeTrace(r->server, __func__, "initializing SDK succceeded");

            // Store the config that was selected for all other methods to be able to use
            otel_cfg* process_cfg = ApacheConfigHandlers::getProcessConfig(r);
            apr_pool_userdata_set((const void *)process_cfg, OTEL_CONFIG_KEY, apr_pool_cleanup_null, r->server->process->pool);

            return true;
        }
        else
        {
            ApacheTracing::writeError(r->server, __func__, "SDK Init failed, result code is: %d", res);
            return false;
        }
    }
    else
    {
        ApacheTracing::writeError(r->server, __func__, "agent not enabled");
        return false;
    }
    return false;
}

bool ApacheHooks::otel_requestHasErrors(request_rec* r)
{
    return r->status >= LOWEST_HTTP_ERROR_CODE;
}

namespace {

std::string jsonEscape(const std::string& value)
{
    std::ostringstream escaped;
    escaped << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': escaped << "\\\""; break;
            case '\\': escaped << "\\\\"; break;
            case '\b': escaped << "\\b"; break;
            case '\f': escaped << "\\f"; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    escaped << "\\u"
                            << std::hex << std::uppercase
                            << static_cast<int>(character)
                            << std::dec << std::nouppercase;
                }
                else
                {
                    escaped << character;
                }
        }
    }
    escaped << '"';
    return escaped.str();
}

std::unordered_set<std::string> separateHeaderNames(const char* configured)
{
    std::unordered_set<std::string> names;
    std::istringstream input(configured ? configured : "");
    std::string name;
    while (std::getline(input, name, ','))
    {
        const size_t first = name.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        names.insert(name);
    }
    return names;
}

std::string headersAsJson(const apr_table_t* headers,
    const std::unordered_set<std::string>& excludedHeaders)
{
    std::map<std::string, std::vector<std::string>> values;
    if (headers)
    {
        const apr_array_header_t* entries = apr_table_elts(headers);
        const apr_table_entry_t* table = reinterpret_cast<const apr_table_entry_t*>(entries->elts);
        for (int i = 0; i < entries->nelts; ++i)
        {
            const std::string name = table[i].key ? table[i].key : "";
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (excludedHeaders.count(lowerName))
            {
                continue;
            }
            values[name].push_back(table[i].val ? table[i].val : "");
        }
    }

    std::ostringstream json;
    if (values.empty()) return "{}";
    json << "{\n";
    bool first = true;
    for (const auto& header : values)
    {
        if (!first) json << ",\n";
        first = false;
        json << jsonEscape(header.first) << ": ";
        if (header.second.size() == 1)
        {
            json << jsonEscape(header.second.front());
        }
        else
        {
            json << "[";
            for (size_t i = 0; i < header.second.size(); ++i)
            {
                if (i != 0) json << ",";
                json << jsonEscape(header.second[i]);
            }
            json << "]";
        }
    }
    json << "\n}";
    return json.str();
}

std::unordered_map<std::string, std::vector<std::string>> separatedHeaderAttributes(
    const apr_table_t* headers, const std::unordered_set<std::string>& selected,
    const std::string& prefix)
{
    std::unordered_map<std::string, std::vector<std::string>> attributes;
    if (!headers || selected.empty()) return attributes;
    std::map<std::string, std::vector<std::string>> values;
    const apr_array_header_t* entries = apr_table_elts(headers);
    const apr_table_entry_t* table = reinterpret_cast<const apr_table_entry_t*>(entries->elts);
    for (int i = 0; i < entries->nelts; ++i)
    {
        std::string name = table[i].key ? table[i].key : "";
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (selected.count(name)) values[name].push_back(table[i].val ? table[i].val : "");
    }
    for (const auto& header : values)
        attributes[prefix + header.first] = header.second;
    return attributes;
}

bool routeIsTraceable(const char* path, const char* routes)
{
    if (!routes || !*routes) return true;
    const std::string requestPath = path ? path : "";
    std::string configured(routes);
    size_t start = 0;
    while (start <= configured.size())
    {
        const size_t end = configured.find(',', start);
        std::string route = configured.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        while (!route.empty() && std::isspace(static_cast<unsigned char>(route.front())))
            route.erase(route.begin());
        while (!route.empty() && std::isspace(static_cast<unsigned char>(route.back())))
            route.pop_back();
        while (route.size() > 1 && route.back() == '/') route.pop_back();
        if (!route.empty() &&
            (route == "/" || requestPath == route ||
             (requestPath.size() > route.size() &&
              requestPath.compare(0, route.size(), route) == 0 &&
              requestPath[route.size()] == '/')))
        {
            return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

}

void fillRequestPayload(request_rec* request, otel::core::RequestPayload* payload)
{
    payload->modern_http_attributes = true;
    const char* val;

    otel_cfg* cfg = ApacheConfigHandlers::getConfig(request);
    const bool captureHeaders = !cfg || cfg->getCaptureRequestHeaders();
    const bool extractPropagation = !cfg || cfg->getOtelPropagatorExtract();
    const bool captureQuery = !cfg || cfg->getCaptureQueryParameters();
    const auto separateHeaders = separateHeaderNames(cfg ? cfg->getSeparateRequestHeaders() : "");

    // Both modes parse valid context; OFF reuses only its trace ID as a root.
    if (!extractPropagation)
        payload->set_http_headers(kRootTraceCarrierKey, "1");
    if (request->headers_in)
    {
        const apr_array_header_t* entries = apr_table_elts(request->headers_in);
        const apr_table_entry_t* table = reinterpret_cast<const apr_table_entry_t*>(entries->elts);
        for (int i = 0; i < entries->nelts; ++i)
        {
            const std::string name = table[i].key ? table[i].key : "";
            const std::string value = table[i].val ? table[i].val : "";
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            payload->set_http_headers(lowerName, value);
            // SDK 1.2's B3 propagator looks up these mixed-case keys.
            // Apache HTTP header names are case-insensitive.
            if (lowerName == "x-b3-traceid")
                payload->set_http_headers("X-B3-TraceId", value);
            else if (lowerName == "x-b3-spanid")
                payload->set_http_headers("X-B3-SpanId", value);
            else if (lowerName == "x-b3-sampled")
                payload->set_http_headers("X-B3-Sampled", value);
        }
        if (captureHeaders)
        {
            // Only the configured separation list creates per-header attrs.
            // Leave the legacy request_headers map empty for Apache.
            payload->set_request_attribute("http.request.headers",
                headersAsJson(request->headers_in, separateHeaders));
            for (const auto& attribute : separatedHeaderAttributes(
                request->headers_in, separateHeaders, "http.request.header."))
            {
                payload->header_attributes[attribute.first] = attribute.second;
            }
        }
    }

    // uri
    val = request->parsed_uri.path ? request->parsed_uri.path : " ";
    payload->set_uri(val);

    // request_protocol
    val = request->protocol ? request->protocol : " ";
    payload->set_request_protocol(val);

    // http_post_parameter
    if (!request->method || !request->args || apr_strnatcmp(request->method, "POST"))
    {
        val = " ";
    }
    else val = request->args;
    payload->set_http_post_parameter(val);

    // http_get_parameter
    if(!request->method || !request->args || apr_strnatcmp(request->method, "GET"))
    {
        val = " ";
    }
    else val = request->args;
    payload->set_http_get_parameter(val);
    if (captureQuery)
    {
        if (request->args && *request->args)
            payload->set_request_attribute("url.query", sanitizedQuery(request->args));
    }

    // http_request_method
    val = request->method ? request->method: " ";
    payload->set_http_request_method(val);

    if (request->server)
    {
        payload->set_server_name(request->server->server_hostname);
    }
    payload->set_scheme(ap_run_http_scheme(request));

    if (request->hostname)
    {
        payload->set_host(request->hostname);
    }

    if (request->unparsed_uri)
    {
        payload->set_target(request->unparsed_uri);
    }

    const std::string route = request->parsed_uri.path ? request->parsed_uri.path : "";
    const std::string scheme = ap_run_http_scheme(request) ? ap_run_http_scheme(request) : "";
    const std::string host = request->hostname ? request->hostname : "";
    payload->set_request_attribute("url.path", route);
    payload->set_request_attribute("url.scheme", scheme);
    if (!host.empty()) payload->set_request_attribute("server.address", host);
    const char* authority = apr_table_get(request->headers_in, "Host");
    apr_uri_t incoming;
    const std::string incomingUrl = scheme + "://" + (authority ? authority : host) + "/";
    const bool parsed = apr_uri_parse(request->pool, incomingUrl.c_str(), &incoming) == APR_SUCCESS;
    payload->set_request_attribute("server.port", std::to_string(parsed && incoming.port
        ? incoming.port : ap_get_server_port(request)));
    payload->set_request_attribute("http.request.method", httpMethod(request->method));
    if (httpMethod(request->method) == "_OTHER" && request->method)
        payload->set_request_attribute("http.request.method_original", request->method);
    const std::string protocol = request->protocol ? request->protocol : "";
    const auto slash = protocol.find('/');
    if (slash != std::string::npos)
    {
        const std::string version = protocol.substr(slash + 1);
        payload->set_request_attribute("network.protocol.version",
            version == "2.0" ? "2" : (version == "3.0" ? "3" : version));
    }
#ifdef APLOG_USE_MODULE
    const char* client = request->useragent_ip;
    const char* peer = request->connection->client_ip;
    const apr_sockaddr_t* peerAddress = request->connection->client_addr;
#else
    const char* client = request->connection->remote_ip;
    const char* peer = request->connection->remote_ip;
    const apr_sockaddr_t* peerAddress = request->connection->remote_addr;
#endif
    if (client) payload->set_request_attribute("client.address", client);
    if (peer) payload->set_request_attribute("network.peer.address", peer);
    if (peerAddress) payload->set_request_attribute("network.peer.port", std::to_string(peerAddress->port));
    const char* userAgent = apr_table_get(request->headers_in, "User-Agent");
    if (userAgent) payload->set_request_attribute("user_agent.original", userAgent);

    switch (request->proto_num)
    {  // TODO: consider using ap_get_protocol for other flavors
      case HTTP_PROTO_1000:
        payload->set_flavor(kHTTPFlavor1_0.c_str());
        break;
      case HTTP_PROTO_1001:
        payload->set_flavor(kHTTPFlavor1_1.c_str());
        break;
    }

#ifdef APLOG_USE_MODULE
    payload->set_client_ip(request->useragent_ip);
#else
    if (request->connection)
    {
        payload->set_client_ip(request->connection->remote_ip);
    }

#endif

    payload->set_port(ap_default_port(request));
    const std::string operationName = routingSpanName(request, false);
    payload->set_operation_name(operationName.c_str());
}
// We have to use this hook if we want to use directory level configuration.
// post_read_request is too early in the cycle to get the config parameters for a
// directory configuration.
//
// This is a RUN_ALL hook.
int ApacheHooks::otel_hook_header_parser_begin(request_rec *r)
{
    if (!ap_is_initial_req(r))
    {
        return DECLINED;
    }
    otel_cfg* routeConfig = ApacheConfigHandlers::getConfig(r);
    if (!routeConfig || !routeIsTraceable(
        r->parsed_uri.path, routeConfig->getTraceRoutes()))
    {
        return DECLINED;
    }
    else if (!initialize_opentelemetry(r))
    {
        ApacheTracing::writeTrace(r->server, __func__, "opentelemetry did not get initialized");
        return DECLINED;
    }

    // TODO: Check whether it is static page or not i.e. Excluded Extensions.

    HookContainer::getInstance().traceHooks(r);

    OTEL_SDK_STATUS_CODE res = OTEL_SUCCESS;
    OTEL_SDK_HANDLE_REQ reqHandle = OTEL_SDK_NO_HANDLE;

    const char* wscontext = NULL;
    otel_cfg* cfg = ApacheConfigHandlers::getConfig(r);
    std::string tmpContextName;

    if (cfg)
    {
        tmpContextName = ApacheConfigHandlers::computeContextName(cfg);

        if (!tmpContextName.empty())
        {
            wscontext = tmpContextName.c_str();
        }
    }

    ApacheTracing::writeTrace(r->server, __func__, "%s", (wscontext ? wscontext : "using default context"));

    // Fill the Request payload information
    std::unique_ptr<otel::core::RequestPayload> requestPayload(new otel::core::RequestPayload);
    fillRequestPayload(r, requestPayload.get());

    // Start Request
    res = wsAgent.startRequest(wscontext, requestPayload.get(), &reqHandle);

    // Store the request data into pool
    if (OTEL_ISSUCCESS(res))
    {
        const char* incomingHost = apr_table_get(r->headers_in, "Host");
        if (incomingHost)
            apr_pool_userdata_set(apr_pstrdup(r->pool, incomingHost), OTEL_INCOMING_HOST_KEY,
                apr_pool_cleanup_null, r->pool);
        if (!requestPayload->trace_id.empty())
        {
            apr_pool_userdata_set(apr_pstrdup(r->pool, requestPayload->trace_id.c_str()),
                OTEL_RESPONSE_TRACE_KEY, apr_pool_cleanup_null, r->pool);
            setResponseTraceId(r);
        }
        // Store the Request Handle on the request object
        OTEL_SDK_HANDLE_REQ reqHandleValue = (OTEL_SDK_HANDLE_REQ)apr_palloc(r->pool, sizeof(OTEL_SDK_HANDLE_REQ));

        if (reqHandleValue)
        {
            reqHandleValue = reqHandle;
            apr_table_setn(r->notes, OTEL_REQ_HANDLE_KEY, (const char*)reqHandleValue);
        }

        ApacheTracing::writeTrace(r->server, __func__, "request begin successful");
    }
    else if (res == OTEL_STATUS(cfg_channel_uninitialized) || res == OTEL_STATUS(bt_detection_disabled))
    {
        ApacheTracing::writeTrace(r->server, __func__, "request begin detection disabled, result code: %d", res);
    }
    else
    {
        ApacheTracing::writeTrace(r->server, __func__, "request begin error, result code: %d", res);
    }

    return DECLINED;
}

int ApacheHooks::otel_hook_log_transaction_end(request_rec* r)
{
    /*
        End the request and report the associated metrics.
    */

    if (!ap_is_initial_req(r))
    {
        return DECLINED;
    }

    // TODO - Determine whether there were any errors from any other part of this request processing (even prior to our handler)

    otel_cfg* cfg = NULL;
    apr_pool_userdata_get((void**)&cfg, OTEL_CONFIG_KEY, r->server->process->pool);

    if (!cfg)
    {
        ApacheTracing::writeTrace(r->server, __func__, "configuration is NULL");
        return DECLINED;
    }
    else if (!cfg->getOtelEnabled())
    {
        ApacheTracing::writeError(r->server, __func__, "agent is disabled");
        return DECLINED;
    }

    otel_stopInteraction(r);

    OTEL_SDK_HANDLE_REQ reqHandle = (OTEL_SDK_HANDLE_REQ) apr_table_get(r->notes, OTEL_REQ_HANDLE_KEY);

    if (!reqHandle)
    {
        return DECLINED;
    }

    apr_table_unset(r->notes, OTEL_REQ_HANDLE_KEY);

    OTEL_SDK_STATUS_CODE res;
    std::unique_ptr<otel::core::ResponsePayload> responsePayload
        (new otel::core::ResponsePayload);
    responsePayload->status_code = r->status;
    responsePayload->modern_http_attributes = true;
    const char* finalName = static_cast<const char*>(requestPoolValue(r, OTEL_SERVER_NAME_KEY));
    if (finalName) responsePayload->span_name = finalName;
    if (r->connection->aborted) responsePayload->error_type = "connection_aborted";
    otel_cfg* requestConfig = ApacheConfigHandlers::getConfig(r);
    if (!requestConfig || requestConfig->getCaptureResponseHeaders())
    {
        const apr_table_t* responseHeaders = r->headers_out;
        if (r->err_headers_out)
        {
            responseHeaders = apr_table_overlay(r->pool, r->headers_out, r->err_headers_out);
        }
        const auto separateHeaders = separateHeaderNames(
            requestConfig ? requestConfig->getSeparateResponseHeaders() : "");
        responsePayload->response_attributes["http.response.headers"] =
            headersAsJson(responseHeaders, separateHeaders);
        for (const auto& attribute : separatedHeaderAttributes(
            responseHeaders, separateHeaders, "http.response.header."))
        {
            responsePayload->header_attributes[attribute.first] = attribute.second;
        }
    }

    if (otel_requestHasErrors(r))
    {
        std::ostringstream oss;
        oss << r->status;
        res = wsAgent.endRequest
            (reqHandle, oss.str().c_str(), responsePayload.get());
    }
    else
    {
        res = wsAgent.endRequest(
            reqHandle, NULL, responsePayload.get());
    }

    if (OTEL_ISSUCCESS(res))
    {
        ApacheTracing::writeTrace(r->server, __func__, "request end successful (HTTP status=%d)", r->status);
    }
    else
    {
        ApacheTracing::writeTrace(r->server, __func__, "request end result code: %d", res);
    }

    return DECLINED;
}

//--------------------------------------------------------------------------
// HANDLER HOOKS FOR STAGES OF APACHE PROCESSING
//
// Now let's declare routines for each of the callback hooks in order.
// (That's the order in which they're listed in the callback list, *not
// the order in which the server calls them!  See the command_rec
// declaration near the bottom of this file.)  Note that these may be
// called for situations that don't relate primarily to our function - in
// other words, the fixup handler shouldn't assume that the request has
// to do with "opentelemetry" stuff.
//
// With the exception of the content handler, all of our routines will be
// called for each request, unless an earlier handler from another module
// aborted the sequence.
//
// There are three types of hooks (see include/ap_config.h):
//
// VOID      : No return code, run all handlers declared by any module
// RUN_FIRST : Run all handlers until one returns something other
//             than DECLINED. Hook runner result is result of last callback
// RUN_ALL   : Run all handlers until one returns something other than OK
//             or DECLINED. The hook runner returns that other value. If
//             all hooks run, the hook runner returns OK.
//
// Handlers that are declared as "int" can return the following:
//
//  OK          Handler accepted the request and did its thing with it.
//  DECLINED    Handler took no action.
//  HTTP_mumble Handler looked at request and found it wanting.
//
// See include/httpd.h for a list of HTTP_mumble status codes.  Handlers
// that are not declared as int return a valid pointer, or NULL if they
// DECLINE to handle their phase for that specific request.  Exceptions, if
// any, are noted with each routine.
//--------------------------------------------------------------------------

apr_status_t ApacheHooks::otel_child_exit(void* data)
{
    server_rec* s = static_cast<server_rec*>(data);
    const char* sname = (s->server_hostname ? s->server_hostname : "");

    void* retdata;
    apr_pool_userdata_get(&retdata, OTEL_CONFIG_KEY, s->process->pool);
    otel_cfg* our_config = static_cast<otel_cfg*>(retdata);

    if (our_config)
    {
        ApacheTracing::writeError(s, __func__, "terminating agent (sname=%s)", sname);
        wsAgent.term();
    }

    return APR_SUCCESS;
}

void ApacheHooks::otel_child_init(apr_pool_t* p, server_rec* s)
{
    ApacheTracing::logStartupTrace(s);
    apr_pool_cleanup_register(p, s, otel_child_exit, otel_child_exit);
}

// Stage Hooks

int ApacheHooksForStage::otel_hook_header_parser1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HEADER_PARSER1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_header_parser2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HEADER_PARSER2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_header_parser3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HEADER_PARSER3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_header_parser4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HEADER_PARSER4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_header_parser5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HEADER_PARSER5);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_quick_handler1(request_rec* r, int i)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_QUICK_HANDLER1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_quick_handler2(request_rec* r, int i)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_QUICK_HANDLER2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_quick_handler3(request_rec* r, int i)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_QUICK_HANDLER3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_quick_handler4(request_rec* r, int i)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_QUICK_HANDLER4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_quick_handler5(request_rec* r, int i)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_QUICK_HANDLER5);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_access_checker1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_access_checker2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_access_checker3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_access_checker4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_access_checker5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER5);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_check_user_id1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_CHECK_USER_ID1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_check_user_id2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_CHECK_USER_ID2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_check_user_id3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_CHECK_USER_ID3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_check_user_id4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_CHECK_USER_ID4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_check_user_id5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_CHECK_USER_ID5);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_auth_checker1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_AUTH_CHECKER1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_auth_checker2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_AUTH_CHECKER2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_auth_checker3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_AUTH_CHECKER3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_auth_checker4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_AUTH_CHECKER4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_auth_checker5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_AUTH_CHECKER5);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_type_checker1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_TYPE_CHECKER1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_type_checker2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_TYPE_CHECKER2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_type_checker3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_TYPE_CHECKER3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_type_checker4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_TYPE_CHECKER4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_type_checker5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_TYPE_CHECKER5);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_fixups1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_FIXUPS1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_fixups2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_FIXUPS2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_fixups3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_FIXUPS3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_fixups4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_FIXUPS4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_fixups5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_FIXUPS5);
    return DECLINED;
}

void ApacheHooksForStage::otel_hook_insert_filter1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_INSERT_FILTER1);
}

void ApacheHooksForStage::otel_hook_insert_filter2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_INSERT_FILTER2);
}


void ApacheHooksForStage::otel_hook_insert_filter3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_INSERT_FILTER3);
}

void ApacheHooksForStage::otel_hook_insert_filter4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_INSERT_FILTER4);
}

void ApacheHooksForStage::otel_hook_insert_filter5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_INSERT_FILTER5);
}

// The HANDLER stage does not always run all the hooks.  After a hook returns "OK", the stage
// will end and the remaining hooks are ignored. We use this behavior to ignore any backend calls
// that do not go out to another tier. Thus, if we hit any of the backend_end hook in HANDLER,
// we ignore that backend call in the SDK.
//
// ASSUMPTION: if a module returns DECLINED, it does not go out to another tier.
int ApacheHooksForStage::otel_hook_handler1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER1,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER2,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER3,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER4,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER5,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler6(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER6,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler7(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER7,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler8(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER8,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler9(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER9,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler10(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER10,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler11(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER11,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler12(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER12,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler13(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER13,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler14(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER14,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler15(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER15,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler16(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER16,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler17(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER17,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler18(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER18,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler19(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER19,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_handler20(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_HANDLER20,
            true,  // always run
            true); // ignore backend if one exists
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_log_transaction1(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION1);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_log_transaction2(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION2);
    return DECLINED;
}

int ApacheHooksForStage::otel_hook_log_transaction3(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION3);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_log_transaction4(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION4);
    return DECLINED;
}
int ApacheHooksForStage::otel_hook_log_transaction5(request_rec* r)
{
    ApacheHooks::otel_startInteraction(
            r,
            HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION5);
    return DECLINED;
}

// These hooks are for stopping interactions after a module
int ApacheHooks::otel_hook_interaction_end(request_rec *r)
{
    otel_stopInteraction(r);
    return DECLINED;
}

// The HANDLER stage does not always run all the hooks.  After a hook returns "OK", the stage
// will end and the remaining hooks are ignored. We use this behavior to ignore any endpoint calls
// that do not go out to another tier. Thus, if we hit any of the interaction_end hook in HANDLER,
// we ignore that endpoint call in the SDK.
//
// ASSUMPTION: if a module returns DECLINED, it does not go out to another tier.
int ApacheHooks::otel_hook_interaction_end_handler(request_rec *r)
{
    otel_stopInteraction(r, true, true); // always run and ignore backend if triggered
    return DECLINED;
}

int ApacheHooks::otel_hook_interaction_end_quick_handler(request_rec *r, int i)
{
    otel_stopInteraction(r);
    return DECLINED;
}

void ApacheHooks::otel_hook_interaction_end_insert_filter(request_rec *r)
{
    // Add our output filter into the chain for this request.
    ap_add_output_filter(OTEL_OUTPUT_FILTER_NAME, NULL, r, r->connection);
    otel_stopInteraction(r);
}

int ApacheHooks::otel_hook_interaction_end_log_transaction(request_rec *r)
{
    otel_stopInteraction(r, true, false); // always run and do not ignore backends
    return DECLINED;
}

template<typename T, typename S>
void ApacheHooksForStage::insertHooksForStage(
        apr_pool_t* p,
        hook_get_t getModules,
        T setHook,
        const std::vector<S> &beginHandlers,
        const std::vector<HookContainer::otel_endpoint_indexes> &indexes,
        S endHandler,
        const std::string& stage)
{
    std::vector<HookInfo> hooksNeeded;
    ExcludedModules::findHookPoints(hooksNeeded, getModules, stage);

    size_t hookCount = 0;

    for (auto it = hooksNeeded.begin(); it != hooksNeeded.end(); it++)
    {
        // The proxy handler is instrumented by its callback wrapper instead.
        if (stage == "handler" && it->module == "mod_proxy.c") continue;
        if (hookCount < beginHandlers.size())
        {
            HookContainer::getInstance().addHook(indexes[hookCount], stage, it->module, it->order);
            const char* const* moduleList = HookContainer::getInstance().getModuleList(indexes[hookCount]);

            setHook(beginHandlers[hookCount], NULL, moduleList, it->order);
            setHook(endHandler, moduleList, NULL, it->order);
            hookCount++;

            ApacheTracing::writeTrace(NULL, __func__,
                    "Instrumentation Hooks added. Stage: %s Module: %s",
                    stage.c_str(), it->module.c_str());
        }
        else
        {
            ApacheTracing::writeTrace(NULL, __func__,
                    "Instrumentation Hooks NOT added. Max hooks for stage. Stage: %s Module: %s",
                    stage.c_str(), it->module.c_str());
        }
    }
}

const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_header_parser_hooks =
        {otel_hook_header_parser1
        ,otel_hook_header_parser2
        ,otel_hook_header_parser3
        ,otel_hook_header_parser4
        ,otel_hook_header_parser5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_header_parser_indexes =
        {HookContainer::OTEL_ENDPOINT_HEADER_PARSER1
        ,HookContainer::OTEL_ENDPOINT_HEADER_PARSER2
        ,HookContainer::OTEL_ENDPOINT_HEADER_PARSER3
        ,HookContainer::OTEL_ENDPOINT_HEADER_PARSER4
        ,HookContainer::OTEL_ENDPOINT_HEADER_PARSER5};
const std::vector<ApacheHooksForStage::quickHandlerHooks> ApacheHooksForStage::otel_quick_handler_hooks =
        {otel_hook_quick_handler1
        ,otel_hook_quick_handler2
        ,otel_hook_quick_handler3
        ,otel_hook_quick_handler4
        ,otel_hook_quick_handler5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_quick_handler_indexes =
        {HookContainer::OTEL_ENDPOINT_QUICK_HANDLER1
        ,HookContainer::OTEL_ENDPOINT_QUICK_HANDLER2
        ,HookContainer::OTEL_ENDPOINT_QUICK_HANDLER3
        ,HookContainer::OTEL_ENDPOINT_QUICK_HANDLER4
        ,HookContainer::OTEL_ENDPOINT_QUICK_HANDLER5};
const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_access_checker_hooks =
        {otel_hook_access_checker1
        ,otel_hook_access_checker2
        ,otel_hook_access_checker3
        ,otel_hook_access_checker4
        ,otel_hook_access_checker5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_access_checker_indexes =
        {HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER1
        ,HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER2
        ,HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER3
        ,HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER4
        ,HookContainer::OTEL_ENDPOINT_ACCESS_CHECKER5};
const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_check_user_id_hooks =
        {otel_hook_check_user_id1
        ,otel_hook_check_user_id2
        ,otel_hook_check_user_id3
        ,otel_hook_check_user_id4
        ,otel_hook_check_user_id5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_check_user_id_indexes =
        {HookContainer::OTEL_ENDPOINT_CHECK_USER_ID1
        ,HookContainer::OTEL_ENDPOINT_CHECK_USER_ID2
        ,HookContainer::OTEL_ENDPOINT_CHECK_USER_ID3
        ,HookContainer::OTEL_ENDPOINT_CHECK_USER_ID4
        ,HookContainer::OTEL_ENDPOINT_CHECK_USER_ID5};
const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_auth_checker_hooks =
        {otel_hook_auth_checker1
        ,otel_hook_auth_checker2
        ,otel_hook_auth_checker3
        ,otel_hook_auth_checker4
        ,otel_hook_auth_checker5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_auth_checker_indexes =
        {HookContainer::OTEL_ENDPOINT_AUTH_CHECKER1
        ,HookContainer::OTEL_ENDPOINT_AUTH_CHECKER2
        ,HookContainer::OTEL_ENDPOINT_AUTH_CHECKER3
        ,HookContainer::OTEL_ENDPOINT_AUTH_CHECKER4
        ,HookContainer::OTEL_ENDPOINT_AUTH_CHECKER5};
const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_type_checker_hooks =
        {otel_hook_type_checker1
        ,otel_hook_type_checker2
        ,otel_hook_type_checker3
        ,otel_hook_type_checker4
        ,otel_hook_type_checker5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_type_checker_indexes =
        {HookContainer::OTEL_ENDPOINT_TYPE_CHECKER1
        ,HookContainer::OTEL_ENDPOINT_TYPE_CHECKER2
        ,HookContainer::OTEL_ENDPOINT_TYPE_CHECKER3
        ,HookContainer::OTEL_ENDPOINT_TYPE_CHECKER4
        ,HookContainer::OTEL_ENDPOINT_TYPE_CHECKER5};
const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_fixups_hooks =
        {otel_hook_fixups1
        ,otel_hook_fixups2
        ,otel_hook_fixups3
        ,otel_hook_fixups4
        ,otel_hook_fixups5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_fixups_indexes =
        {HookContainer::OTEL_ENDPOINT_FIXUPS1
        ,HookContainer::OTEL_ENDPOINT_FIXUPS2
        ,HookContainer::OTEL_ENDPOINT_FIXUPS3
        ,HookContainer::OTEL_ENDPOINT_FIXUPS4
        ,HookContainer::OTEL_ENDPOINT_FIXUPS5};
const std::vector<ApacheHooksForStage::filterHooks> ApacheHooksForStage::otel_insert_filter_hooks =
        {otel_hook_insert_filter1
        ,otel_hook_insert_filter2
        ,otel_hook_insert_filter3
        ,otel_hook_insert_filter4
        ,otel_hook_insert_filter5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_insert_filter_indexes =
        {HookContainer::OTEL_ENDPOINT_INSERT_FILTER1
        ,HookContainer::OTEL_ENDPOINT_INSERT_FILTER2
        ,HookContainer::OTEL_ENDPOINT_INSERT_FILTER3
        ,HookContainer::OTEL_ENDPOINT_INSERT_FILTER4
        ,HookContainer::OTEL_ENDPOINT_INSERT_FILTER5};
const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_handler_hooks =
        {otel_hook_handler1
        ,otel_hook_handler2
        ,otel_hook_handler3
        ,otel_hook_handler4
        ,otel_hook_handler5
        ,otel_hook_handler6
        ,otel_hook_handler7
        ,otel_hook_handler8
        ,otel_hook_handler9
        ,otel_hook_handler10
        ,otel_hook_handler11
        ,otel_hook_handler12
        ,otel_hook_handler13
        ,otel_hook_handler14
        ,otel_hook_handler15
        ,otel_hook_handler16
        ,otel_hook_handler17
        ,otel_hook_handler18
        ,otel_hook_handler19
        ,otel_hook_handler20};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_handler_indexes =
        {HookContainer::OTEL_ENDPOINT_HANDLER1
        ,HookContainer::OTEL_ENDPOINT_HANDLER2
        ,HookContainer::OTEL_ENDPOINT_HANDLER3
        ,HookContainer::OTEL_ENDPOINT_HANDLER4
        ,HookContainer::OTEL_ENDPOINT_HANDLER5
        ,HookContainer::OTEL_ENDPOINT_HANDLER6
        ,HookContainer::OTEL_ENDPOINT_HANDLER7
        ,HookContainer::OTEL_ENDPOINT_HANDLER8
        ,HookContainer::OTEL_ENDPOINT_HANDLER9
        ,HookContainer::OTEL_ENDPOINT_HANDLER10
        ,HookContainer::OTEL_ENDPOINT_HANDLER11
        ,HookContainer::OTEL_ENDPOINT_HANDLER12
        ,HookContainer::OTEL_ENDPOINT_HANDLER13
        ,HookContainer::OTEL_ENDPOINT_HANDLER14
        ,HookContainer::OTEL_ENDPOINT_HANDLER15
        ,HookContainer::OTEL_ENDPOINT_HANDLER16
        ,HookContainer::OTEL_ENDPOINT_HANDLER17
        ,HookContainer::OTEL_ENDPOINT_HANDLER18
        ,HookContainer::OTEL_ENDPOINT_HANDLER19
        ,HookContainer::OTEL_ENDPOINT_HANDLER20};
const std::vector<ApacheHooksForStage::processRequestHooks> ApacheHooksForStage::otel_log_transaction_hooks =
        {otel_hook_log_transaction1
        ,otel_hook_log_transaction2
        ,otel_hook_log_transaction3
        ,otel_hook_log_transaction4
        ,otel_hook_log_transaction5};
const std::vector<HookContainer::otel_endpoint_indexes> ApacheHooksForStage::otel_log_transaction_indexes =
        {HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION1
        ,HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION2
        ,HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION3
        ,HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION4
        ,HookContainer::OTEL_ENDPOINT_LOG_TRANSACTION5};
