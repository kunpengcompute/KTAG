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

class quic_harness {
public:
    quic_harness()
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
            "proxy_quic_test_v1", 1U);
        if (schema_ == nullptr) {
            return;
        }
        for (const auto &feature :
             ktag_proxy_features::KTAG_PROXY_QUIC_FEATURES) {
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

    ~quic_harness()
    {
        ktag_feature_batch_destroy(batch_);
        ktag_feature_destroy(context_);
        ktag_feature_schema_destroy(schema_);
        ktag_feature_registry_destroy(registry_);
    }

    quic_harness(const quic_harness &) = delete;
    quic_harness &operator=(const quic_harness &) = delete;

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
        return record_.value_count == 68U;
    }

    bool finish_empty()
    {
        if (!valid_ || finished_ ||
            ktag_feature_flush(context_, batch_) != KTAG_SUCCESS) {
            return false;
        }
        finished_ = true;
        return ktag_feature_batch_get_count(batch_) == 0U;
    }

    const float *feature(
        size_t catalog_index,
        uint32_t expected_dimension = 1U) const
    {
        if (!finished_ ||
            catalog_index >=
                ktag_proxy_features::KTAG_PROXY_QUIC_FEATURES.size()) {
            return nullptr;
        }
        ktag_feature_descriptor descriptor{};
        uint32_t offset = 0U;
        if (ktag_feature_schema_find(
                schema_,
                ktag_proxy_features::KTAG_PROXY_QUIC_FEATURES
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
        return finished_ && record_.values != nullptr ?
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

float expected_log2(uint64_t value)
{
    return static_cast<float>(
        std::log2(1.0 + static_cast<double>(value)));
}

TEST(KTAGProxySequence, ExtractsSignedTcpSequenceAndBursts)
{
    sequence_harness harness;
    ASSERT_TRUE(harness.valid());
    ASSERT_TRUE(harness.process(
        make_transport_frame(false, true, false, 10U, 0x18U, 1U),
        1U, 1, 0));
    ASSERT_TRUE(harness.process(
        make_transport_frame(false, true, false, 0U, 0x10U, 2U),
        2U, 1, 0));
    ASSERT_TRUE(harness.process(
        make_transport_frame(false, true, true, 20U, 0x18U, 1U),
        3U, 1, 100));
    ASSERT_TRUE(harness.process(
        make_transport_frame(false, true, true, 5U, 0x10U, 2U),
        4U, 1, 300));
    ASSERT_TRUE(harness.process(
        make_transport_frame(false, true, false, 15U, 0x18U, 3U),
        5U, 1, 400));
    ASSERT_TRUE(harness.finish());

    const float *payload =
        harness.feature(SIGNED_PAYLOAD_LENGTH, 32U);
    const float *time =
        harness.feature(RELATIVE_TIME_LOG2_US, 32U);
    const float *flags = harness.feature(TCP_FLAGS, 32U);
    const float *sequence_count =
        harness.feature(SEQUENCE_VALID_COUNT, 1U);
    const float *burst_packets =
        harness.feature(BURST_SIGNED_PACKET_COUNT, 16U);
    const float *burst_payload =
        harness.feature(BURST_SIGNED_PAYLOAD_BYTES_LOG2, 16U);
    const float *burst_duration =
        harness.feature(BURST_DURATION_LOG2_US, 16U);
    const float *burst_count =
        harness.feature(BURST_VALID_COUNT, 1U);
    const float *switch_count =
        harness.feature(DIRECTION_SWITCH_COUNT, 1U);
    const float *ratio =
        harness.feature(FORWARD_PAYLOAD_RATIO, 1U);
    ASSERT_NE(nullptr, payload);
    ASSERT_NE(nullptr, time);
    ASSERT_NE(nullptr, flags);
    ASSERT_NE(nullptr, sequence_count);
    ASSERT_NE(nullptr, burst_packets);
    ASSERT_NE(nullptr, burst_payload);
    ASSERT_NE(nullptr, burst_duration);
    ASSERT_NE(nullptr, burst_count);
    ASSERT_NE(nullptr, switch_count);
    ASSERT_NE(nullptr, ratio);

    const std::array<float, 5U> expected_payload{{
        10.0F, 0.0F, -20.0F, -5.0F, 15.0F}};
    const std::array<float, 5U> expected_flags{{
        24.0F, 16.0F, -24.0F, -16.0F, 24.0F}};
    for (size_t index = 0U; index < expected_payload.size(); ++index) {
        EXPECT_FLOAT_EQ(expected_payload[index], payload[index]);
        EXPECT_FLOAT_EQ(expected_flags[index], flags[index]);
    }
    EXPECT_FLOAT_EQ(0.0F, time[0U]);
    EXPECT_FLOAT_EQ(0.0F, time[1U]);
    EXPECT_FLOAT_EQ(expected_log2(100U), time[2U]);
    EXPECT_FLOAT_EQ(expected_log2(300U), time[3U]);
    EXPECT_FLOAT_EQ(expected_log2(400U), time[4U]);
    EXPECT_FLOAT_EQ(5.0F, sequence_count[0U]);
    for (size_t index = 5U; index < 32U; ++index) {
        EXPECT_FLOAT_EQ(0.0F, payload[index]);
        EXPECT_FLOAT_EQ(0.0F, time[index]);
        EXPECT_FLOAT_EQ(0.0F, flags[index]);
    }

    EXPECT_FLOAT_EQ(2.0F, burst_packets[0U]);
    EXPECT_FLOAT_EQ(-2.0F, burst_packets[1U]);
    EXPECT_FLOAT_EQ(1.0F, burst_packets[2U]);
    EXPECT_FLOAT_EQ(expected_log2(10U), burst_payload[0U]);
    EXPECT_FLOAT_EQ(-expected_log2(25U), burst_payload[1U]);
    EXPECT_FLOAT_EQ(expected_log2(15U), burst_payload[2U]);
    EXPECT_FLOAT_EQ(0.0F, burst_duration[0U]);
    EXPECT_FLOAT_EQ(expected_log2(200U), burst_duration[1U]);
    EXPECT_FLOAT_EQ(0.0F, burst_duration[2U]);
    EXPECT_FLOAT_EQ(3.0F, burst_count[0U]);
    EXPECT_FLOAT_EQ(2.0F, switch_count[0U]);
    EXPECT_FLOAT_EQ(0.5F, ratio[0U]);
}

TEST(KTAGProxySequence, ProducesSameFeaturesAcrossIpVersions)
{
    sequence_harness ipv4;
    sequence_harness ipv6;
    ASSERT_TRUE(ipv4.valid());
    ASSERT_TRUE(ipv6.valid());
    for (auto *harness : {&ipv4, &ipv6}) {
        const bool use_ipv6 = harness == &ipv6;
        ASSERT_TRUE(harness->process(
            make_transport_frame(
                use_ipv6, false, false, 7U),
            1U, 2, 0));
        ASSERT_TRUE(harness->process(
            make_transport_frame(
                use_ipv6, false, true, 9U),
            2U, 2, 10));
        ASSERT_TRUE(harness->process(
            make_transport_frame(
                use_ipv6, false, false, 0U),
            3U, 2, 10));
        ASSERT_TRUE(harness->finish());
    }

    const auto ipv4_values = ipv4.values();
    const auto ipv6_values = ipv6.values();
    ASSERT_EQ(148U, ipv4_values.size());
    ASSERT_EQ(ipv4_values.size(), ipv6_values.size());
    for (size_t index = 0U; index < ipv4_values.size(); ++index) {
        EXPECT_FLOAT_EQ(ipv4_values[index], ipv6_values[index])
            << index;
    }

    const float *payload =
        ipv4.feature(SIGNED_PAYLOAD_LENGTH, 32U);
    const float *flags = ipv4.feature(TCP_FLAGS, 32U);
    ASSERT_NE(nullptr, payload);
    ASSERT_NE(nullptr, flags);
    EXPECT_FLOAT_EQ(7.0F, payload[0U]);
    EXPECT_FLOAT_EQ(-9.0F, payload[1U]);
    EXPECT_FLOAT_EQ(0.0F, payload[2U]);
    for (size_t index = 0U; index < 32U; ++index) {
        EXPECT_FLOAT_EQ(0.0F, flags[index]);
    }

    sequence_harness tcp_ipv4;
    sequence_harness tcp_ipv6;
    ASSERT_TRUE(tcp_ipv4.valid());
    ASSERT_TRUE(tcp_ipv6.valid());
    for (auto *harness : {&tcp_ipv4, &tcp_ipv6}) {
        const bool use_ipv6 = harness == &tcp_ipv6;
        ASSERT_TRUE(harness->process(
            make_transport_frame(
                use_ipv6, true, false, 11U, 0x18U, 1U),
            1U, 5, 0));
        ASSERT_TRUE(harness->process(
            make_transport_frame(
                use_ipv6, true, true, 13U, 0x10U, 1U),
            2U, 5, 20));
        ASSERT_TRUE(harness->finish());
    }
    const auto tcp_ipv4_values = tcp_ipv4.values();
    const auto tcp_ipv6_values = tcp_ipv6.values();
    ASSERT_EQ(tcp_ipv4_values.size(), tcp_ipv6_values.size());
    for (size_t index = 0U; index < tcp_ipv4_values.size(); ++index) {
        EXPECT_FLOAT_EQ(
            tcp_ipv4_values[index], tcp_ipv6_values[index])
            << index;
    }
}

TEST(KTAGProxySequence, EnforcesPacketAndBurstCaps)
{
    sequence_harness exactly_32;
    ASSERT_TRUE(exactly_32.valid());
    for (uint32_t index = 0U; index < 32U; ++index) {
        ASSERT_TRUE(exactly_32.process(
            make_transport_frame(false, false, false, 2U),
            index + 1U, 3, static_cast<int32_t>(index)));
    }
    ASSERT_TRUE(exactly_32.finish());
    const float *exact_sequence_count =
        exactly_32.feature(SEQUENCE_VALID_COUNT, 1U);
    const float *exact_burst_count =
        exactly_32.feature(BURST_VALID_COUNT, 1U);
    const float *exact_burst_packets =
        exactly_32.feature(BURST_SIGNED_PACKET_COUNT, 16U);
    ASSERT_NE(nullptr, exact_sequence_count);
    ASSERT_NE(nullptr, exact_burst_count);
    ASSERT_NE(nullptr, exact_burst_packets);
    EXPECT_FLOAT_EQ(32.0F, exact_sequence_count[0U]);
    EXPECT_FLOAT_EQ(1.0F, exact_burst_count[0U]);
    EXPECT_FLOAT_EQ(32.0F, exact_burst_packets[0U]);

    sequence_harness over_limit;
    ASSERT_TRUE(over_limit.valid());
    for (uint32_t index = 0U; index < 33U; ++index) {
        ASSERT_TRUE(over_limit.process(
            make_transport_frame(
                false,
                false,
                (index & 1U) != 0U,
                1U),
            index + 1U, 4, static_cast<int32_t>(index)));
    }
    ASSERT_TRUE(over_limit.finish());
    const float *payload =
        over_limit.feature(SIGNED_PAYLOAD_LENGTH, 32U);
    const float *sequence_count =
        over_limit.feature(SEQUENCE_VALID_COUNT, 1U);
    const float *burst_packets =
        over_limit.feature(BURST_SIGNED_PACKET_COUNT, 16U);
    const float *burst_count =
        over_limit.feature(BURST_VALID_COUNT, 1U);
    const float *switch_count =
        over_limit.feature(DIRECTION_SWITCH_COUNT, 1U);
    const float *ratio =
        over_limit.feature(FORWARD_PAYLOAD_RATIO, 1U);
    ASSERT_NE(nullptr, payload);
    ASSERT_NE(nullptr, sequence_count);
    ASSERT_NE(nullptr, burst_packets);
    ASSERT_NE(nullptr, burst_count);
    ASSERT_NE(nullptr, switch_count);
    ASSERT_NE(nullptr, ratio);
    EXPECT_FLOAT_EQ(32.0F, sequence_count[0U]);
    EXPECT_FLOAT_EQ(16.0F, burst_count[0U]);
    EXPECT_FLOAT_EQ(31.0F, switch_count[0U]);
    for (uint32_t index = 0U; index < 32U; ++index) {
        EXPECT_FLOAT_EQ(
            (index & 1U) == 0U ? 1.0F : -1.0F,
            payload[index]);
    }
    for (uint32_t index = 0U; index < 16U; ++index) {
        EXPECT_FLOAT_EQ(
            (index & 1U) == 0U ? 1.0F : -1.0F,
            burst_packets[index]);
    }
    EXPECT_NEAR(17.0F / 33.0F, ratio[0U], 1e-6F);
}

TEST(KTAGProxySequence, ClampsReversedTimeAndHandlesLargePayload)
{
    sequence_harness timing;
    ASSERT_TRUE(timing.valid());
    ASSERT_TRUE(timing.process(
        make_transport_frame(false, false, false, 1U),
        1U, 10, 500000));
    ASSERT_TRUE(timing.process(
        make_transport_frame(false, false, false, 1U),
        2U, 10, 500000));
    ASSERT_TRUE(timing.process(
        make_transport_frame(false, false, false, 1U),
        3U, 9, 500000));
    ASSERT_TRUE(timing.finish());
    const float *time =
        timing.feature(RELATIVE_TIME_LOG2_US, 32U);
    const float *duration =
        timing.feature(BURST_DURATION_LOG2_US, 16U);
    ASSERT_NE(nullptr, time);
    ASSERT_NE(nullptr, duration);
    EXPECT_FLOAT_EQ(0.0F, time[0U]);
    EXPECT_FLOAT_EQ(0.0F, time[1U]);
    EXPECT_FLOAT_EQ(0.0F, time[2U]);
    EXPECT_FLOAT_EQ(0.0F, duration[0U]);

    sequence_harness large_payload;
    ASSERT_TRUE(large_payload.valid());
    ASSERT_TRUE(large_payload.process(
        make_transport_frame(false, false, false, 60000U),
        1U, 11, 0));
    ASSERT_TRUE(large_payload.process(
        make_transport_frame(false, false, true, 5000U),
        2U, 11, 1));
    ASSERT_TRUE(large_payload.finish());
    const float *payload =
        large_payload.feature(SIGNED_PAYLOAD_LENGTH, 32U);
    const float *burst_payload =
        large_payload.feature(
            BURST_SIGNED_PAYLOAD_BYTES_LOG2, 16U);
    ASSERT_NE(nullptr, payload);
    ASSERT_NE(nullptr, burst_payload);
    EXPECT_FLOAT_EQ(60000.0F, payload[0U]);
    EXPECT_FLOAT_EQ(-5000.0F, payload[1U]);
    EXPECT_TRUE(std::isfinite(burst_payload[0U]));
    EXPECT_TRUE(std::isfinite(burst_payload[1U]));
}

TEST(KTAGProxyTcpSignature, ParsesTlsAcrossSegmentsAndIpVersions)
{
    const auto client_hello = make_client_hello_record(true);
    const auto server_record = make_tls_application_record("xyz");
    constexpr size_t SPLIT = 37U;
    ASSERT_LT(SPLIT, client_hello.size());
    const std::vector<uint8_t> first(
        client_hello.begin(), client_hello.begin() + SPLIT);
    const std::vector<uint8_t> second(
        client_hello.begin() + SPLIT, client_hello.end());

    tcp_signature_harness ipv4;
    ASSERT_TRUE(ipv4.valid());
    ASSERT_TRUE(ipv4.process(
        make_tcp_frame_with_payload(
            false, false, first, 1000U),
        1U));
    ASSERT_TRUE(ipv4.process(
        make_tcp_frame_with_payload(
            false,
            false,
            second,
            1000U + static_cast<uint32_t>(SPLIT)),
        2U));
    ASSERT_TRUE(ipv4.process(
        make_tcp_frame_with_payload(
            false, true, server_record, 5000U),
        3U));
    ASSERT_TRUE(ipv4.finish());

    tcp_signature_harness ipv6;
    ASSERT_TRUE(ipv6.valid());
    ASSERT_TRUE(ipv6.process(
        make_tcp_frame_with_payload(
            true, false, client_hello, 1000U, 0x18U, true),
        1U));
    ASSERT_TRUE(ipv6.process(
        make_tcp_frame_with_payload(
            true, true, server_record, 5000U, 0x18U, true),
        2U));
    ASSERT_TRUE(ipv6.finish());

    const auto ipv4_values = ipv4.values();
    const auto ipv6_values = ipv6.values();
    ASSERT_EQ(52U, ipv4_values.size());
    ASSERT_EQ(ipv4_values.size(), ipv6_values.size());
    for (size_t index = 0U; index < ipv4_values.size(); ++index) {
        EXPECT_FLOAT_EQ(ipv4_values[index], ipv6_values[index])
            << index;
    }

    const float *candidate = ipv4.feature(TLS_RECORD_CANDIDATE);
    const float *complete =
        ipv4.feature(TLS_CLIENT_HELLO_COMPLETE);
    const float *legacy = ipv4.feature(TLS_LEGACY_VERSION);
    const float *supported =
        ipv4.feature(TLS_SUPPORTED_VERSION_MAX);
    const float *cipher_count =
        ipv4.feature(TLS_CIPHER_SUITE_COUNT);
    const float *extension_count =
        ipv4.feature(TLS_EXTENSION_COUNT);
    const float *cipher_hash =
        ipv4.feature(TLS_CIPHER_SUITE_HASH_U16, 2U);
    const float *extension_hash =
        ipv4.feature(TLS_EXTENSION_ORDER_HASH_U16, 2U);
    const float *sni = ipv4.feature(TLS_SNI_PRESENT);
    const float *h2 = ipv4.feature(TLS_ALPN_H2_PRESENT);
    const float *http11 =
        ipv4.feature(TLS_ALPN_HTTP11_PRESENT);
    const float *session =
        ipv4.feature(TLS_SESSION_ID_LENGTH);
    const float *psk = ipv4.feature(TLS_PSK_PRESENT);
    const float *early = ipv4.feature(TLS_EARLY_DATA_PRESENT);
    const float *records =
        ipv4.feature(TLS_RECORD_SIGNED_LENGTH, 16U);
    const float *record_count =
        ipv4.feature(TLS_RECORD_VALID_COUNT);
    const float *failures =
        ipv4.feature(TLS_PARSE_FAILURE_COUNT);
    ASSERT_NE(nullptr, candidate);
    ASSERT_NE(nullptr, complete);
    ASSERT_NE(nullptr, legacy);
    ASSERT_NE(nullptr, supported);
    ASSERT_NE(nullptr, cipher_count);
    ASSERT_NE(nullptr, extension_count);
    ASSERT_NE(nullptr, cipher_hash);
    ASSERT_NE(nullptr, extension_hash);
    ASSERT_NE(nullptr, sni);
    ASSERT_NE(nullptr, h2);
    ASSERT_NE(nullptr, http11);
    ASSERT_NE(nullptr, session);
    ASSERT_NE(nullptr, psk);
    ASSERT_NE(nullptr, early);
    ASSERT_NE(nullptr, records);
    ASSERT_NE(nullptr, record_count);
    ASSERT_NE(nullptr, failures);

    EXPECT_FLOAT_EQ(1.0F, candidate[0U]);
    EXPECT_FLOAT_EQ(1.0F, complete[0U]);
    EXPECT_FLOAT_EQ(771.0F, legacy[0U]);
    EXPECT_FLOAT_EQ(772.0F, supported[0U]);
    EXPECT_FLOAT_EQ(3.0F, cipher_count[0U]);
    EXPECT_FLOAT_EQ(5.0F, extension_count[0U]);
    EXPECT_FLOAT_EQ(47982.0F, cipher_hash[0U]);
    EXPECT_FLOAT_EQ(14528.0F, cipher_hash[1U]);
    EXPECT_FLOAT_EQ(56644.0F, extension_hash[0U]);
    EXPECT_FLOAT_EQ(53229.0F, extension_hash[1U]);
    EXPECT_FLOAT_EQ(1.0F, sni[0U]);
    EXPECT_FLOAT_EQ(1.0F, h2[0U]);
    EXPECT_FLOAT_EQ(1.0F, http11[0U]);
    EXPECT_FLOAT_EQ(4.0F, session[0U]);
    EXPECT_FLOAT_EQ(1.0F, psk[0U]);
    EXPECT_FLOAT_EQ(1.0F, early[0U]);
    EXPECT_FLOAT_EQ(
        static_cast<float>(client_hello.size() - 5U), records[0U]);
    EXPECT_FLOAT_EQ(
        -static_cast<float>(server_record.size() - 5U), records[1U]);
    EXPECT_FLOAT_EQ(2.0F, record_count[0U]);
    EXPECT_FLOAT_EQ(0.0F, failures[0U]);
    for (size_t index = 2U; index < 16U; ++index) {
        EXPECT_FLOAT_EQ(0.0F, records[index]);
    }

    tcp_signature_harness tls12;
    ASSERT_TRUE(tls12.valid());
    ASSERT_TRUE(tls12.process(
        make_tcp_frame_with_payload(
            false, false, make_client_hello_record(false), 2000U),
        1U));
    ASSERT_TRUE(tls12.finish());
    const float *tls12_supported =
        tls12.feature(TLS_SUPPORTED_VERSION_MAX);
    ASSERT_NE(nullptr, tls12_supported);
    EXPECT_FLOAT_EQ(771.0F, tls12_supported[0U]);
}

TEST(KTAGProxyTcpSignature, TracksTcpPrefixOrderingAndResourceCaps)
{
    tcp_signature_harness ordering;
    ASSERT_TRUE(ordering.valid());
    ASSERT_TRUE(ordering.process(
        make_tcp_frame_with_payload(
            false, false, bytes("ABCDE"), 100U),
        1U));
    ASSERT_TRUE(ordering.process(
        make_tcp_frame_with_payload(
            false, false, bytes("ABCDE"), 100U),
        2U));
    ASSERT_TRUE(ordering.process(
        make_tcp_frame_with_payload(
            false, false, bytes("KLMNO"), 110U),
        3U));
    ASSERT_TRUE(ordering.process(
        make_tcp_frame_with_payload(
            false, false, bytes("FGHIJ"), 105U),
        4U));
    ASSERT_TRUE(ordering.finish());

    const float *forward_length =
        ordering.feature(TCP_FORWARD_PREFIX_LENGTH_LOG2);
    const float *reverse_length =
        ordering.feature(TCP_REVERSE_PREFIX_LENGTH_LOG2);
    const float *gap = ordering.feature(TCP_SEGMENT_GAP_COUNT);
    const float *retransmission =
        ordering.feature(TCP_RETRANSMISSION_COUNT);
    const float *out_of_order =
        ordering.feature(TCP_OUT_OF_ORDER_COUNT);
    const float *entropy = ordering.feature(TCP_FORWARD_ENTROPY);
    const float *printable =
        ordering.feature(TCP_FORWARD_PRINTABLE_RATIO);
    const float *zeros = ordering.feature(TCP_FORWARD_ZERO_RATIO);
    ASSERT_NE(nullptr, forward_length);
    ASSERT_NE(nullptr, reverse_length);
    ASSERT_NE(nullptr, gap);
    ASSERT_NE(nullptr, retransmission);
    ASSERT_NE(nullptr, out_of_order);
    ASSERT_NE(nullptr, entropy);
    ASSERT_NE(nullptr, printable);
    ASSERT_NE(nullptr, zeros);
    EXPECT_FLOAT_EQ(expected_log2(15U), forward_length[0U]);
    EXPECT_FLOAT_EQ(0.0F, reverse_length[0U]);
    EXPECT_FLOAT_EQ(1.0F, gap[0U]);
    EXPECT_FLOAT_EQ(1.0F, retransmission[0U]);
    EXPECT_FLOAT_EQ(1.0F, out_of_order[0U]);
    EXPECT_NEAR(std::log2(15.0), entropy[0U], 1e-6F);
    EXPECT_FLOAT_EQ(1.0F, printable[0U]);
    EXPECT_FLOAT_EQ(0.0F, zeros[0U]);

    tcp_signature_harness byte_cap;
    ASSERT_TRUE(byte_cap.valid());
    const std::vector<uint8_t> zero_prefix(16U * 1024U, 0U);
    ASSERT_TRUE(byte_cap.process(
        make_tcp_frame_with_payload(
            false, false, zero_prefix, 1000U),
        1U));
    ASSERT_TRUE(byte_cap.finish());
    const float *truncated =
        byte_cap.feature(TCP_FORWARD_PREFIX_TRUNCATED);
    const float *zero_ratio_value =
        byte_cap.feature(TCP_FORWARD_ZERO_RATIO);
    const float *zero_entropy =
        byte_cap.feature(TCP_FORWARD_ENTROPY);
    ASSERT_NE(nullptr, truncated);
    ASSERT_NE(nullptr, zero_ratio_value);
    ASSERT_NE(nullptr, zero_entropy);
    EXPECT_FLOAT_EQ(1.0F, truncated[0U]);
    EXPECT_FLOAT_EQ(1.0F, zero_ratio_value[0U]);
    EXPECT_FLOAT_EQ(0.0F, zero_entropy[0U]);

    tcp_signature_harness segment_cap;
    ASSERT_TRUE(segment_cap.valid());
    for (uint32_t index = 0U; index < 65U; ++index) {
        ASSERT_TRUE(segment_cap.process(
            make_tcp_frame_with_payload(
                false,
                false,
                std::vector<uint8_t>{static_cast<uint8_t>(index)},
                3000U + index),
            index + 1U));
    }
    ASSERT_TRUE(segment_cap.finish());
    const float *capped_length =
        segment_cap.feature(TCP_FORWARD_PREFIX_LENGTH_LOG2);
    const float *capped_truncated =
        segment_cap.feature(TCP_FORWARD_PREFIX_TRUNCATED);
    ASSERT_NE(nullptr, capped_length);
    ASSERT_NE(nullptr, capped_truncated);
    EXPECT_FLOAT_EQ(expected_log2(64U), capped_length[0U]);
    EXPECT_FLOAT_EQ(0.0F, capped_truncated[0U]);
}

TEST(KTAGProxyTcpSignature, HandlesMalformedTruncatedAndRandomTls)
{
    tcp_signature_harness random;
    ASSERT_TRUE(random.valid());
    ASSERT_TRUE(random.process(
        make_tcp_frame_with_payload(
            false,
            false,
            std::vector<uint8_t>{1U, 2U, 3U, 4U, 5U},
            100U),
        1U));
    ASSERT_TRUE(random.finish());
    const float *random_candidate =
        random.feature(TLS_RECORD_CANDIDATE);
    const float *random_failure =
        random.feature(TLS_PARSE_FAILURE_COUNT);
    ASSERT_NE(nullptr, random_candidate);
    ASSERT_NE(nullptr, random_failure);
    EXPECT_FLOAT_EQ(0.0F, random_candidate[0U]);
    EXPECT_FLOAT_EQ(0.0F, random_failure[0U]);

    tcp_signature_harness truncated;
    ASSERT_TRUE(truncated.valid());
    const std::vector<uint8_t> partial{{
        0x16U, 0x03U, 0x03U, 0x00U, 0x20U, 0x01U,
    }};
    ASSERT_TRUE(truncated.process(
        make_tcp_frame_with_payload(
            false, false, partial, 200U),
        1U));
    ASSERT_TRUE(truncated.finish());
    const float *truncated_candidate =
        truncated.feature(TLS_RECORD_CANDIDATE);
    const float *truncated_complete =
        truncated.feature(TLS_CLIENT_HELLO_COMPLETE);
    const float *truncated_failure =
        truncated.feature(TLS_PARSE_FAILURE_COUNT);
    ASSERT_NE(nullptr, truncated_candidate);
    ASSERT_NE(nullptr, truncated_complete);
    ASSERT_NE(nullptr, truncated_failure);
    EXPECT_FLOAT_EQ(1.0F, truncated_candidate[0U]);
    EXPECT_FLOAT_EQ(0.0F, truncated_complete[0U]);
    EXPECT_FLOAT_EQ(1.0F, truncated_failure[0U]);

    auto malformed = make_client_hello_record(true);
    constexpr size_t EXTENSIONS_LENGTH_OFFSET = 58U;
    ASSERT_GT(malformed.size(), EXTENSIONS_LENGTH_OFFSET + 1U);
    malformed[EXTENSIONS_LENGTH_OFFSET] = 0xffU;
    malformed[EXTENSIONS_LENGTH_OFFSET + 1U] = 0xffU;
    tcp_signature_harness bad_extension;
    ASSERT_TRUE(bad_extension.valid());
    ASSERT_TRUE(bad_extension.process(
        make_tcp_frame_with_payload(
            false, false, malformed, 300U),
        1U));
    ASSERT_TRUE(bad_extension.finish());
    const float *bad_complete =
        bad_extension.feature(TLS_CLIENT_HELLO_COMPLETE);
    const float *bad_failure =
        bad_extension.feature(TLS_PARSE_FAILURE_COUNT);
    ASSERT_NE(nullptr, bad_complete);
    ASSERT_NE(nullptr, bad_failure);
    EXPECT_FLOAT_EQ(0.0F, bad_complete[0U]);
    EXPECT_FLOAT_EQ(1.0F, bad_failure[0U]);
}

TEST(KTAGProxyTcpSignature, DetectsCleartextCarriersOnlyOutsideTls)
{
    const auto request = bytes(
        "POST /service HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Content-Type: application/grpc\r\n\r\n");
    std::vector<uint8_t> http2 = bytes(
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    http2.insert(http2.end(), {
        0U, 0U, 0U, 4U, 0U, 0U, 0U, 0U, 0U,
    });

    tcp_signature_harness cleartext;
    ASSERT_TRUE(cleartext.valid());
    ASSERT_TRUE(cleartext.process(
        make_tcp_frame_with_payload(
            false, false, request, 1000U),
        1U));
    ASSERT_TRUE(cleartext.process(
        make_tcp_frame_with_payload(
            false, true, http2, 2000U),
        2U));
    ASSERT_TRUE(cleartext.finish());
    for (size_t feature : {
             CARRIER_HTTP1_REQUEST,
             CARRIER_WEBSOCKET_UPGRADE,
             CARRIER_HTTP2_PREFACE,
             CARRIER_HTTP2_FRAME,
             CARRIER_GRPC_CONTENT_TYPE}) {
        const float *value = cleartext.feature(feature);
        ASSERT_NE(nullptr, value);
        EXPECT_FLOAT_EQ(1.0F, value[0U]) << feature;
    }

    tcp_signature_harness encrypted;
    ASSERT_TRUE(encrypted.valid());
    ASSERT_TRUE(encrypted.process(
        make_tcp_frame_with_payload(
            false,
            false,
            make_tls_application_record(
                "GET / HTTP/1.1\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\n\r\n"),
            3000U),
        1U));
    ASSERT_TRUE(encrypted.finish());
    const float *tls_candidate =
        encrypted.feature(TLS_RECORD_CANDIDATE);
    ASSERT_NE(nullptr, tls_candidate);
    EXPECT_FLOAT_EQ(1.0F, tls_candidate[0U]);
    for (size_t feature : {
             CARRIER_HTTP1_REQUEST,
             CARRIER_WEBSOCKET_UPGRADE,
             CARRIER_HTTP2_PREFACE,
             CARRIER_HTTP2_FRAME,
             CARRIER_GRPC_CONTENT_TYPE}) {
        const float *value = encrypted.feature(feature);
        ASSERT_NE(nullptr, value);
        EXPECT_FLOAT_EQ(0.0F, value[0U]) << feature;
    }
}

TEST(KTAGProxyQuic, ParsesV1V2VersionNegotiationAndOtherVersion)
{
    using ktag_proxy_features::quic_header_type;
    const auto v1_initial = make_quic_long_packet(
        ktag_proxy_features::KTAG_QUIC_VERSION_1,
        quic_header_type::initial,
        3U);
    const auto v2_initial = make_quic_long_packet(
        ktag_proxy_features::KTAG_QUIC_VERSION_2,
        quic_header_type::initial,
        64U);
    const auto negotiation = make_quic_version_negotiation();
    const auto other = make_unknown_quic_long();

    quic_harness harness;
    ASSERT_TRUE(harness.valid());
    ASSERT_TRUE(harness.process(
        make_udp_frame_with_payload(
            false, false, v1_initial),
        1U));
    ASSERT_TRUE(harness.process(
        make_udp_frame_with_payload(
            false, true, v2_initial),
        2U));
    ASSERT_TRUE(harness.process(
        make_udp_frame_with_payload(
            false, false, negotiation),
        3U));
    ASSERT_TRUE(harness.process(
        make_udp_frame_with_payload(
            false, false, other),
        4U));
    ASSERT_TRUE(harness.finish());

    const float *candidate =
        harness.feature(QUIC_PACKET_CANDIDATE_COUNT);
    const float *success =
        harness.feature(QUIC_PARSE_SUCCESS_COUNT);
    const float *failure =
        harness.feature(QUIC_PARSE_FAILURE_COUNT);
    const float *fixed = harness.feature(QUIC_FIXED_BIT_COUNT);
    const float *long_header =
        harness.feature(QUIC_LONG_HEADER_COUNT);
    const float *v1 = harness.feature(QUIC_VERSION_V1_COUNT);
    const float *v2 = harness.feature(QUIC_VERSION_V2_COUNT);
    const float *version_other =
        harness.feature(QUIC_VERSION_OTHER_COUNT);
    const float *version_negotiation =
        harness.feature(QUIC_VERSION_NEGOTIATION_COUNT);
    const float *initial = harness.feature(QUIC_INITIAL_COUNT);
    const float *first_initial =
        harness.feature(QUIC_FIRST_INITIAL_LENGTH_LOG2);
    const float *dcid = harness.feature(QUIC_FIRST_DCID_LENGTH);
    const float *scid = harness.feature(QUIC_FIRST_SCID_LENGTH);
    const float *token =
        harness.feature(QUIC_FIRST_TOKEN_LENGTH_LOG2);
    const float *lengths =
        harness.feature(QUIC_SIGNED_DATAGRAM_LENGTH, 16U);
    const float *types =
        harness.feature(QUIC_HEADER_TYPE_SEQUENCE, 16U);
    const float *sequence_count =
        harness.feature(QUIC_SEQUENCE_VALID_COUNT);
    ASSERT_NE(nullptr, candidate);
    ASSERT_NE(nullptr, success);
    ASSERT_NE(nullptr, failure);
    ASSERT_NE(nullptr, fixed);
    ASSERT_NE(nullptr, long_header);
    ASSERT_NE(nullptr, v1);
    ASSERT_NE(nullptr, v2);
    ASSERT_NE(nullptr, version_other);
    ASSERT_NE(nullptr, version_negotiation);
    ASSERT_NE(nullptr, initial);
    ASSERT_NE(nullptr, first_initial);
    ASSERT_NE(nullptr, dcid);
    ASSERT_NE(nullptr, scid);
    ASSERT_NE(nullptr, token);
    ASSERT_NE(nullptr, lengths);
    ASSERT_NE(nullptr, types);
    ASSERT_NE(nullptr, sequence_count);

    EXPECT_FLOAT_EQ(4.0F, candidate[0U]);
    EXPECT_FLOAT_EQ(4.0F, success[0U]);
    EXPECT_FLOAT_EQ(0.0F, failure[0U]);
    EXPECT_FLOAT_EQ(3.0F, fixed[0U]);
    EXPECT_FLOAT_EQ(4.0F, long_header[0U]);
    EXPECT_FLOAT_EQ(1.0F, v1[0U]);
    EXPECT_FLOAT_EQ(1.0F, v2[0U]);
    EXPECT_FLOAT_EQ(1.0F, version_other[0U]);
    EXPECT_FLOAT_EQ(1.0F, version_negotiation[0U]);
    EXPECT_FLOAT_EQ(2.0F, initial[0U]);
    EXPECT_FLOAT_EQ(
        expected_log2(v1_initial.size()), first_initial[0U]);
    EXPECT_FLOAT_EQ(8.0F, dcid[0U]);
    EXPECT_FLOAT_EQ(4.0F, scid[0U]);
    EXPECT_FLOAT_EQ(expected_log2(3U), token[0U]);
    EXPECT_FLOAT_EQ(
        static_cast<float>(v1_initial.size()), lengths[0U]);
    EXPECT_FLOAT_EQ(
        -static_cast<float>(v2_initial.size()), lengths[1U]);
    EXPECT_FLOAT_EQ(
        static_cast<float>(negotiation.size()), lengths[2U]);
    EXPECT_FLOAT_EQ(
        static_cast<float>(other.size()), lengths[3U]);
    EXPECT_FLOAT_EQ(2.0F, types[0U]);
    EXPECT_FLOAT_EQ(2.0F, types[1U]);
    EXPECT_FLOAT_EQ(1.0F, types[2U]);
    EXPECT_FLOAT_EQ(0.0F, types[3U]);
    EXPECT_FLOAT_EQ(4.0F, sequence_count[0U]);
}

TEST(KTAGProxyQuic, ClassifiesLongAndShortHeaderTypes)
{
    using ktag_proxy_features::quic_header_type;
    const std::array<std::pair<uint32_t, quic_header_type>, 6U>
        packets{{
            {ktag_proxy_features::KTAG_QUIC_VERSION_1,
             quic_header_type::zero_rtt},
            {ktag_proxy_features::KTAG_QUIC_VERSION_1,
             quic_header_type::handshake},
            {ktag_proxy_features::KTAG_QUIC_VERSION_1,
             quic_header_type::retry},
            {ktag_proxy_features::KTAG_QUIC_VERSION_2,
             quic_header_type::zero_rtt},
            {ktag_proxy_features::KTAG_QUIC_VERSION_2,
             quic_header_type::handshake},
            {ktag_proxy_features::KTAG_QUIC_VERSION_2,
             quic_header_type::retry},
        }};

    quic_harness harness;
    ASSERT_TRUE(harness.valid());
    uint64_t sequence = 1U;
    for (const auto &packet : packets) {
        ASSERT_TRUE(harness.process(
            make_udp_frame_with_payload(
                false,
                false,
                make_quic_long_packet(packet.first, packet.second)),
            sequence++));
    }
    ASSERT_TRUE(harness.process(
        make_udp_frame_with_payload(
            false, true, make_quic_short_packet()),
        sequence));
    ASSERT_TRUE(harness.finish());

    const float *candidate =
        harness.feature(QUIC_PACKET_CANDIDATE_COUNT);
    const float *success =
        harness.feature(QUIC_PARSE_SUCCESS_COUNT);
    const float *fixed = harness.feature(QUIC_FIXED_BIT_COUNT);
    const float *long_header =
        harness.feature(QUIC_LONG_HEADER_COUNT);
    const float *short_header =
        harness.feature(QUIC_SHORT_HEADER_COUNT);
    const float *v1 = harness.feature(QUIC_VERSION_V1_COUNT);
    const float *v2 = harness.feature(QUIC_VERSION_V2_COUNT);
    const float *zero_rtt = harness.feature(QUIC_ZERO_RTT_COUNT);
    const float *handshake = harness.feature(QUIC_HANDSHAKE_COUNT);
    const float *retry = harness.feature(QUIC_RETRY_COUNT);
    const float *types =
        harness.feature(QUIC_HEADER_TYPE_SEQUENCE, 16U);
    ASSERT_NE(nullptr, candidate);
    ASSERT_NE(nullptr, success);
    ASSERT_NE(nullptr, fixed);
    ASSERT_NE(nullptr, long_header);
    ASSERT_NE(nullptr, short_header);
    ASSERT_NE(nullptr, v1);
    ASSERT_NE(nullptr, v2);
    ASSERT_NE(nullptr, zero_rtt);
    ASSERT_NE(nullptr, handshake);
    ASSERT_NE(nullptr, retry);
    ASSERT_NE(nullptr, types);
    EXPECT_FLOAT_EQ(7.0F, candidate[0U]);
    EXPECT_FLOAT_EQ(7.0F, success[0U]);
    EXPECT_FLOAT_EQ(7.0F, fixed[0U]);
    EXPECT_FLOAT_EQ(6.0F, long_header[0U]);
    EXPECT_FLOAT_EQ(1.0F, short_header[0U]);
    EXPECT_FLOAT_EQ(3.0F, v1[0U]);
    EXPECT_FLOAT_EQ(3.0F, v2[0U]);
    EXPECT_FLOAT_EQ(2.0F, zero_rtt[0U]);
    EXPECT_FLOAT_EQ(2.0F, handshake[0U]);
    EXPECT_FLOAT_EQ(2.0F, retry[0U]);
    const std::array<float, 7U> expected{{
        3.0F, 4.0F, 5.0F, 3.0F, 4.0F, 5.0F, 6.0F,
    }};
    for (size_t index = 0U; index < expected.size(); ++index) {
        EXPECT_FLOAT_EQ(expected[index], types[index]);
    }
}

TEST(KTAGProxyQuic, HandlesMalformedRandomAndSalamanderPayloads)
{
    std::vector<uint8_t> bad_dcid{0xc0U};
    append_be32(
        &bad_dcid, ktag_proxy_features::KTAG_QUIC_VERSION_1);
    bad_dcid.push_back(21U);

    std::vector<uint8_t> bad_scid{0xc0U};
    append_be32(
        &bad_scid, ktag_proxy_features::KTAG_QUIC_VERSION_1);
    bad_scid.push_back(0U);
    bad_scid.push_back(21U);

    std::vector<uint8_t> bad_varint{0xc0U};
    append_be32(
        &bad_varint, ktag_proxy_features::KTAG_QUIC_VERSION_1);
    bad_varint.insert(bad_varint.end(), {0U, 0U, 0xc0U});

    std::vector<uint8_t> truncated_token{0xc0U};
    append_be32(
        &truncated_token, ktag_proxy_features::KTAG_QUIC_VERSION_1);
    truncated_token.insert(
        truncated_token.end(), {0U, 0U, 10U, 0xaaU, 0xbbU});

    std::vector<uint8_t> salamander(256U);
    for (size_t index = 0U; index < salamander.size(); ++index) {
        salamander[index] = static_cast<uint8_t>(index + 0x20U);
    }

    const std::array<std::vector<uint8_t>, 8U> payloads{{
        std::vector<uint8_t>{},
        std::vector<uint8_t>{1U, 2U, 3U, 4U},
        std::vector<uint8_t>{0xc0U},
        bad_dcid,
        bad_scid,
        bad_varint,
        truncated_token,
        salamander,
    }};
    quic_harness harness;
    ASSERT_TRUE(harness.valid());
    for (size_t index = 0U; index < payloads.size(); ++index) {
        ASSERT_TRUE(harness.process(
            make_udp_frame_with_payload(
                false, false, payloads[index]),
            index + 1U));
    }
    ASSERT_TRUE(harness.finish());

    const float *candidate =
        harness.feature(QUIC_PACKET_CANDIDATE_COUNT);
    const float *success =
        harness.feature(QUIC_PARSE_SUCCESS_COUNT);
    const float *failure =
        harness.feature(QUIC_PARSE_FAILURE_COUNT);
    const float *fixed = harness.feature(QUIC_FIXED_BIT_COUNT);
    const float *long_header =
        harness.feature(QUIC_LONG_HEADER_COUNT);
    const float *v1 = harness.feature(QUIC_VERSION_V1_COUNT);
    const float *initial = harness.feature(QUIC_INITIAL_COUNT);
    const float *sequence_count =
        harness.feature(QUIC_SEQUENCE_VALID_COUNT);
    const float *histogram =
        harness.feature(QUIC_FIRST_BYTE_HISTOGRAM, 16U);
    const float *entropy =
        harness.feature(QUIC_PAYLOAD_ENTROPY_MEAN);
    ASSERT_NE(nullptr, candidate);
    ASSERT_NE(nullptr, success);
    ASSERT_NE(nullptr, failure);
    ASSERT_NE(nullptr, fixed);
    ASSERT_NE(nullptr, long_header);
    ASSERT_NE(nullptr, v1);
    ASSERT_NE(nullptr, initial);
    ASSERT_NE(nullptr, sequence_count);
    ASSERT_NE(nullptr, histogram);
    ASSERT_NE(nullptr, entropy);
    EXPECT_FLOAT_EQ(5.0F, candidate[0U]);
    EXPECT_FLOAT_EQ(0.0F, success[0U]);
    EXPECT_FLOAT_EQ(5.0F, failure[0U]);
    EXPECT_FLOAT_EQ(5.0F, fixed[0U]);
    EXPECT_FLOAT_EQ(5.0F, long_header[0U]);
    EXPECT_FLOAT_EQ(4.0F, v1[0U]);
    EXPECT_FLOAT_EQ(4.0F, initial[0U]);
    EXPECT_FLOAT_EQ(8.0F, sequence_count[0U]);
    EXPECT_FLOAT_EQ(1.0F, histogram[0U]);
    EXPECT_FLOAT_EQ(1.0F, histogram[2U]);
    EXPECT_FLOAT_EQ(5.0F, histogram[12U]);
    EXPECT_GT(entropy[0U], 0.0F);
    EXPECT_LE(entropy[0U], 8.0F);
}

TEST(KTAGProxyQuic, EnforcesDatagramCapAndMatchesIpVersions)
{
    quic_harness capped;
    ASSERT_TRUE(capped.valid());
    for (uint32_t index = 0U; index < 65U; ++index) {
        ASSERT_TRUE(capped.process(
            make_udp_frame_with_payload(
                false,
                false,
                make_quic_short_packet(
                    static_cast<uint8_t>(index))),
            index + 1U));
    }
    ASSERT_TRUE(capped.finish());
    const float *candidate =
        capped.feature(QUIC_PACKET_CANDIDATE_COUNT);
    const float *success =
        capped.feature(QUIC_PARSE_SUCCESS_COUNT);
    const float *short_header =
        capped.feature(QUIC_SHORT_HEADER_COUNT);
    const float *sequence_count =
        capped.feature(QUIC_SEQUENCE_VALID_COUNT);
    const float *types =
        capped.feature(QUIC_HEADER_TYPE_SEQUENCE, 16U);
    const float *histogram =
        capped.feature(QUIC_FIRST_BYTE_HISTOGRAM, 16U);
    ASSERT_NE(nullptr, candidate);
    ASSERT_NE(nullptr, success);
    ASSERT_NE(nullptr, short_header);
    ASSERT_NE(nullptr, sequence_count);
    ASSERT_NE(nullptr, types);
    ASSERT_NE(nullptr, histogram);
    EXPECT_FLOAT_EQ(64.0F, candidate[0U]);
    EXPECT_FLOAT_EQ(64.0F, success[0U]);
    EXPECT_FLOAT_EQ(64.0F, short_header[0U]);
    EXPECT_FLOAT_EQ(16.0F, sequence_count[0U]);
    EXPECT_FLOAT_EQ(64.0F, histogram[4U]);
    for (size_t index = 0U; index < 16U; ++index) {
        EXPECT_FLOAT_EQ(6.0F, types[index]);
    }

    const auto initial = make_quic_long_packet(
        ktag_proxy_features::KTAG_QUIC_VERSION_1,
        ktag_proxy_features::quic_header_type::initial);
    quic_harness ipv4;
    quic_harness ipv6;
    ASSERT_TRUE(ipv4.valid());
    ASSERT_TRUE(ipv6.valid());
    ASSERT_TRUE(ipv4.process(
        make_udp_frame_with_payload(
            false, false, initial),
        1U));
    ASSERT_TRUE(ipv6.process(
        make_udp_frame_with_payload(
            true, false, initial, true),
        1U));
    ASSERT_TRUE(ipv4.finish());
    ASSERT_TRUE(ipv6.finish());
    const auto ipv4_values = ipv4.values();
    const auto ipv6_values = ipv6.values();
    ASSERT_EQ(68U, ipv4_values.size());
    ASSERT_EQ(ipv4_values.size(), ipv6_values.size());
    for (size_t index = 0U; index < ipv4_values.size(); ++index) {
        EXPECT_FLOAT_EQ(ipv4_values[index], ipv6_values[index])
            << index;
    }
}

TEST(KTAGProxyQuic, HandlesIpv6FragmentBoundaries)
{
    const std::vector<uint8_t> partial{
        0xc0U, 0U, 0U, 0U, 1U, 8U,
    };
    quic_harness first_fragment;
    ASSERT_TRUE(first_fragment.valid());
    auto first_fragment_frame =
        make_ipv6_udp_fragment_with_payload(partial, 1U);
    constexpr size_t IPV6_FRAGMENT_UDP_OFFSET = 14U + 40U + 8U;
    ASSERT_GE(
        first_fragment_frame.size(), IPV6_FRAGMENT_UDP_OFFSET + 8U);
    write_be16(
        &first_fragment_frame,
        IPV6_FRAGMENT_UDP_OFFSET + 4U,
        1208U);
    ASSERT_TRUE(first_fragment.process(
        first_fragment_frame,
        1U));
    ASSERT_TRUE(first_fragment.finish());
    const float *candidate =
        first_fragment.feature(QUIC_PACKET_CANDIDATE_COUNT);
    const float *failure =
        first_fragment.feature(QUIC_PARSE_FAILURE_COUNT);
    ASSERT_NE(nullptr, candidate);
    ASSERT_NE(nullptr, failure);
    EXPECT_FLOAT_EQ(1.0F, candidate[0U]);
    EXPECT_FLOAT_EQ(1.0F, failure[0U]);

    quic_harness non_initial;
    ASSERT_TRUE(non_initial.valid());
    ASSERT_TRUE(non_initial.process(
        make_ipv6_udp_fragment_with_payload(partial, 8U),
        1U));
    EXPECT_TRUE(non_initial.finish_empty());
}

} // namespace
