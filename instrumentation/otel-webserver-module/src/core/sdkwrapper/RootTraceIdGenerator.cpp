// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0
#include "sdkwrapper/RootTraceIdGenerator.h"

namespace otel {
namespace core {
namespace sdkwrapper {
namespace {
thread_local opentelemetry::trace::TraceId requestedTraceId;
}

RootTraceIdScope::RootTraceIdScope(opentelemetry::trace::TraceId traceId) noexcept
    : previous(requestedTraceId)
{
    requestedTraceId = traceId;
}

RootTraceIdScope::~RootTraceIdScope()
{
    requestedTraceId = previous;
}

opentelemetry::trace::TraceId RootTraceIdGenerator::GenerateTraceId() noexcept
{
    if (!requestedTraceId.IsValid()) return random.GenerateTraceId();
    const auto result = requestedTraceId;
    requestedTraceId = opentelemetry::trace::TraceId();
    return result;
}

opentelemetry::trace::SpanId RootTraceIdGenerator::GenerateSpanId() noexcept
{
    return random.GenerateSpanId();
}
} // sdkwrapper
} // core
} // otel
