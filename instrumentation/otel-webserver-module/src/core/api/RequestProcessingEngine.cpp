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

#include "api/RequestProcessingEngine.h"
#include "AgentLogger.h"
#include "api/ApiUtils.h"
#include "api/Payload.h"
#include "sdkwrapper/SdkWrapper.h"
#include "sdkwrapper/IScopedSpan.h"
#include "sdkwrapper/SdkConstants.h"
#include <sys/types.h>
#include <unistd.h>
#include <boost/filesystem.hpp>
#include <boost/thread/locks.hpp>
#include <log4cxx/logger.h>
#include <unordered_map>
#include <sstream>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/context/runtime_context.h>


namespace otel {
namespace core {

using namespace sdkwrapper;

constexpr const char* http_request_header = "http.request.header.";
constexpr const char* http_response_header = "http.response.header.";

static void addSelectedHeaders(const std::shared_ptr<IScopedSpan>& span,
    const std::unordered_map<std::string, std::vector<std::string>>& attributes) {
    for (const auto& attribute : attributes) {
        if (attribute.second.size() == 1) {
            span->AddAttribute(attribute.first, attribute.second.front());
            continue;
        }
        std::vector<opentelemetry::nostd::string_view> values;
        for (const auto& value : attribute.second) values.emplace_back(value);
        span->AddAttribute(attribute.first,
            opentelemetry::nostd::span<const opentelemetry::nostd::string_view>(values.data(), values.size()));
    }
}

RequestProcessingEngine::RequestProcessingEngine()
    : mLogger(getLogger(std::string(LogContext::AGENT) + ".RequestProcessingEngine"))
{
}

void RequestProcessingEngine::init(std::shared_ptr<TenantConfig>& config,
    std::shared_ptr<SpanNamer> spanNamer) {
    m_sdkWrapper.reset(new sdkwrapper::SdkWrapper());
    m_sdkWrapper->Init(config);
    m_spanNamer = spanNamer;
}

OTEL_SDK_STATUS_CODE RequestProcessingEngine::startRequest(
    const std::string& wscontext,
    RequestPayload* payload,
    OTEL_SDK_HANDLE_REQ* reqHandle) {
    if (!reqHandle) {
        LOG4CXX_ERROR(mLogger, __FUNCTION__ << " " << OTEL_STATUS(handle_pointer_is_null));
        return OTEL_STATUS(handle_pointer_is_null);
    }

    if (!payload) {
        LOG4CXX_ERROR(mLogger, __FUNCTION__ << " " << OTEL_STATUS(payload_reflector_is_null));
        return OTEL_STATUS(payload_reflector_is_null);
    }

    std::string spanName = (payload->get_operation_name().empty() ? m_spanNamer->getSpanName(payload->get_uri()) : payload->get_operation_name());
    otel::core::sdkwrapper::OtelKeyValueMap keyValueMap;
    if (!payload->modern_http_attributes) {
        keyValueMap[kAttrRequestProtocol] = payload->get_request_protocol();
        keyValueMap[kAttrHTTPServerName] = payload->get_server_name();
        keyValueMap[kAttrHTTPMethod] = payload->get_http_request_method();
        keyValueMap[kAttrHTTPScheme] = payload->get_scheme();
        keyValueMap[kAttrNetHostName] = payload->get_host();
        keyValueMap[kAttrNETHostPort] = payload->get_port();
        keyValueMap[kAttrHTTPTarget] =payload->get_target();
        keyValueMap[kAttrHTTPFlavor] = payload->get_flavor();
        keyValueMap[kAttrHTTPClientIP] = payload->get_client_ip();
        keyValueMap[kAttrHTTPUserAgent] = payload->get_user_agent();
        keyValueMap[kAttrNETPeerPort] = payload->get_peer_port();
    }

    auto& request_headers = payload->get_request_headers();
    for (auto itr = request_headers.begin(); itr != request_headers.end(); itr++) {
        std::string key = std::string(http_request_header) +
                std::string(itr->first);
        keyValueMap[key] = itr->second;
    }
    for (const auto& attribute : payload->get_request_attributes()) {
        if (payload->modern_http_attributes &&
            (attribute.first == "server.port" || attribute.first == "network.peer.port"))
            keyValueMap[attribute.first] = static_cast<int64_t>(std::stol(attribute.second));
        else
            keyValueMap[attribute.first] = attribute.second;
    }

    auto span = m_sdkWrapper->CreateSpan(spanName, sdkwrapper::SpanKind::SERVER, keyValueMap, payload->get_http_headers());
    addSelectedHeaders(span, payload->header_attributes);
    const auto spanContext = opentelemetry::trace::GetSpan(
        opentelemetry::context::RuntimeContext::GetCurrent())->GetContext();
    if (spanContext.IsValid()) {
        char traceId[32];
        spanContext.trace_id().ToLowerBase16(traceId);
        payload->trace_id.assign(traceId, sizeof(traceId));
    }
    
    LOG4CXX_TRACE(mLogger, "Span started for context: [" << wscontext
        <<"] SpanName: " << spanName << ", RequestProtocol: " << payload->get_request_protocol()
        <<" SpanId: " << span.get());
    RequestContext* requestContext = new RequestContext(span);
    requestContext->setContextName(wscontext);

    // Fill the requestHandle
    *reqHandle = (void*)requestContext;

    return OTEL_SUCCESS;
}

OTEL_SDK_STATUS_CODE RequestProcessingEngine::endRequest(
    OTEL_SDK_HANDLE_REQ reqHandle,
    const char* error,
    const ResponsePayload* payload) {

    if (!reqHandle) {
        LOG4CXX_ERROR(mLogger, "Invalid request, can't end request");
        return OTEL_STATUS(fail);
    }

    RequestContext* requestContext = (RequestContext*)reqHandle;
    auto rootSpan = requestContext->rootSpan();

    // End all the active interactions one by one if they aren't closed.
    // Even if we end parent span, child spans might remain active. We need
    // to ensure that all client/internal spans are closed (in case of error in request
    // they might not be explicitely closed)

    while (requestContext->hasActiveInteraction()) {
        auto interactionSpan = requestContext->lastActiveInteraction();
        LOG4CXX_TRACE(mLogger, "Ending Span with id: " << interactionSpan.get());
        interactionSpan->End();
        requestContext->removeInteraction();
    }

    // check for error and set attribute in the scopedSpan.
    if (payload && payload->modern_http_attributes) {
        const bool failed = payload->status_code >= 500 || !payload->error_type.empty() || (!payload->status_code && error);
        rootSpan->SetStatus(failed ? StatusCode::Error : StatusCode::Unset);
    } else if (error) {
        std::stringstream strValue;
        unsigned int errorValue;

        strValue << error;
        strValue >> errorValue;

        std::string errorStatus (kHttpErrorCode + error); // This is status message eg: HTTP ERROR CODE:403

        if (errorValue >= HTTP_ERROR_1XX &&   errorValue < HTTP_ERROR_4XX ) {
            rootSpan->SetStatus(StatusCode::Unset);
        }
        else if (errorValue >= HTTP_ERROR_4XX &&   errorValue < HTTP_ERROR_5XX ) {
            if (rootSpan->GetSpanKind() == SpanKind::SERVER)
                rootSpan->SetStatus(StatusCode::Unset);
            else
                rootSpan->SetStatus(StatusCode::Error, errorStatus);

        } else {
            rootSpan->SetStatus(StatusCode::Error, errorStatus);
        }

        LOG4CXX_TRACE(mLogger, "Setting status as error[" << errorStatus <<"] on root Span");

    } else {
        rootSpan->SetStatus(payload && payload->modern_http_attributes ? StatusCode::Unset : StatusCode::Ok);
    }
    if (payload != nullptr) {
        addSelectedHeaders(rootSpan, payload->header_attributes);
        for (auto itr = payload->response_headers.begin();
            itr != payload->response_headers.end(); itr++) {
            std::string key = std::string(http_response_header) +
                std::string(itr->first);
            rootSpan->AddAttribute(key, itr->second);
        }
        for (const auto& attribute : payload->response_attributes) {
            rootSpan->AddAttribute(attribute.first, attribute.second);
        }

        for (auto itr = payload->otel_attributes.begin(); itr != payload->otel_attributes.end(); itr++) {
            std::string key = std::string(kOtelAttributes) + std::string(itr->first);
            rootSpan->AddAttribute(key, itr->second);
        }

        if (payload->modern_http_attributes) {
            if (payload->status_code)
                rootSpan->AddAttribute("http.response.status_code", static_cast<int64_t>(payload->status_code));
        }
        else
            rootSpan->AddAttribute(kAttrHTTPStatusCode, payload->status_code);
        if (payload->modern_http_attributes && (!payload->error_type.empty() || payload->status_code >= 500))
            rootSpan->AddAttribute("error.type", payload->error_type.empty()
                ? std::to_string(payload->status_code) : payload->error_type);
    }

    LOG4CXX_TRACE(mLogger, "Ending root span with id: " << rootSpan.get());
    rootSpan->End();
    delete requestContext;

    return OTEL_SUCCESS;
}

OTEL_SDK_STATUS_CODE RequestProcessingEngine::startInteraction(
    OTEL_SDK_HANDLE_REQ reqHandle,
    const InteractionPayload* payload,
    std::unordered_map<std::string, std::string>& propagationHeaders) {

    if (!reqHandle) {
        LOG4CXX_ERROR(mLogger, __FUNCTION__ << " " << OTEL_STATUS(handle_pointer_is_null));
        return OTEL_STATUS(handle_pointer_is_null);
    }

    if (!payload) {
        LOG4CXX_ERROR(mLogger, __FUNCTION__ << " " << OTEL_STATUS(payload_reflector_is_null));
        return OTEL_STATUS(payload_reflector_is_null);
    }

    RequestContext* requestContext = (RequestContext*)reqHandle;

    // TODO : Add internal spans for virtual hosts post MVP

    // Create client span for this interaction. And set it in RequestContext object.
    otel::core::sdkwrapper::OtelKeyValueMap keyValueMap;

    // TODO : confirm and update name later
    std::string spanName = payload->moduleName + "_" + payload->phaseName;
    if (!payload->span_name.empty()) spanName = payload->span_name;
    for (const auto& attribute : payload->attributes) keyValueMap[attribute.first] = attribute.second;
    for (const auto& attribute : payload->numeric_attributes) keyValueMap[attribute.first] = attribute.second;
    if (!payload->modern_http_attributes) keyValueMap["interactionType"] = "EXIT_CALL";
    // Legacy metadata still used internally by the NGINX adapter.
    std::string parentSpanId = m_sdkWrapper->ReturnCurrentSpanId();
    const SpanKind kind = payload->modern_http_attributes && !payload->http_client
        ? SpanKind::INTERNAL : SpanKind::CLIENT;
    auto interactionSpan = m_sdkWrapper->CreateSpan(spanName, kind, keyValueMap);
    LOG4CXX_TRACE(mLogger, "Client Span started with SpanName: " << spanName
        << " Span Id: " << interactionSpan.get());
    if (kind == SpanKind::CLIENT) {
        m_sdkWrapper->PopulatePropagationHeaders(propagationHeaders);
        if (!payload->modern_http_attributes) propagationHeaders["Parent_Span_Id"] = parentSpanId;
    }

    // Add the interaction to the request context.
    requestContext->addInteraction(interactionSpan);
    return OTEL_SUCCESS;
}

OTEL_SDK_API OTEL_SDK_STATUS_CODE RequestProcessingEngine::endInteraction(
    OTEL_SDK_HANDLE_REQ reqHandle,
    bool ignoreBackend,
    EndInteractionPayload *payload) {

    if (!reqHandle) {
        LOG4CXX_ERROR(mLogger, __FUNCTION__ << " " << OTEL_STATUS(handle_pointer_is_null));
        return OTEL_STATUS(handle_pointer_is_null);
    }

    if (!payload) {
        LOG4CXX_ERROR(mLogger, __FUNCTION__ << " " << OTEL_STATUS(payload_reflector_is_null));
        return OTEL_STATUS(payload_reflector_is_null);
    }

    // TODO : incorporate ignore backend logic here.

    RequestContext* rContext = (RequestContext*)reqHandle;

    if (!rContext->hasActiveInteraction()) {
        // error : requestContext has no corresponding interaction.
        LOG4CXX_TRACE(mLogger, __FUNCTION__ << " " << OTEL_STATUS(invalid_context));
        return OTEL_STATUS(invalid_context);
    }

    auto interactionSpan = rContext->lastActiveInteraction();

    if (payload->modern_http_attributes) {
        for (const auto& attribute : payload->attributes)
            interactionSpan->AddAttribute(attribute.first, attribute.second);
        for (const auto& attribute : payload->numeric_attributes)
            interactionSpan->AddAttribute(attribute.first, attribute.second);
        const bool failed = payload->http_client &&
            (payload->http_status_code >= 400 || !payload->error_type.empty());
        interactionSpan->SetStatus(failed ? StatusCode::Error : StatusCode::Unset);
        if (payload->http_status_code)
            interactionSpan->AddAttribute("http.response.status_code", static_cast<int64_t>(payload->http_status_code));
        if (failed)
            interactionSpan->AddAttribute("error.type", payload->error_type.empty()
                ? std::to_string(payload->http_status_code) : payload->error_type);
        interactionSpan->End();
        rContext->removeInteraction();
        return OTEL_SUCCESS;
    }

    // If errorCode is 0 or errMsg is empty, there is no error.
    bool isError = payload->errorCode != 0 && !payload->errorMsg.empty();
    if (isError) {
        if (payload->errorCode >= HTTP_ERROR_1XX &&   payload->errorCode < HTTP_ERROR_4XX ) {
            interactionSpan->SetStatus(StatusCode::Unset);
        }
        else if (payload->errorCode >= HTTP_ERROR_4XX &&   payload->errorCode < HTTP_ERROR_5XX ) {
            if (interactionSpan->GetSpanKind() == SpanKind::SERVER)
                interactionSpan->SetStatus(StatusCode::Unset);
            else
                interactionSpan->SetStatus(StatusCode::Error, payload->errorMsg);

        } else {
            interactionSpan->SetStatus(StatusCode::Error, payload->errorMsg);
        }
        if (!payload->modern_http_attributes)
            interactionSpan->AddAttribute("error_code", payload->errorCode);
        LOG4CXX_TRACE(mLogger, "Span updated with error Code: " << payload->errorCode);
    } else {
        interactionSpan->SetStatus(payload->modern_http_attributes ? StatusCode::Unset : StatusCode::Ok);
    }

    if (!payload->backendName.empty()) {
        // TODO : update span name when api is added.
        interactionSpan->AddAttribute("backend_name", payload->backendName);
        interactionSpan->AddAttribute("backend_type", payload->backendType);
        LOG4CXX_TRACE(mLogger, "Span updated with BackendName: " << payload->backendName
            << " BackendType: " << payload->backendType);
    }

    LOG4CXX_TRACE(mLogger, "Ending Span with id: " << interactionSpan.get());
    interactionSpan->End();
    rContext->removeInteraction();
    return OTEL_SUCCESS;
}

} // core
} // otel
