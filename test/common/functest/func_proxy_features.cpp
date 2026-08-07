/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "ktag_feature_plugin.h"
#include "proxy_feature_catalog.h"
#include "proxy_quic_parser.h"

namespace {

constexpr size_t SIGNED_PAYLOAD_LENGTH = 0U;
constexpr size_t RELATIVE_TIME_LOG2_US = 1U;
constexpr size_t TCP_FLAGS = 2U;
constexpr size_t SEQUENCE_VALID_COUNT = 3U;
constexpr size_t BURST_SIGNED_PACKET_COUNT = 4U;
constexpr size_t BURST_SIGNED_PAYLOAD_BYTES_LOG2 = 5U;
constexpr size_t BURST_DURATION_LOG2_US = 6U;
constexpr size_t BURST_VALID_COUNT = 7U;
constexpr size_t DIRECTION_SWITCH_COUNT = 8U;
constexpr size_t FORWARD_PAYLOAD_RATIO = 9U;

constexpr size_t TCP_FORWARD_PREFIX_LENGTH_LOG2 = 0U;
constexpr size_t TCP_REVERSE_PREFIX_LENGTH_LOG2 = 1U;
constexpr size_t TCP_FORWARD_PREFIX_TRUNCATED = 2U;
constexpr size_t TCP_SEGMENT_GAP_COUNT = 4U;
constexpr size_t TCP_RETRANSMISSION_COUNT = 5U;
constexpr size_t TCP_OUT_OF_ORDER_COUNT = 6U;
constexpr size_t TCP_FORWARD_ENTROPY = 7U;
constexpr size_t TCP_FORWARD_PRINTABLE_RATIO = 9U;
constexpr size_t TCP_FORWARD_ZERO_RATIO = 11U;
constexpr size_t TLS_RECORD_CANDIDATE = 13U;
constexpr size_t TLS_CLIENT_HELLO_COMPLETE = 14U;
constexpr size_t TLS_LEGACY_VERSION = 15U;
constexpr size_t TLS_SUPPORTED_VERSION_MAX = 16U;
constexpr size_t TLS_CIPHER_SUITE_COUNT = 17U;
constexpr size_t TLS_EXTENSION_COUNT = 18U;
constexpr size_t TLS_CIPHER_SUITE_HASH_U16 = 19U;
constexpr size_t TLS_EXTENSION_ORDER_HASH_U16 = 20U;
constexpr size_t TLS_SNI_PRESENT = 21U;
constexpr size_t TLS_ALPN_H2_PRESENT = 22U;
constexpr size_t TLS_ALPN_HTTP11_PRESENT = 23U;
constexpr size_t TLS_SESSION_ID_LENGTH = 24U;
constexpr size_t TLS_PSK_PRESENT = 25U;
constexpr size_t TLS_EARLY_DATA_PRESENT = 26U;
constexpr size_t TLS_RECORD_SIGNED_LENGTH = 27U;
constexpr size_t TLS_RECORD_VALID_COUNT = 28U;
constexpr size_t TLS_PARSE_FAILURE_COUNT = 29U;
constexpr size_t CARRIER_HTTP1_REQUEST = 30U;
constexpr size_t CARRIER_WEBSOCKET_UPGRADE = 31U;
constexpr size_t CARRIER_HTTP2_PREFACE = 32U;
constexpr size_t CARRIER_HTTP2_FRAME = 33U;
constexpr size_t CARRIER_GRPC_CONTENT_TYPE = 34U;

constexpr size_t QUIC_PACKET_CANDIDATE_COUNT = 0U;
constexpr size_t QUIC_PARSE_SUCCESS_COUNT = 1U;
constexpr size_t QUIC_PARSE_FAILURE_COUNT = 2U;
constexpr size_t QUIC_FIXED_BIT_COUNT = 3U;
constexpr size_t QUIC_LONG_HEADER_COUNT = 4U;
constexpr size_t QUIC_SHORT_HEADER_COUNT = 5U;
constexpr size_t QUIC_VERSION_V1_COUNT = 6U;
constexpr size_t QUIC_VERSION_V2_COUNT = 7U;
constexpr size_t QUIC_VERSION_OTHER_COUNT = 8U;
constexpr size_t QUIC_VERSION_NEGOTIATION_COUNT = 9U;
constexpr size_t QUIC_INITIAL_COUNT = 10U;
constexpr size_t QUIC_ZERO_RTT_COUNT = 11U;
constexpr size_t QUIC_HANDSHAKE_COUNT = 12U;
constexpr size_t QUIC_RETRY_COUNT = 13U;
constexpr size_t QUIC_FIRST_INITIAL_LENGTH_LOG2 = 14U;
constexpr size_t QUIC_FIRST_DCID_LENGTH = 15U;
constexpr size_t QUIC_FIRST_SCID_LENGTH = 16U;
constexpr size_t QUIC_FIRST_TOKEN_LENGTH_LOG2 = 17U;
constexpr size_t QUIC_SIGNED_DATAGRAM_LENGTH = 18U;
constexpr size_t QUIC_HEADER_TYPE_SEQUENCE = 19U;
constexpr size_t QUIC_SEQUENCE_VALID_COUNT = 20U;
constexpr size_t QUIC_FIRST_BYTE_HISTOGRAM = 21U;
constexpr size_t QUIC_PAYLOAD_ENTROPY_MEAN = 22U;

void write_be16(
    std::vector<uint8_t> *data,
    size_t offset,
    uint16_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 8U);
    (*data)[offset + 1U] = static_cast<uint8_t>(value & 0xffU);
}

void write_be32(
    std::vector<uint8_t> *data,
    size_t offset,
    uint32_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 24U);
    (*data)[offset + 1U] =
        static_cast<uint8_t>((value >> 16U) & 0xffU);
    (*data)[offset + 2U] =
        static_cast<uint8_t>((value >> 8U) & 0xffU);
    (*data)[offset + 3U] = static_cast<uint8_t>(value & 0xffU);
}

std::vector<uint8_t> make_transport_frame(
    bool ipv6,
    bool tcp,
    bool reverse,
    uint32_t payload_length,
    uint8_t tcp_flags = 0U,
    uint32_t tcp_sequence = 0U,
    bool ipv6_extension = false)
{
    constexpr size_t ETHERNET_LENGTH = 14U;
    constexpr size_t IPV4_LENGTH = 20U;
    constexpr size_t IPV6_LENGTH = 40U;
    constexpr size_t TCP_LENGTH = 20U;
    constexpr size_t UDP_LENGTH = 8U;
    constexpr size_t IPV6_EXTENSION_LENGTH = 8U;
    const size_t extension_length =
        ipv6 && ipv6_extension ? IPV6_EXTENSION_LENGTH : 0U;
    const size_t network_length = ipv6 ?
        IPV6_LENGTH + extension_length : IPV4_LENGTH;
    const size_t transport_length = tcp ? TCP_LENGTH : UDP_LENGTH;
    const uint64_t layer3_payload_length =
        static_cast<uint64_t>(extension_length) +
        transport_length + payload_length;
    if (layer3_payload_length >
            std::numeric_limits<uint16_t>::max() ||
        (!ipv6 &&
         IPV4_LENGTH + layer3_payload_length >
             std::numeric_limits<uint16_t>::max())) {
        return {};
    }
    const uint64_t frame_length =
        ETHERNET_LENGTH + network_length +
        transport_length + payload_length;
    if (frame_length > std::numeric_limits<uint32_t>::max()) {
        return {};
    }

    std::vector<uint8_t> frame(
        static_cast<size_t>(frame_length), 0U);
    const size_t network = ETHERNET_LENGTH;
    if (ipv6) {
        write_be16(&frame, 12U, 0x86ddU);
        frame[network] = 0x60U;
        write_be16(
            &frame,
            network + 4U,
            static_cast<uint16_t>(layer3_payload_length));
        frame[network + 6U] =
            ipv6_extension ? 0U : (tcp ? 6U : 17U);
        frame[network + 7U] = 64U;
        std::array<uint8_t, 16U> first{};
        std::array<uint8_t, 16U> second{};
        first[0U] = 0x20U;
        first[1U] = 0x01U;
        first[15U] = 1U;
        second[0U] = 0x20U;
        second[1U] = 0x01U;
        second[15U] = 2U;
        const auto &source = reverse ? second : first;
        const auto &destination = reverse ? first : second;
        std::memcpy(
            frame.data() + network + 8U,
            source.data(),
            source.size());
        std::memcpy(
            frame.data() + network + 24U,
            destination.data(),
            destination.size());
        if (ipv6_extension) {
            frame[network + IPV6_LENGTH] = tcp ? 6U : 17U;
            frame[network + IPV6_LENGTH + 1U] = 0U;
        }
    } else {
        write_be16(&frame, 12U, 0x0800U);
        frame[network] = 0x45U;
        write_be16(
            &frame,
            network + 2U,
            static_cast<uint16_t>(
                IPV4_LENGTH + layer3_payload_length));
        frame[network + 8U] = 64U;
        frame[network + 9U] = tcp ? 6U : 17U;
        const std::array<uint8_t, 4U> first{{10U, 0U, 0U, 1U}};
        const std::array<uint8_t, 4U> second{{10U, 0U, 0U, 2U}};
        const auto &source = reverse ? second : first;
        const auto &destination = reverse ? first : second;
        std::memcpy(
            frame.data() + network + 12U,
            source.data(),
            source.size());
        std::memcpy(
            frame.data() + network + 16U,
            destination.data(),
            destination.size());
    }

    const size_t transport = network + network_length;
    write_be16(&frame, transport, reverse ? 443U : 50000U);
    write_be16(&frame, transport + 2U, reverse ? 50000U : 443U);
    if (tcp) {
        write_be32(&frame, transport + 4U, tcp_sequence);
        frame[transport + 12U] = 0x50U;
        frame[transport + 13U] = tcp_flags;
    } else {
        write_be16(
            &frame,
            transport + 4U,
            static_cast<uint16_t>(
                UDP_LENGTH + payload_length));
    }
    std::fill(
        frame.begin() +
            static_cast<std::vector<uint8_t>::difference_type>(
                transport + transport_length),
        frame.end(),
        0xabU);
    return frame;
}

std::vector<uint8_t> make_tcp_frame_with_payload(
    bool ipv6,
    bool reverse,
    const std::vector<uint8_t> &payload,
    uint32_t tcp_sequence,
    uint8_t tcp_flags = 0x18U,
    bool ipv6_extension = false)
{
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }
    auto frame = make_transport_frame(
        ipv6,
        true,
        reverse,
        static_cast<uint32_t>(payload.size()),
        tcp_flags,
        tcp_sequence,
        ipv6_extension);
    if (frame.size() < payload.size()) {
        return {};
    }
    std::copy(
        payload.begin(), payload.end(), frame.end() - payload.size());
    return frame;
}

std::vector<uint8_t> make_udp_frame_with_payload(
    bool ipv6,
    bool reverse,
    const std::vector<uint8_t> &payload,
    bool ipv6_extension = false)
{
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }
    auto frame = make_transport_frame(
        ipv6,
        false,
        reverse,
        static_cast<uint32_t>(payload.size()),
        0U,
        0U,
        ipv6_extension);
    if (frame.size() < payload.size()) {
        return {};
    }
    std::copy(
        payload.begin(), payload.end(), frame.end() - payload.size());
    return frame;
}

std::vector<uint8_t> make_ipv6_udp_fragment_with_payload(
    const std::vector<uint8_t> &payload,
    uint16_t fragment_field)
{
    auto frame = make_udp_frame_with_payload(
        true, false, payload, true);
    constexpr size_t NETWORK_OFFSET = 14U;
    constexpr size_t IPV6_HEADER_LENGTH = 40U;
    if (frame.size() < NETWORK_OFFSET + IPV6_HEADER_LENGTH + 8U) {
        return {};
    }
    frame[NETWORK_OFFSET + 6U] = 44U;
    write_be16(
        &frame,
        NETWORK_OFFSET + IPV6_HEADER_LENGTH + 2U,
        fragment_field);
    return frame;
}

std::vector<uint8_t> bytes(std::string_view value)
{
    return std::vector<uint8_t>(value.begin(), value.end());
}

void append_be16(std::vector<uint8_t> *data, uint16_t value)
{
    data->push_back(static_cast<uint8_t>(value >> 8U));
    data->push_back(static_cast<uint8_t>(value & 0xffU));
}

void append_be24(std::vector<uint8_t> *data, uint32_t value)
{
    data->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    data->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    data->push_back(static_cast<uint8_t>(value & 0xffU));
}

void append_extension(
    std::vector<uint8_t> *extensions,
    uint16_t type,
    const std::vector<uint8_t> &value)
{
    append_be16(extensions, type);
    append_be16(
        extensions, static_cast<uint16_t>(value.size()));
    extensions->insert(
        extensions->end(), value.begin(), value.end());
}

std::vector<uint8_t> make_client_hello_record(bool tls13)
{
    std::vector<uint8_t> body;
    append_be16(&body, 0x0303U);
    body.insert(body.end(), 32U, 0x11U);
    body.push_back(4U);
    body.insert(body.end(), {1U, 2U, 3U, 4U});
    append_be16(&body, 6U);
    body.insert(body.end(), {
        0x13U, 0x01U, 0x13U, 0x02U, 0x00U, 0x9cU});
    body.push_back(1U);
    body.push_back(0U);

    std::vector<uint8_t> extensions;
    std::vector<uint8_t> sni;
    append_be16(&sni, 14U);
    sni.push_back(0U);
    append_be16(&sni, 11U);
    const auto hostname = bytes("example.com");
    sni.insert(sni.end(), hostname.begin(), hostname.end());
    append_extension(&extensions, 0U, sni);

    std::vector<uint8_t> versions;
    versions.push_back(tls13 ? 4U : 2U);
    if (tls13) {
        append_be16(&versions, 0x0304U);
    }
    append_be16(&versions, 0x0303U);
    append_extension(&extensions, 43U, versions);

    std::vector<uint8_t> alpn;
    append_be16(&alpn, 12U);
    alpn.push_back(2U);
    alpn.insert(alpn.end(), {'h', '2'});
    alpn.push_back(8U);
    const auto http11 = bytes("http/1.1");
    alpn.insert(alpn.end(), http11.begin(), http11.end());
    append_extension(&extensions, 16U, alpn);

    append_extension(&extensions, 42U, {});
    const std::vector<uint8_t> psk{{
        0U, 8U, 0U, 2U, 'i', 'd', 0U, 0U, 0U, 0U,
        0U, 2U, 1U, 0U,
    }};
    append_extension(&extensions, 41U, psk);
    append_be16(
        &body, static_cast<uint16_t>(extensions.size()));
    body.insert(body.end(), extensions.begin(), extensions.end());

    std::vector<uint8_t> handshake;
    handshake.push_back(1U);
    append_be24(
        &handshake, static_cast<uint32_t>(body.size()));
    handshake.insert(handshake.end(), body.begin(), body.end());

    std::vector<uint8_t> record;
    record.insert(record.end(), {0x16U, 0x03U, 0x01U});
    append_be16(
        &record, static_cast<uint16_t>(handshake.size()));
    record.insert(record.end(), handshake.begin(), handshake.end());
    return record;
}

std::vector<uint8_t> make_tls_application_record(
    std::string_view plaintext_lookalike)
{
    std::vector<uint8_t> record{{0x17U, 0x03U, 0x03U}};
    append_be16(
        &record,
        static_cast<uint16_t>(plaintext_lookalike.size()));
    record.insert(
        record.end(),
        plaintext_lookalike.begin(),
        plaintext_lookalike.end());
    return record;
}

void append_be32(std::vector<uint8_t> *data, uint32_t value)
{
    data->push_back(static_cast<uint8_t>(value >> 24U));
    data->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    data->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    data->push_back(static_cast<uint8_t>(value & 0xffU));
}

void append_quic_varint(std::vector<uint8_t> *data, uint64_t value)
{
    if (value <= 63U) {
        data->push_back(static_cast<uint8_t>(value));
        return;
    }
    if (value <= 16383U) {
        data->push_back(static_cast<uint8_t>(
            0x40U | ((value >> 8U) & 0x3fU)));
        data->push_back(static_cast<uint8_t>(value & 0xffU));
        return;
    }
    if (value <= 1073741823ULL) {
        data->push_back(static_cast<uint8_t>(
            0x80U | ((value >> 24U) & 0x3fU)));
        data->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
        data->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
        data->push_back(static_cast<uint8_t>(value & 0xffU));
        return;
    }
    data->push_back(static_cast<uint8_t>(
        0xc0U | ((value >> 56U) & 0x3fU)));
    for (int shift = 48; shift >= 0; shift -= 8) {
        data->push_back(static_cast<uint8_t>(
            (value >> static_cast<uint32_t>(shift)) & 0xffU));
    }
}

uint8_t quic_wire_type(
    uint32_t version,
    ktag_proxy_features::quic_header_type type)
{
    using ktag_proxy_features::quic_header_type;
    if (version == ktag_proxy_features::KTAG_QUIC_VERSION_2) {
        switch (type) {
            case quic_header_type::initial:
                return 1U;
            case quic_header_type::zero_rtt:
                return 2U;
            case quic_header_type::handshake:
                return 3U;
            case quic_header_type::retry:
                return 0U;
            default:
                return 0U;
        }
    }
    switch (type) {
        case quic_header_type::initial:
            return 0U;
        case quic_header_type::zero_rtt:
            return 1U;
        case quic_header_type::handshake:
            return 2U;
        case quic_header_type::retry:
            return 3U;
        default:
            return 0U;
    }
}

std::vector<uint8_t> make_quic_long_packet(
    uint32_t version,
    ktag_proxy_features::quic_header_type type,
    size_t token_length = 3U)
{
    const uint8_t first = static_cast<uint8_t>(
        0xc0U | (quic_wire_type(version, type) << 4U));
    std::vector<uint8_t> packet{first};
    append_be32(&packet, version);
    packet.push_back(8U);
    packet.insert(packet.end(), 8U, 0xa1U);
    packet.push_back(4U);
    packet.insert(packet.end(), 4U, 0xb2U);

    using ktag_proxy_features::quic_header_type;
    if (type == quic_header_type::initial) {
        append_quic_varint(&packet, token_length);
        packet.insert(packet.end(), token_length, 0xccU);
        append_quic_varint(&packet, 5U);
        packet.insert(packet.end(), 5U, 0xddU);
    } else if (type == quic_header_type::zero_rtt ||
               type == quic_header_type::handshake) {
        append_quic_varint(&packet, 5U);
        packet.insert(packet.end(), 5U, 0xddU);
    } else if (type == quic_header_type::retry) {
        packet.insert(packet.end(), 3U, 0xeeU);
        packet.insert(packet.end(), 16U, 0xffU);
    }
    return packet;
}

std::vector<uint8_t> make_quic_version_negotiation()
{
    std::vector<uint8_t> packet{0x80U};
    append_be32(&packet, 0U);
    packet.push_back(4U);
    packet.insert(packet.end(), 4U, 0x11U);
    packet.push_back(2U);
    packet.insert(packet.end(), 2U, 0x22U);
    append_be32(&packet, ktag_proxy_features::KTAG_QUIC_VERSION_1);
    append_be32(&packet, ktag_proxy_features::KTAG_QUIC_VERSION_2);
    return packet;
}

std::vector<uint8_t> make_unknown_quic_long()
{
    std::vector<uint8_t> packet{0xc0U};
    append_be32(&packet, 0xfaceb00cU);
    packet.push_back(1U);
    packet.push_back(0x11U);
    packet.push_back(1U);
    packet.push_back(0x22U);
    packet.push_back(0U);
    return packet;
}

std::vector<uint8_t> make_quic_short_packet(uint8_t suffix = 1U)
{
    return std::vector<uint8_t>{0x40U, suffix, 0x55U, 0xaaU};
}

class sequence_harness {
public:
    sequence_harness()
    {
        ktag_feature_plugin_descriptor plugin{};
        plugin.struct_size = sizeof(plugin);
        plugin.api_version = KTAG_API_VERSION;
        if (ktag_feature_plugin_query(
                KTAG_FEATURE_PLUGIN_ABI_VERSION,
                &plugin) != KTAG_SUCCESS ||
            plugin.register_providers == nullptr) {
            return;
        }

        registry_ = ktag_feature_registry_create();
        if (registry_ == nullptr ||
            plugin.register_providers(nullptr, registry_) !=
                KTAG_SUCCESS) {
            return;
        }
        schema_ =
            ktag_feature_schema_create("proxy_sequence_test_v1", 1U);
        if (schema_ == nullptr) {
            return;
        }
        for (const auto &feature :
             ktag_proxy_features::KTAG_PROXY_SEQUENCE_FEATURES) {
            if (ktag_feature_schema_add_registered(
                    schema_, registry_, feature.feature_id) !=
                KTAG_SUCCESS) {
                return;
            }
        }
        if (ktag_feature_schema_freeze(schema_) != KTAG_SUCCESS) {
            return;
        }

        ktag_feature_config config{};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.schema = schema_;
        config.registry = registry_;
        config.flow_capacity = 8U;
        config.udp_packet_limit = 1024U;
        config.lifecycle_mode =
            KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
        context_ = ktag_feature_create(&config);
        batch_ = ktag_feature_batch_create(schema_, 1U);
        valid_ = context_ != nullptr && batch_ != nullptr;
    }

    ~sequence_harness()
    {
        ktag_feature_batch_destroy(batch_);
        ktag_feature_destroy(context_);
        ktag_feature_schema_destroy(schema_);
        ktag_feature_registry_destroy(registry_);
    }

    sequence_harness(const sequence_harness &) = delete;
    sequence_harness &operator=(const sequence_harness &) = delete;

    bool valid() const
    {
        return valid_;
    }

    bool process(
        const std::vector<uint8_t> &frame,
        uint64_t sequence,
        int64_t seconds,
        int32_t microseconds)
    {
        if (!valid_ || frame.empty() || finished_) {
            return false;
        }
        ktag_packet packet{};
        ktag_packet_init(&packet);
        packet.data = frame.data();
        packet.captured_length =
            static_cast<uint32_t>(frame.size());
        packet.original_length =
            static_cast<uint32_t>(frame.size());
        packet.link_type = KTAG_CAPTURE_LINKTYPE_ETHERNET;
        packet.sequence = sequence;
        packet.timestamp.seconds = seconds;
        packet.timestamp.microseconds = microseconds;
        return ktag_feature_process_packet(
                   context_, &packet, batch_) == KTAG_SUCCESS &&
            ktag_feature_batch_get_count(batch_) == 0U;
    }

    bool finish()
    {
        if (!valid_ || finished_ ||
            ktag_feature_flush(context_, batch_) != KTAG_SUCCESS ||
            ktag_feature_batch_get_count(batch_) != 1U ||
            ktag_feature_batch_get_record(
                batch_, 0U, &record_) != KTAG_SUCCESS) {
            return false;
        }
        finished_ = true;
        return record_.value_count == 148U;
    }

    const float *feature(
        size_t catalog_index,
        uint32_t expected_dimension) const
    {
        if (!finished_ ||
            catalog_index >=
                ktag_proxy_features::
                    KTAG_PROXY_SEQUENCE_FEATURES.size()) {
            return nullptr;
        }
        ktag_feature_descriptor descriptor{};
        uint32_t offset = 0U;
        if (ktag_feature_schema_find(
                schema_,
                ktag_proxy_features::KTAG_PROXY_SEQUENCE_FEATURES
                    [catalog_index].feature_id,
                &descriptor,
                &offset) != KTAG_SUCCESS ||
            descriptor.dimension != expected_dimension ||
            offset > record_.value_count ||
            expected_dimension > record_.value_count - offset) {
            return nullptr;
        }
        return record_.values + offset;
    }

    std::vector<float> values() const
    {
        return finished_ ?
            std::vector<float>(
                record_.values,
                record_.values + record_.value_count) :
            std::vector<float>{};
    }

private:
    ktag_feature_registry *registry_ = nullptr;
    ktag_feature_schema *schema_ = nullptr;
    ktag_feature_context *context_ = nullptr;
    ktag_feature_batch *batch_ = nullptr;
    ktag_feature_record record_{};
    bool valid_ = false;
    bool finished_ = false;
};

class tcp_signature_harness {
public:
    tcp_signature_harness()
    {
        ktag_feature_plugin_descriptor plugin{};
        plugin.struct_size = sizeof(plugin);
        plugin.api_version = KTAG_API_VERSION;
        if (ktag_feature_plugin_query(
                KTAG_FEATURE_PLUGIN_ABI_VERSION,
                &plugin) != KTAG_SUCCESS ||
            plugin.register_providers == nullptr) {
            return;
        }

        registry_ = ktag_feature_registry_create();
        if (registry_ == nullptr ||
            plugin.register_providers(nullptr, registry_) !=
                KTAG_SUCCESS) {
            return;
        }
        schema_ = ktag_feature_schema_create(
            "proxy_tcp_signature_test_v1", 1U);
        if (schema_ == nullptr) {
            return;
        }
        for (const auto &feature :
             ktag_proxy_features::KTAG_PROXY_TCP_SIGNATURE_FEATURES) {
            if (ktag_feature_schema_add_registered(
                    schema_, registry_, feature.feature_id) !=
                KTAG_SUCCESS) {
                return;
            }
        }
        if (ktag_feature_schema_freeze(schema_) != KTAG_SUCCESS) {
            return;
        }

        ktag_feature_config config{};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.schema = schema_;
        config.registry = registry_;
        config.flow_capacity = 8U;
        config.udp_packet_limit = 1024U;
        config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
        context_ = ktag_feature_create(&config);
        batch_ = ktag_feature_batch_create(schema_, 1U);
        valid_ = context_ != nullptr && batch_ != nullptr;
    }

    ~tcp_signature_harness()
    {
        ktag_feature_batch_destroy(batch_);
        ktag_feature_destroy(context_);
        ktag_feature_schema_destroy(schema_);
        ktag_feature_registry_destroy(registry_);
    }

    tcp_signature_harness(const tcp_signature_harness &) = delete;
    tcp_signature_harness &operator=(
        const tcp_signature_harness &) = delete;

    bool valid() const
    {
        return valid_;
    }

    bool process(
        const std::vector<uint8_t> &frame,
        uint64_t sequence,
        int64_t seconds = 1,
        int32_t microseconds = 0)
    {
        if (!valid_ || frame.empty() || finished_) {
            return false;
        }
        ktag_packet packet{};
        ktag_packet_init(&packet);
        packet.data = frame.data();
        packet.captured_length =
            static_cast<uint32_t>(frame.size());
        packet.original_length =
            static_cast<uint32_t>(frame.size());
        packet.link_type = KTAG_CAPTURE_LINKTYPE_ETHERNET;
        packet.sequence = sequence;
        packet.timestamp.seconds = seconds;
        packet.timestamp.microseconds = microseconds;
        return ktag_feature_process_packet(
                   context_, &packet, batch_) == KTAG_SUCCESS &&
            ktag_feature_batch_get_count(batch_) == 0U;
    }

    bool finish()
    {
        if (!valid_ || finished_ ||
            ktag_feature_flush(context_, batch_) != KTAG_SUCCESS ||
            ktag_feature_batch_get_count(batch_) != 1U ||
            ktag_feature_batch_get_record(
                batch_, 0U, &record_) != KTAG_SUCCESS) {
            return false;
        }
        finished_ = true;
        return record_.value_count == 52U;
    }

    const float *feature(
        size_t catalog_index,
        uint32_t expected_dimension = 1U) const
    {
        if (!finished_ ||
            catalog_index >=
                ktag_proxy_features::
                    KTAG_PROXY_TCP_SIGNATURE_FEATURES.size()) {
            return nullptr;
        }
        ktag_feature_descriptor descriptor{};
        uint32_t offset = 0U;
        if (ktag_feature_schema_find(
                schema_,
                ktag_proxy_features::
                    KTAG_PROXY_TCP_SIGNATURE_FEATURES
                        [catalog_index].feature_id,
                &descriptor,
                &offset) != KTAG_SUCCESS ||
            descriptor.dimension != expected_dimension ||
            offset > record_.value_count ||
            expected_dimension > record_.value_count - offset) {
            return nullptr;
        }
        return record_.values + offset;
    }

    std::vector<float> values() const
    {
        return finished_ ?
            std::vector<float>(
                record_.values,
                record_.values + record_.value_count) :
            std::vector<float>{};
    }

private:
    ktag_feature_registry *registry_ = nullptr;
    ktag_feature_schema *schema_ = nullptr;
    ktag_feature_context *context_ = nullptr;
    ktag_feature_batch *batch_ = nullptr;
    ktag_feature_record record_{};
    bool valid_ = false;
    bool finished_ = false;
};

} // namespace
