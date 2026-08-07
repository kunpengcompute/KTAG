/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_tcp_signature_provider.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>

#include "proxy_carrier_parser.h"
#include "proxy_feature_catalog.h"
#include "proxy_math.h"
#include "proxy_provider_common.h"
#include "proxy_tls_parser.h"

namespace ktag_proxy_features {
namespace {

constexpr uint8_t TCP_PROTOCOL = 6U;
constexpr uint8_t TCP_SYN_FLAG = 0x02U;
constexpr size_t PREFIX_CAPACITY = 16U * 1024U;
constexpr uint32_t SEGMENT_CAPACITY = 64U;
constexpr uint32_t RECORD_CAPACITY = 16U;

enum tcp_signature_feature_index : size_t {
    FORWARD_PREFIX_LENGTH_LOG2 = 0U,
    REVERSE_PREFIX_LENGTH_LOG2,
    FORWARD_PREFIX_TRUNCATED,
    REVERSE_PREFIX_TRUNCATED,
    SEGMENT_GAP_COUNT,
    RETRANSMISSION_COUNT,
    OUT_OF_ORDER_COUNT,
    FORWARD_ENTROPY,
    REVERSE_ENTROPY,
    FORWARD_PRINTABLE_RATIO,
    REVERSE_PRINTABLE_RATIO,
    FORWARD_ZERO_RATIO,
    REVERSE_ZERO_RATIO,
    TLS_RECORD_CANDIDATE,
    TLS_CLIENT_HELLO_COMPLETE,
    TLS_LEGACY_VERSION,
    TLS_SUPPORTED_VERSION_MAX,
    TLS_CIPHER_SUITE_COUNT,
    TLS_EXTENSION_COUNT,
    TLS_CIPHER_SUITE_HASH_U16,
    TLS_EXTENSION_ORDER_HASH_U16,
    TLS_SNI_PRESENT,
    TLS_ALPN_H2_PRESENT,
    TLS_ALPN_HTTP11_PRESENT,
    TLS_SESSION_ID_LENGTH,
    TLS_PSK_PRESENT,
    TLS_EARLY_DATA_PRESENT,
    TLS_RECORD_SIGNED_LENGTH,
    TLS_RECORD_VALID_COUNT,
    TLS_PARSE_FAILURE_COUNT,
    CARRIER_HTTP1_REQUEST,
    CARRIER_WEBSOCKET_UPGRADE,
    CARRIER_HTTP2_PREFACE,
    CARRIER_HTTP2_FRAME,
    CARRIER_GRPC_CONTENT_TYPE,
    TCP_SIGNATURE_FEATURE_COUNT,
};

static_assert(
    TCP_SIGNATURE_FEATURE_COUNT ==
        KTAG_PROXY_TCP_SIGNATURE_FEATURES.size());
static_assert(
    KTAG_PROXY_TCP_SIGNATURE_FEATURES
        [TLS_CIPHER_SUITE_HASH_U16].dimension == 2U);
static_assert(
    KTAG_PROXY_TCP_SIGNATURE_FEATURES
        [TLS_EXTENSION_ORDER_HASH_U16].dimension == 2U);
static_assert(
    KTAG_PROXY_TCP_SIGNATURE_FEATURES
        [TLS_RECORD_SIGNED_LENGTH].dimension == RECORD_CAPACITY);

struct prefix_state {
    bool initialized = false;
    bool truncated = false;
    uint32_t base_sequence = 0U;
    uint32_t segment_count = 0U;
    size_t contiguous_length = 0U;
    size_t highest_seen_end = 0U;
    std::array<uint8_t, PREFIX_CAPACITY> bytes{};
    std::array<uint8_t, PREFIX_CAPACITY> present{};
};

struct tcp_signature_flow_state {
    std::array<prefix_state, 2U> prefixes{};
    std::array<uint32_t, 2U> observed_tls_record_count{};
    std::array<float, RECORD_CAPACITY> signed_record_lengths{};
    uint32_t signed_record_count = 0U;
    uint64_t segment_gap_count = 0U;
    uint64_t retransmission_count = 0U;
    uint64_t out_of_order_count = 0U;
};

size_t direction_index(ktag_direction direction)
{
    return direction == KTAG_DIRECTION_REVERSE ? 1U : 0U;
}

ktag_status set_optional(
    ktag_feature_output *output,
    tcp_signature_feature_index feature,
    const float *values,
    uint32_t value_count)
{
    const ktag_status status = ktag_feature_output_set(
        output,
        KTAG_PROXY_TCP_SIGNATURE_FEATURES[feature].feature_id,
        values,
        value_count);
    return status == KTAG_ERROR_NOT_FOUND ? KTAG_SUCCESS : status;
}

ktag_status set_scalar(
    ktag_feature_output *output,
    tcp_signature_feature_index feature,
    float value)
{
    return set_optional(output, feature, &value, 1U);
}

float printable_ratio(const uint8_t *data, size_t length)
{
    if (data == nullptr || length == 0U) {
        return 0.0F;
    }
    size_t printable = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (data[index] >= 0x20U && data[index] <= 0x7eU) {
            ++printable;
        }
    }
    return static_cast<float>(
        static_cast<double>(printable) /
        static_cast<double>(length));
}

float zero_ratio(const uint8_t *data, size_t length)
{
    if (data == nullptr || length == 0U) {
        return 0.0F;
    }
    size_t zeros = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (data[index] == 0U) {
            ++zeros;
        }
    }
    return static_cast<float>(
        static_cast<double>(zeros) / static_cast<double>(length));
}

void add_segment(
    prefix_state *prefix,
    const uint8_t *payload,
    size_t payload_length,
    uint32_t payload_sequence,
    tcp_signature_flow_state *flow)
{
    if (prefix == nullptr || payload == nullptr ||
        payload_length == 0U || flow == nullptr ||
        prefix->segment_count >= SEGMENT_CAPACITY) {
        return;
    }
    ++prefix->segment_count;

    if (!prefix->initialized) {
        prefix->initialized = true;
        prefix->base_sequence = payload_sequence;
    }

    const uint32_t relative =
        payload_sequence - prefix->base_sequence;
    if (relative >
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        flow->out_of_order_count = saturating_add_u64(
            flow->out_of_order_count, 1U);
        return;
    }
    const size_t offset = static_cast<size_t>(relative);
    const size_t contiguous_before = prefix->contiguous_length;
    const size_t highest_before = prefix->highest_seen_end;
    if (offset >= PREFIX_CAPACITY) {
        prefix->truncated = true;
        flow->segment_gap_count = saturating_add_u64(
            flow->segment_gap_count, 1U);
        return;
    }

    const size_t writable = PREFIX_CAPACITY - offset;
    const size_t copy_length = std::min(payload_length, writable);
    if (copy_length < payload_length ||
        offset + copy_length == PREFIX_CAPACITY) {
        prefix->truncated = true;
    }

    bool duplicate_found = false;
    bool novel_found = false;
    for (size_t index = 0U; index < copy_length; ++index) {
        const size_t target = offset + index;
        if (prefix->present[target] != 0U) {
            duplicate_found = true;
            continue;
        }
        prefix->bytes[target] = payload[index];
        prefix->present[target] = 1U;
        novel_found = true;
    }

    if (duplicate_found) {
        flow->retransmission_count = saturating_add_u64(
            flow->retransmission_count, 1U);
    }
    if (novel_found && offset > contiguous_before) {
        flow->segment_gap_count = saturating_add_u64(
            flow->segment_gap_count, 1U);
    }
    if (novel_found && offset < highest_before) {
        flow->out_of_order_count = saturating_add_u64(
            flow->out_of_order_count, 1U);
    }

    prefix->highest_seen_end = std::max(
        prefix->highest_seen_end, offset + copy_length);
    while (prefix->contiguous_length < PREFIX_CAPACITY &&
           prefix->present[prefix->contiguous_length] != 0U) {
        ++prefix->contiguous_length;
    }
    if (prefix->contiguous_length == PREFIX_CAPACITY) {
        prefix->truncated = true;
    }
}

void update_tls_record_sequence(
    tcp_signature_flow_state *state,
    size_t prefix_index,
    ktag_direction direction)
{
    auto &prefix = state->prefixes[prefix_index];
    tls_parse_result parsed{};
    parse_tls_prefix(
        prefix.bytes.data(), prefix.contiguous_length, &parsed);
    uint32_t &observed =
        state->observed_tls_record_count[prefix_index];
    const uint32_t available = std::min(
        parsed.stored_record_count,
        static_cast<uint32_t>(parsed.record_lengths.size()));
    for (uint32_t index = observed;
         index < available &&
         state->signed_record_count < RECORD_CAPACITY;
         ++index) {
        state->signed_record_lengths[state->signed_record_count++] =
            signed_bounded_float(
                parsed.record_lengths[index], direction);
    }
    observed = parsed.complete_record_count;
}

ktag_status tcp_signature_flow_create(
    void *,
    const ktag_flow_view *,
    void **flow_state)
{
    if (flow_state == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    auto *state = new (std::nothrow) tcp_signature_flow_state();
    if (state == nullptr) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    }
    *flow_state = state;
    return KTAG_SUCCESS;
}

ktag_status tcp_signature_on_packet(
    void *,
    void *flow_state,
    const ktag_packet_view *packet,
    const ktag_flow_view *)
{
    auto *state =
        static_cast<tcp_signature_flow_state *>(flow_state);
    if (state == nullptr || packet == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }

    ktag_packet_transport_view transport{};
    transport.struct_size = sizeof(transport);
    transport.api_version = KTAG_API_VERSION;
    uint32_t payload_length = 0U;
    const uint8_t *payload =
        ktag_packet_view_get_payload(packet, &payload_length);
    if (ktag_packet_view_get_transport(packet, &transport) !=
            KTAG_SUCCESS ||
        (payload == nullptr && payload_length != 0U) ||
        (transport.direction != KTAG_DIRECTION_FORWARD &&
         transport.direction != KTAG_DIRECTION_REVERSE)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (transport.transport_protocol != TCP_PROTOCOL ||
        payload_length == 0U) {
        return KTAG_SUCCESS;
    }

    const size_t index = direction_index(transport.direction);
    auto &prefix = state->prefixes[index];
    const size_t previous_length = prefix.contiguous_length;
    const uint32_t payload_sequence = transport.tcp_sequence +
        ((transport.tcp_flags & TCP_SYN_FLAG) != 0U ? 1U : 0U);
    add_segment(
        &prefix,
        payload,
        payload_length,
        payload_sequence,
        state);
    if (prefix.contiguous_length != previous_length) {
        update_tls_record_sequence(
            state, index, transport.direction);
    }
    return KTAG_SUCCESS;
}

const tls_parse_result *select_client_hello(
    const std::array<tls_parse_result, 2U> &tls)
{
    if (tls[0U].client_hello_complete) {
        return &tls[0U];
    }
    if (tls[1U].client_hello_complete) {
        return &tls[1U];
    }
    return nullptr;
}

ktag_status write_tcp_features(
    const tcp_signature_flow_state &state,
    const std::array<tls_parse_result, 2U> &tls,
    const std::array<carrier_parse_result, 2U> &carrier,
    ktag_feature_output *output)
{
    const auto &forward = state.prefixes[0U];
    const auto &reverse = state.prefixes[1U];
    const tls_parse_result *hello = select_client_hello(tls);
    const std::array<float, 2U> cipher_hash{{
        hello == nullptr ? 0.0F :
            bounded_float(hello->cipher_suite_hash >> 16U),
        hello == nullptr ? 0.0F :
            bounded_float(hello->cipher_suite_hash & 0xffffU),
    }};
    const std::array<float, 2U> extension_hash{{
        hello == nullptr ? 0.0F :
            bounded_float(hello->extension_order_hash >> 16U),
        hello == nullptr ? 0.0F :
            bounded_float(hello->extension_order_hash & 0xffffU),
    }};
    const bool record_candidate =
        tls[0U].record_candidate || tls[1U].record_candidate;
    const uint64_t parse_failures = saturating_add_u64(
        tls[0U].parse_failure_count,
        tls[1U].parse_failure_count);

    const bool http1 =
        carrier[0U].http1_request || carrier[1U].http1_request;
    const bool websocket = carrier[0U].websocket_upgrade ||
        carrier[1U].websocket_upgrade;
    const bool http2_preface = carrier[0U].http2_preface ||
        carrier[1U].http2_preface;
    const bool http2_frame = carrier[0U].http2_frame ||
        carrier[1U].http2_frame;
    const bool grpc = carrier[0U].grpc_content_type ||
        carrier[1U].grpc_content_type;

    if (set_scalar(output, FORWARD_PREFIX_LENGTH_LOG2,
            log2_1p_u64(forward.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, REVERSE_PREFIX_LENGTH_LOG2,
            log2_1p_u64(reverse.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, FORWARD_PREFIX_TRUNCATED,
            forward.truncated ? 1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, REVERSE_PREFIX_TRUNCATED,
            reverse.truncated ? 1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, SEGMENT_GAP_COUNT,
            bounded_float(state.segment_gap_count)) != KTAG_SUCCESS ||
        set_scalar(output, RETRANSMISSION_COUNT,
            bounded_float(state.retransmission_count)) != KTAG_SUCCESS ||
        set_scalar(output, OUT_OF_ORDER_COUNT,
            bounded_float(state.out_of_order_count)) != KTAG_SUCCESS ||
        set_scalar(output, FORWARD_ENTROPY,
            byte_entropy(forward.bytes.data(),
                forward.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, REVERSE_ENTROPY,
            byte_entropy(reverse.bytes.data(),
                reverse.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, FORWARD_PRINTABLE_RATIO,
            printable_ratio(forward.bytes.data(),
                forward.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, REVERSE_PRINTABLE_RATIO,
            printable_ratio(reverse.bytes.data(),
                reverse.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, FORWARD_ZERO_RATIO,
            zero_ratio(forward.bytes.data(),
                forward.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, REVERSE_ZERO_RATIO,
            zero_ratio(reverse.bytes.data(),
                reverse.contiguous_length)) != KTAG_SUCCESS ||
        set_scalar(output, TLS_RECORD_CANDIDATE,
            record_candidate ? 1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, TLS_CLIENT_HELLO_COMPLETE,
            hello == nullptr ? 0.0F : 1.0F) != KTAG_SUCCESS ||
        set_scalar(output, TLS_LEGACY_VERSION,
            hello == nullptr ? 0.0F :
                bounded_float(hello->legacy_version)) != KTAG_SUCCESS ||
        set_scalar(output, TLS_SUPPORTED_VERSION_MAX,
            hello == nullptr ? 0.0F :
                bounded_float(hello->supported_version_max)) !=
            KTAG_SUCCESS ||
        set_scalar(output, TLS_CIPHER_SUITE_COUNT,
            hello == nullptr ? 0.0F :
                bounded_float(hello->cipher_suite_count)) !=
            KTAG_SUCCESS ||
        set_scalar(output, TLS_EXTENSION_COUNT,
            hello == nullptr ? 0.0F :
                bounded_float(hello->extension_count)) != KTAG_SUCCESS ||
        set_optional(output, TLS_CIPHER_SUITE_HASH_U16,
            cipher_hash.data(),
            static_cast<uint32_t>(cipher_hash.size())) != KTAG_SUCCESS ||
        set_optional(output, TLS_EXTENSION_ORDER_HASH_U16,
            extension_hash.data(),
            static_cast<uint32_t>(extension_hash.size())) !=
            KTAG_SUCCESS ||
        set_scalar(output, TLS_SNI_PRESENT,
            hello != nullptr && hello->sni_present ? 1.0F : 0.0F) !=
            KTAG_SUCCESS ||
        set_scalar(output, TLS_ALPN_H2_PRESENT,
            hello != nullptr && hello->alpn_h2_present ? 1.0F : 0.0F) !=
            KTAG_SUCCESS ||
        set_scalar(output, TLS_ALPN_HTTP11_PRESENT,
            hello != nullptr && hello->alpn_http11_present ?
                1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, TLS_SESSION_ID_LENGTH,
            hello == nullptr ? 0.0F :
                bounded_float(hello->session_id_length)) != KTAG_SUCCESS ||
        set_scalar(output, TLS_PSK_PRESENT,
            hello != nullptr && hello->psk_present ? 1.0F : 0.0F) !=
            KTAG_SUCCESS ||
        set_scalar(output, TLS_EARLY_DATA_PRESENT,
            hello != nullptr && hello->early_data_present ?
                1.0F : 0.0F) != KTAG_SUCCESS ||
        set_optional(output, TLS_RECORD_SIGNED_LENGTH,
            state.signed_record_lengths.data(),
            static_cast<uint32_t>(
                state.signed_record_lengths.size())) != KTAG_SUCCESS ||
        set_scalar(output, TLS_RECORD_VALID_COUNT,
            bounded_float(state.signed_record_count)) != KTAG_SUCCESS ||
        set_scalar(output, TLS_PARSE_FAILURE_COUNT,
            bounded_float(parse_failures)) != KTAG_SUCCESS ||
        set_scalar(output, CARRIER_HTTP1_REQUEST,
            http1 ? 1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, CARRIER_WEBSOCKET_UPGRADE,
            websocket ? 1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, CARRIER_HTTP2_PREFACE,
            http2_preface ? 1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, CARRIER_HTTP2_FRAME,
            http2_frame ? 1.0F : 0.0F) != KTAG_SUCCESS ||
        set_scalar(output, CARRIER_GRPC_CONTENT_TYPE,
            grpc ? 1.0F : 0.0F) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    return KTAG_SUCCESS;
}

ktag_status tcp_signature_flow_finish(
    void *,
    void *flow_state,
    const ktag_flow_view *,
    ktag_feature_output *output)
{
    const auto *state =
        static_cast<const tcp_signature_flow_state *>(flow_state);
    if (state == nullptr || output == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }

    std::array<tls_parse_result, 2U> tls{};
    std::array<carrier_parse_result, 2U> carrier{};
    for (size_t index = 0U; index < state->prefixes.size(); ++index) {
        const auto &prefix = state->prefixes[index];
        parse_tls_prefix(
            prefix.bytes.data(), prefix.contiguous_length, &tls[index]);
        if (!tls[index].record_candidate) {
            parse_cleartext_carrier(
                prefix.bytes.data(),
                prefix.contiguous_length,
                &carrier[index]);
        }
    }
    return write_tcp_features(*state, tls, carrier, output);
}

void tcp_signature_flow_destroy(void *, void *flow_state)
{
    delete static_cast<tcp_signature_flow_state *>(flow_state);
}

} // namespace

ktag_status register_tcp_signature_provider(
    ktag_feature_registry *registry)
{
    ktag_feature_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.api_version = KTAG_API_VERSION;
    callbacks.flow_create = tcp_signature_flow_create;
    callbacks.on_packet = tcp_signature_on_packet;
    callbacks.flow_finish = tcp_signature_flow_finish;
    callbacks.flow_destroy = tcp_signature_flow_destroy;
    return register_provider(
        registry,
        KTAG_PROXY_TCP_SIGNATURE_CATALOG,
        callbacks,
        KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW,
        nullptr);
}

} // namespace ktag_proxy_features
