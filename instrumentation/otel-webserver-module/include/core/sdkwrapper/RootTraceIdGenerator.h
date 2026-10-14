// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <opentelemetry/sdk/trace/random_id_generator.h>

namespace otel {
namespace core {
namespace sdkwrapper {

// An override exists only while creating one server root on this thread.
// Other requests, providers and all normal spans keep the SDK's random IDs.
class RootTraceIdScope {
public:
    explicit RootTraceIdScope(opentelemetry::trace::TraceId traceId) noexcept;
    ~RootTraceIdScope();
    RootTraceIdScope(const RootTraceIdScope&) = delete;
    RootTraceIdScope& operator=(const RootTraceIdScope&) = delete;
private:
    opentelemetry::trace::TraceId previous;
};

class RootTraceIdGenerator : public opentelemetry::sdk::trace::IdGenerator {
public:
    opentelemetry::trace::TraceId GenerateTraceId() noexcept override;
    opentelemetry::trace::SpanId GenerateSpanId() noexcept override;
private:
    opentelemetry::sdk::trace::RandomIdGenerator random;
};

} // sdkwrapper
} // core
} // otel
