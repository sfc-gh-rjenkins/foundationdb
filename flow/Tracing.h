/*
 * Tracing.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "fdbclient/FDBTypes.h"
#include "flow/IRandom.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <atomic>

struct Location {
	StringRef name;
};

inline Location operator"" _loc(const char* str, size_t size) {
	return Location{ StringRef(reinterpret_cast<const uint8_t*>(str), size) };
}

struct Span {
	Span(SpanID context, Location location, std::initializer_list<SpanID> const& parents = {})
	  : context(context), begin(g_network->now()), location(location), parents(arena, parents.begin(), parents.end()) {
		if (parents.size() > 0) {
			// If the parents' token is 0 (meaning the trace should not be
			// recorded), set the child token to 0 as well. Otherwise, generate
			// a new, random token.
			uint64_t traceId = 0;
			if ((*parents.begin()).second() > 0) {
				traceId = deterministicRandom()->randomUInt64();
			}
			this->context = SpanID((*parents.begin()).first(), traceId);
		}
	}
	Span(Location location, std::initializer_list<SpanID> const& parents = {})
	  : Span(UID(deterministicRandom()->randomUInt64(),
	             deterministicRandom()->random01() < FLOW_KNOBS->TRACING_SAMPLE_RATE
	                 ? deterministicRandom()->randomUInt64()
	                 : 0),
	         location,
	         parents) {}
	Span(Location location, SpanID context) : Span(location, { context }) {}
	Span(const Span&) = delete;
	Span(Span&& o) {
		arena = std::move(o.arena);
		context = o.context;
		begin = o.begin;
		end = o.end;
		location = o.location;
		parents = std::move(o.parents);
		o.context = UID();
		o.begin = 0.0;
		o.end = 0.0;
	}
	Span() {}
	~Span();
	Span& operator=(Span&& o);
	Span& operator=(const Span&) = delete;
	void swap(Span& other) {
		std::swap(arena, other.arena);
		std::swap(context, other.context);
		std::swap(begin, other.begin);
		std::swap(end, other.end);
		std::swap(location, other.location);
		std::swap(parents, other.parents);
	}

	void addParent(SpanID span) {
		if (parents.size() == 0) {
			uint64_t traceId = 0;
			if (span.second() > 0) {
				traceId = context.second() == 0 ? deterministicRandom()->randomUInt64() : context.second();
			}
			// Use first parent to set trace ID. This is non-ideal for spans
			// with multiple parents, because the trace ID will associate the
			// span with only one trace. A workaround is to look at the parent
			// relationships instead of the trace ID. Another option in the
			// future is to keep a list of trace IDs.
			context = SpanID(span.first(), traceId);
		}
		parents.push_back(arena, span);
	}

	void addTag(const StringRef& key, const StringRef& value) { tags[key] = value; }

	Arena arena;
	UID context = UID();
	double begin = 0.0, end = 0.0;
	Location location;
	SmallVectorRef<SpanID> parents;
	std::unordered_map<StringRef, StringRef> tags;
};

// OTELSpan
//
// OTELSpan is a tracing implementation which, for the most part, complies with the W3C Trace Context specification
// https://www.w3.org/TR/trace-context/ and the OpenTelemetry API
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md.
//
// The major differences between OTELSpan and the current Span implementation, which is based off the OpenTracing.io
// specification https://opentracing.io/ are as follows.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#span
//
// OTELSpans have...
// 1. A SpanContext which consists of 3 attributes.
//
// TraceId - A valid trace identifier is a 16-byte array with at least one non-zero byte.
// SpanId - A valid span identifier is an 8-byte array with at least one non-zero byte.
// TraceFlags - 1 byte, bit field for flags.
//
// TraceState is not implemented, specifically we do not provide some of the following APIs
// https://www.w3.org/TR/trace-context/#mutating-the-tracestate-field In particular APIs to delete/update a specific,
// arbitrary key/value pair, as this complies with the OTEL specification where SpanContexts are immutable.
//
// 2. A begin/end and those values are serialized, unlike the Span implementation which has an end but serializes with a
// begin and calculated duration field.
// 3. A SpanKind
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#spankind
// 4. A SpanStatus
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#set-status
// 5. A singular parent SpanContext, which may optionally be null, as opposed to our Span implementation which allows
// for a list of parents.
// 6. An "attributes" rather than "tags", however the implementation is the same, a key/value map of strings.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/common/common.md#attributes
// 7. An optional list of linked SpanContexts.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#specifying-links
// 8. An optional list of timestamped Events.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#add-events

enum class SpanKind : uint8_t {
	CLIENT = 0,
	SERVER = 1,
	PRODUCER = 2,
	CONSUMER = 3,
	INTERNAL = 4,
};

enum class SpanStatus : uint8_t {
	UNSET = 0,
	OK = 1,
	ERROR = 2,
};

struct OTELEvent {
	StringRef name;
	double time = 0.0;
	std::unordered_map<StringRef, StringRef> attributes;
};

struct OTELSpan {
	OTELSpan(SpanContext context,
	         Location location,
	         SpanContext parentContext,
	         std::initializer_list<SpanContext> const& links = {})
	  : context(context), location(location), parentContext(parentContext), links(links.begin(), links.end()),
	    begin(g_network->now()) {
		// We've simplified the logic here, essentially we're now always setting trace and span ids and relying on the
		// TraceFlags to determine if we're sampling. Therefore if the parent is sampled, we simply overwrite this
		// span's traceID with the parent trace id.
		if (parentContext.isSampled()) {
			this->context.traceID = UID(parentContext.traceID.first(), parentContext.traceID.second());
			this->context.m_Flags = TraceFlags::sampled;
		} else {
			// However there are two other cases.
			// 1. A legitamite parent span exists but it was not selected for tracing.
			// 2. There is no actual parent, just a default arg parent provided by the constructor AND the "child" span
			// was selected for sampling. For case 1. we handle below by marking the child as unsampled. For case 2 we
			// needn't do anything, and can rely on the values in this OTELSpan
			if (parentContext.traceID.first() != 0 && parentContext.traceID.second() != 0 &&
			    parentContext.spanID != 0) {
				this->context.m_Flags = TraceFlags::unsampled;
			}
		}
		this->kind = SpanKind::SERVER;
		this->status = SpanStatus::OK;
		this->attributes["address"_sr] = g_network->getLocalAddress().toString();
	}

	OTELSpan(Location location,
	         SpanContext parent = SpanContext(),
	         std::initializer_list<SpanContext> const& links = {})
	  : OTELSpan(
	        SpanContext(UID(deterministicRandom()->randomUInt64(), deterministicRandom()->randomUInt64()), // traceID
	                    deterministicRandom()->randomUInt64(), // spanID
	                    deterministicRandom()->random01() < FLOW_KNOBS->TRACING_SAMPLE_RATE // sampled or unsampled
	                        ? TraceFlags::sampled
	                        : TraceFlags::unsampled),
	        location,
	        parent,
	        links) {}

	OTELSpan(Location location, SpanContext parent, SpanContext link) : OTELSpan(location, parent, { link }) {}

	// NOTE: This constructor is primarly for unit testing until we sort out how to enable/disable a Knob dynamically in
	// a test.
	OTELSpan(Location location,
	         std::function<double()> rateProvider,
	         SpanContext parent = SpanContext(),
	         std::initializer_list<SpanContext> const& links = {})
	  : OTELSpan(SpanContext(UID(deterministicRandom()->randomUInt64(), deterministicRandom()->randomUInt64()),
	                         deterministicRandom()->randomUInt64(),
	                         deterministicRandom()->random01() < rateProvider() ? TraceFlags::sampled
	                                                                            : TraceFlags::unsampled),
	             location,
	             parent,
	             links) {}

	OTELSpan(const OTELSpan&) = delete;
	OTELSpan(OTELSpan&& o) {
		location = o.location;
		context = o.context;
		kind = o.kind;
		begin = o.begin;
		end = o.end;
		parentContext = std::move(o.parentContext);
		links = std::move(o.links);
		// TODO - Should we move events and attributes or should we follow the previous model
		// where we didn't move tags?
		events = std::move(o.events);
		attributes = std::move(o.attributes);
		status = o.status;
		o.context = SpanContext();
		o.kind = SpanKind::CLIENT;
		o.begin = 0.0;
		o.end = 0.0;
		o.status = SpanStatus::UNSET;
	}
	OTELSpan() {}
	~OTELSpan();
	OTELSpan& operator=(OTELSpan&& o);
	OTELSpan& operator=(const OTELSpan&) = delete;
	void swap(OTELSpan& other) {
		std::swap(context, other.context);
		std::swap(location, other.location);
		std::swap(parentContext, other.parentContext);
		std::swap(kind, other.kind);
		std::swap(status, other.status);
		std::swap(begin, other.begin);
		std::swap(end, other.end);
		// We're going to keep the swap here for links because it is somewhat equivalent to {} parents
		// in the Span implementation.
		std::swap(links, other.links);
		// TODO - Should we leave out attributes and events? Attributes/tags are left out in the Span::swap.
		// Events are an entirely new concept here, so no precedence.
		std::swap(attributes, other.attributes);
		std::swap(events, other.events);
	}

	void addLink(SpanContext linkContext) { links.push_back(linkContext); }

	void addEvent(OTELEvent event) { events.push_back(event); }

	void addAttribute(const StringRef& key, const StringRef& value) { attributes[key] = value; }

	SpanContext context;
	Location location;
	SpanContext parentContext;
	SpanKind kind;
	// TODO implement SmallVectorRef for links?
	std::vector<SpanContext> links;
	double begin = 0.0, end = 0.0;
	std::unordered_map<StringRef, StringRef> attributes;
	// Have to use vector here due to unordered_map and is_trivially_descructable?
	std::vector<OTELEvent> events;
	SpanStatus status;
};

// The user selects a tracer using a string passed to fdbserver on boot.
// Clients should not refer to TracerType directly, and mappings of names to
// values in this enum can change without notice.
enum class TracerType {
	DISABLED = 0,
	NETWORK_LOSSY = 1,
	SIM_END = 2, // Any tracers that come after SIM_END will not be tested in simulation
	LOG_FILE = 3
};

struct ITracer {
	virtual ~ITracer();
	virtual TracerType type() const = 0;
	// passed ownership to the tracer
	virtual void trace(Span const& span) = 0;
	virtual void trace(OTELSpan const& span) = 0;
};

void openTracer(TracerType type);

template <class T>
struct SpannedDeque : Deque<T> {
	Span span;
	explicit SpannedDeque(Location loc) : span(loc) {}
	SpannedDeque(SpannedDeque&& other) : Deque<T>(std::move(other)), span(std::move(other.span)) {}
	SpannedDeque(SpannedDeque const&) = delete;
	SpannedDeque& operator=(SpannedDeque const&) = delete;
	SpannedDeque& operator=(SpannedDeque&& other) {
		*static_cast<Deque<T>*>(this) = std::move(other);
		span = std::move(other.span);
	}
};

template <class T>
struct OTELSpannedDeque : Deque<T> {
	OTELSpan span;
	explicit OTELSpannedDeque(Location loc) : span(loc) {}
	OTELSpannedDeque(OTELSpannedDeque&& other) : Deque<T>(std::move(other)), span(std::move(other.span)) {}
	OTELSpannedDeque(OTELSpannedDeque const&) = delete;
	OTELSpannedDeque& operator=(OTELSpannedDeque const&) = delete;
	OTELSpannedDeque& operator=(OTELSpannedDeque&& other) {
		*static_cast<Deque<T>*>(this) = std::move(other);
		span = std::move(other.span);
	}
};
