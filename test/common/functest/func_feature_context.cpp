/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <netinet/tcp.h>

#include "kdadk_feature_extract.h"
#include "ktag_feature.h"
#include "ktag_traffic_features.h"

namespace {

struct callback_counts {
    std::atomic<uint32_t> provider_create{0U};
    std::atomic<uint32_t> provider_destroy{0U};
    std::atomic<uint32_t> flow_create{0U};
    std::atomic<uint32_t> flow_destroy{0U};
    std::atomic<uint32_t> packet{0U};
    std::atomic<uint32_t> packet_domain_present{0U};
    std::atomic<uint32_t> finish{0U};
    std::atomic<uint32_t> forward_domain_present{0U};
    std::atomic<uint32_t> reverse_domain_present{0U};
    std::atomic<uint32_t> flow_key_ipv6{0U};
    std::atomic<uint32_t> packet_view_errors{0U};
    std::atomic<uint32_t> packet_direction_mask{0U};
    std::atomic<uint32_t> packet_network_version{0U};
    std::atomic<uint32_t> packet_transport_protocol{0U};
    std::atomic<uint32_t> packet_payload_bytes{0U};
    std::atomic<uint32_t> ipv6_extension_headers{0U};
    std::atomic<uint32_t> fragment_state{0U};
    std::atomic<uint32_t> tcp_flags{0U};
    std::atomic<uint32_t> tcp_sequence{0U};
    std::atomic<uint32_t> tcp_acknowledgment{0U};
};

void write_be16(std::vector<uint8_t> *data, size_t offset, uint16_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 8U);
    (*data)[offset + 1U] = static_cast<uint8_t>(value & 0xffU);
}

void write_be32(std::vector<uint8_t> *data, size_t offset, uint32_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 24U);
    (*data)[offset + 1U] =
        static_cast<uint8_t>((value >> 16U) & 0xffU);
    (*data)[offset + 2U] =
        static_cast<uint8_t>((value >> 8U) & 0xffU);
    (*data)[offset + 3U] =
        static_cast<uint8_t>(value & 0xffU);
}

std::vector<uint8_t> udp_frame_with_payload(
    const std::array<uint8_t, 4> &source,
    uint16_t source_port,
    const std::array<uint8_t, 4> &destination,
    uint16_t destination_port,
    const std::vector<uint8_t> &payload)
{
    constexpr size_t network_offset = 14U;
    constexpr size_t ip_length = 20U;
    constexpr size_t udp_header_length = 8U;
    const size_t udp_length = udp_header_length + payload.size();
    std::vector<uint8_t> frame(
        network_offset + ip_length + udp_length, 0U);
    write_be16(&frame, 12U, 0x0800U);
    frame[network_offset] = 0x45U;
    write_be16(
        &frame,
        network_offset + 2U,
        static_cast<uint16_t>(ip_length + udp_length));
    frame[network_offset + 8U] = 64U;
    frame[network_offset + 9U] = 17U;
    std::memcpy(
        frame.data() + network_offset + 12U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + network_offset + 16U,
        destination.data(),
        destination.size());
    const size_t transport = network_offset + ip_length;
    write_be16(&frame, transport, source_port);
    write_be16(&frame, transport + 2U, destination_port);
    write_be16(
        &frame,
        transport + 4U,
        static_cast<uint16_t>(udp_length));
    if (!payload.empty()) {
        std::memcpy(
            frame.data() + transport + udp_header_length,
            payload.data(),
            payload.size());
    }
    return frame;
}

std::vector<uint8_t> udp_frame(
    const std::array<uint8_t, 4> &source,
    uint16_t source_port,
    const std::array<uint8_t, 4> &destination,
    uint16_t destination_port)
{
    return udp_frame_with_payload(
        source,
        source_port,
        destination,
        destination_port,
        {});
}

std::vector<uint8_t> ipv6_udp_frame(
    const std::array<uint8_t, 16> &source,
    uint16_t source_port,
    const std::array<uint8_t, 16> &destination,
    uint16_t destination_port,
    const std::vector<uint8_t> &payload,
    uint32_t extension_count = 0U)
{
    constexpr size_t network_offset = 14U;
    constexpr size_t udp_header_length = 8U;
    const size_t extension_bytes =
        static_cast<size_t>(extension_count) * 8U;
    const size_t udp_length = udp_header_length + payload.size();
    const size_t ipv6_payload_length = extension_bytes + udp_length;
    std::vector<uint8_t> frame(
        network_offset + 40U + ipv6_payload_length, 0U);
    write_be16(&frame, 12U, 0x86ddU);
    frame[network_offset] = 0x60U;
    write_be16(
        &frame,
        network_offset + 4U,
        static_cast<uint16_t>(ipv6_payload_length));
    frame[network_offset + 6U] =
        extension_count == 0U ? 17U : 60U;
    frame[network_offset + 7U] = 64U;
    std::memcpy(
        frame.data() + network_offset + 8U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + network_offset + 24U,
        destination.data(),
        destination.size());
    size_t transport = network_offset + 40U;
    for (uint32_t index = 0U; index < extension_count; ++index) {
        frame[transport] =
            index + 1U == extension_count ? 17U : 60U;
        frame[transport + 1U] = 0U;
        transport += 8U;
    }
    write_be16(&frame, transport, source_port);
    write_be16(&frame, transport + 2U, destination_port);
    write_be16(
        &frame,
        transport + 4U,
        static_cast<uint16_t>(udp_length));
    if (!payload.empty()) {
        std::memcpy(
            frame.data() + transport + udp_header_length,
            payload.data(),
            payload.size());
    }
    return frame;
}

std::vector<uint8_t> ipv6_tcp_frame(
    const std::array<uint8_t, 16> &source,
    uint16_t source_port,
    const std::array<uint8_t, 16> &destination,
    uint16_t destination_port,
    uint8_t flags,
    uint32_t sequence,
    uint32_t acknowledgment,
    const std::vector<uint8_t> &payload)
{
    constexpr size_t network_offset = 14U;
    constexpr size_t tcp_header_length = 20U;
    const size_t transport_length = tcp_header_length + payload.size();
    std::vector<uint8_t> frame(
        network_offset + 40U + transport_length, 0U);
    write_be16(&frame, 12U, 0x86ddU);
    frame[network_offset] = 0x60U;
    write_be16(
        &frame,
        network_offset + 4U,
        static_cast<uint16_t>(transport_length));
    frame[network_offset + 6U] = 6U;
    frame[network_offset + 7U] = 64U;
    std::memcpy(
        frame.data() + network_offset + 8U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + network_offset + 24U,
        destination.data(),
        destination.size());
    const size_t transport = network_offset + 40U;
    write_be16(&frame, transport, source_port);
    write_be16(&frame, transport + 2U, destination_port);
    write_be32(&frame, transport + 4U, sequence);
    write_be32(&frame, transport + 8U, acknowledgment);
    frame[transport + 12U] = 0x50U;
    frame[transport + 13U] = flags;
    if (!payload.empty()) {
        std::memcpy(
            frame.data() + transport + tcp_header_length,
            payload.data(),
            payload.size());
    }
    return frame;
}

std::vector<uint8_t> dns_aaaa_response(
    const std::array<uint8_t, 16> &address)
{
    std::vector<uint8_t> dns(56U, 0U);
    write_be16(&dns, 0U, 0x1234U);
    write_be16(&dns, 2U, 0x8180U);
    write_be16(&dns, 4U, 1U);
    write_be16(&dns, 6U, 1U);
    dns[12U] = 2U;
    dns[13U] = 'v';
    dns[14U] = '6';
    dns[15U] = 7U;
    std::memcpy(dns.data() + 16U, "example", 7U);
    dns[23U] = 0U;
    write_be16(&dns, 24U, 28U);
    write_be16(&dns, 26U, 1U);
    dns[28U] = 0xc0U;
    dns[29U] = 0x0cU;
    write_be16(&dns, 30U, 28U);
    write_be16(&dns, 32U, 1U);
    write_be32(&dns, 34U, 60U);
    write_be16(&dns, 38U, 16U);
    std::memcpy(dns.data() + 40U, address.data(), address.size());
    return dns;
}

std::vector<uint8_t> tcp_frame(
    const std::array<uint8_t, 4> &source,
    uint16_t source_port,
    const std::array<uint8_t, 4> &destination,
    uint16_t destination_port,
    uint8_t flags)
{
    constexpr size_t network_offset = 14U;
    constexpr size_t ip_length = 20U;
    constexpr size_t tcp_length = 20U;
    std::vector<uint8_t> frame(network_offset + ip_length + tcp_length, 0U);
    write_be16(&frame, 12U, 0x0800U);
    frame[network_offset] = 0x45U;
    write_be16(&frame, network_offset + 2U, ip_length + tcp_length);
    frame[network_offset + 8U] = 64U;
    frame[network_offset + 9U] = 6U;
    std::memcpy(frame.data() + network_offset + 12U, source.data(), source.size());
    std::memcpy(frame.data() + network_offset + 16U, destination.data(), destination.size());
    const size_t transport = network_offset + ip_length;
    write_be16(&frame, transport, source_port);
    write_be16(&frame, transport + 2U, destination_port);
    frame[transport + 12U] = 0x50U;
    frame[transport + 13U] = flags;
    return frame;
}

std::vector<uint8_t> tcp_frame_with_payload(
    const std::array<uint8_t, 4> &source,
    uint16_t source_port,
    const std::array<uint8_t, 4> &destination,
    uint16_t destination_port,
    uint8_t flags,
    const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> frame = tcp_frame(
        source, source_port, destination, destination_port, flags);
    constexpr size_t network_offset = 14U;
    constexpr size_t ip_length = 20U;
    constexpr size_t tcp_length = 20U;
    write_be16(
        &frame,
        network_offset + 2U,
        static_cast<uint16_t>(ip_length + tcp_length + payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

ktag_packet packet_view(
    const std::vector<uint8_t> &frame,
    uint64_t sequence,
    int64_t seconds,
    int32_t microseconds = 0)
{
    ktag_packet packet{};
    packet.struct_size = sizeof(packet);
    packet.api_version = KTAG_API_VERSION;
    packet.data = frame.data();
    packet.captured_length = static_cast<uint32_t>(frame.size());
    packet.original_length = packet.captured_length;
    packet.link_type = KTAG_CAPTURE_LINKTYPE_ETHERNET;
    packet.sequence = sequence;
    packet.timestamp.seconds = seconds;
    packet.timestamp.microseconds = microseconds;
    return packet;
}

ktag_status provider_create(
    const ktag_feature_provider_config *config,
    void **provider_state)
{
    auto *counts = static_cast<callback_counts *>(config->user_data);
    counts->provider_create.fetch_add(1U, std::memory_order_relaxed);
    *provider_state = counts;
    return KTAG_SUCCESS;
}

ktag_status flow_create(
    void *provider_state,
    const ktag_flow_view *flow,
    void **flow_state)
{
    auto *counts = static_cast<callback_counts *>(provider_state);
    counts->flow_create.fetch_add(1U, std::memory_order_relaxed);
    ktag_flow_key key{};
    if (ktag_flow_view_get_flow_key(flow, &key) != KTAG_SUCCESS) {
        counts->packet_view_errors.fetch_add(
            1U, std::memory_order_relaxed);
    } else {
        ktag_ip_address forward_address{};
        ktag_ip_address reverse_address{};
        uint16_t forward_port = 0U;
        uint16_t reverse_port = 0U;
        if (ktag_flow_view_get_endpoint(
                flow, KTAG_DIRECTION_FORWARD, &forward_address,
                &forward_port) != KTAG_SUCCESS ||
            ktag_flow_view_get_endpoint(
                flow, KTAG_DIRECTION_REVERSE, &reverse_address,
                &reverse_port) != KTAG_SUCCESS ||
            forward_address.version != key.address_a.version ||
            reverse_address.version != key.address_b.version ||
            forward_port == 0U || reverse_port == 0U) {
            counts->packet_view_errors.fetch_add(
                1U, std::memory_order_relaxed);
        }
        if (key.address_a.version == 6U) {
            counts->flow_key_ipv6.fetch_add(
                1U, std::memory_order_relaxed);
        }
    }
    *flow_state = counts;
    return KTAG_SUCCESS;
}

ktag_status on_packet(
    void *provider_state,
    void *,
    const ktag_packet_view *packet,
    const ktag_flow_view *flow)
{
    auto *counts = static_cast<callback_counts *>(provider_state);
    counts->packet.fetch_add(1U, std::memory_order_relaxed);
    ktag_packet_transport_view transport{};
    transport.struct_size = sizeof(transport);
    transport.api_version = KTAG_API_VERSION;
    uint32_t payload_length = 0U;
    const uint8_t *payload =
        ktag_packet_view_get_payload(packet, &payload_length);
    if (ktag_packet_view_get_transport(packet, &transport) !=
            KTAG_SUCCESS ||
        (payload == nullptr && payload_length != 0U)) {
        counts->packet_view_errors.fetch_add(
            1U, std::memory_order_relaxed);
    } else {
        counts->packet_direction_mask.fetch_or(
            1U << static_cast<uint32_t>(transport.direction),
            std::memory_order_relaxed);
        counts->packet_network_version.store(
            transport.network_version, std::memory_order_relaxed);
        counts->packet_transport_protocol.store(
            transport.transport_protocol, std::memory_order_relaxed);
        counts->packet_payload_bytes.fetch_add(
            payload_length, std::memory_order_relaxed);
        counts->ipv6_extension_headers.fetch_add(
            transport.ipv6_extension_header_count,
            std::memory_order_relaxed);
        counts->fragment_state.store(
            transport.fragment_state, std::memory_order_relaxed);
        if (transport.transport_protocol == 6U) {
            counts->tcp_flags.store(
                transport.tcp_flags, std::memory_order_relaxed);
            counts->tcp_sequence.store(
                transport.tcp_sequence, std::memory_order_relaxed);
            counts->tcp_acknowledgment.store(
                transport.tcp_acknowledgment,
                std::memory_order_relaxed);
        }
    }
    const char *forward_domain =
        ktag_flow_view_get_domain(flow, KTAG_DIRECTION_FORWARD);
    const char *reverse_domain =
        ktag_flow_view_get_domain(flow, KTAG_DIRECTION_REVERSE);
    if ((forward_domain != nullptr && forward_domain[0] != '\0') ||
        (reverse_domain != nullptr && reverse_domain[0] != '\0')) {
        counts->packet_domain_present.fetch_add(1U, std::memory_order_relaxed);
    }
    return KTAG_SUCCESS;
}

ktag_status flow_finish(
    void *provider_state,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    auto *counts = static_cast<callback_counts *>(provider_state);
    counts->finish.fetch_add(1U, std::memory_order_relaxed);
    const float packet_count = static_cast<float>(
        ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_FORWARD) +
        ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_REVERSE));
    const char *forward_domain =
        ktag_flow_view_get_domain(flow, KTAG_DIRECTION_FORWARD);
    const char *reverse_domain =
        ktag_flow_view_get_domain(flow, KTAG_DIRECTION_REVERSE);
    const bool has_forward_domain =
        forward_domain != nullptr && forward_domain[0] != '\0';
    const bool has_reverse_domain =
        reverse_domain != nullptr && reverse_domain[0] != '\0';
    counts->forward_domain_present.store(
        has_forward_domain ? 1U : 0U, std::memory_order_relaxed);
    counts->reverse_domain_present.store(
        has_reverse_domain ? 1U : 0U, std::memory_order_relaxed);
    const float domain_present =
        has_forward_domain || has_reverse_domain ? 1.0F : 0.0F;
    if (ktag_feature_output_set(
            output, "context.packet_count", &packet_count, 1U) !=
        KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    return ktag_feature_output_set(
        output, "context.domain_present", &domain_present, 1U);
}

void flow_destroy(void *provider_state, void *)
{
    auto *counts = static_cast<callback_counts *>(provider_state);
    counts->flow_destroy.fetch_add(1U, std::memory_order_relaxed);
}

void provider_destroy(void *provider_state)
{
    auto *counts = static_cast<callback_counts *>(provider_state);
    counts->provider_destroy.fetch_add(1U, std::memory_order_relaxed);
}

bool make_contract(
    callback_counts *counts,
    ktag_feature_registry **registry,
    ktag_feature_schema **schema)
{
    *registry = ktag_feature_registry_create();
    *schema = ktag_feature_schema_create("feature_context_test", 1U);
    if (*registry == nullptr || *schema == nullptr) {
        return false;
    }

    ktag_feature_descriptor descriptors[2] = {};
    descriptors[0].struct_size = sizeof(descriptors[0]);
    descriptors[0].api_version = KTAG_API_VERSION;
    descriptors[0].feature_id = "context.packet_count";
    descriptors[0].display_name = "packet_count";
    descriptors[0].provider_id = "test.feature_context";
    descriptors[0].provider_version = 1U;
    descriptors[0].value_type = KTAG_FEATURE_FLOAT32;
    descriptors[0].dimension = 1U;
    descriptors[1] = descriptors[0];
    descriptors[1].feature_id = "context.domain_present";
    descriptors[1].display_name = "domain_present";

    ktag_feature_provider_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.api_version = KTAG_API_VERSION;
    registration.provider_id = "test.feature_context";
    registration.provider_version = 1U;
    registration.features = descriptors;
    registration.feature_count = 2U;
    registration.callbacks.struct_size = sizeof(registration.callbacks);
    registration.callbacks.api_version = KTAG_API_VERSION;
    registration.callbacks.provider_create = provider_create;
    registration.callbacks.flow_create = flow_create;
    registration.callbacks.on_packet = on_packet;
    registration.callbacks.flow_finish = flow_finish;
    registration.callbacks.flow_destroy = flow_destroy;
    registration.callbacks.provider_destroy = provider_destroy;
    registration.user_data = counts;

    return ktag_feature_registry_register(
               *registry, &registration, KTAG_FEATURE_REGISTER_DEFAULT) ==
            KTAG_SUCCESS &&
        ktag_feature_schema_add_registered(
            *schema, *registry, "context.packet_count") == KTAG_SUCCESS &&
        ktag_feature_schema_add_registered(
            *schema, *registry, "context.domain_present") == KTAG_SUCCESS &&
        ktag_feature_schema_freeze(*schema) == KTAG_SUCCESS;
}

ktag_feature_context *make_context(
    ktag_feature_schema *schema,
    ktag_feature_registry *registry,
    uint32_t capacity,
    uint32_t udp_packet_limit,
    uint64_t tcp_timeout,
    uint64_t udp_timeout,
    ktag_domain_store *store = nullptr,
    ktag_feature_lifecycle_mode lifecycle_mode =
        KTAG_FEATURE_LIFECYCLE_MODERN)
{
    ktag_feature_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.schema = schema;
    config.registry = registry;
    config.shared_domain_store = store;
    config.flow_capacity = capacity;
    config.tcp_idle_timeout_us = tcp_timeout;
    config.udp_idle_timeout_us = udp_timeout;
    config.udp_packet_limit = udp_packet_limit;
    config.domain_authority_lookup_on_miss = 0U;
    config.lifecycle_mode = lifecycle_mode;
    return ktag_feature_create(&config);
}

ktag_feature_statistics get_statistics(ktag_feature_context *context)
{
    ktag_feature_statistics statistics{};
    statistics.struct_size = sizeof(statistics);
    statistics.api_version = KTAG_API_VERSION;
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_get_statistics(context, &statistics));
    return statistics;
}

} // namespace

TEST(KTAGFeatureContext, UdpLimitRunsCallbacksAndEmitsDynamicRecord)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 2U, 1000U, 1000U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 2U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    const std::array<uint8_t, 4> first = {10U, 0U, 0U, 1U};
    const std::array<uint8_t, 4> second = {10U, 0U, 0U, 2U};
    const auto forward_frame = udp_frame(first, 50000U, second, 443U);
    const auto reverse_frame = udp_frame(second, 443U, first, 50000U);
    const auto forward = packet_view(forward_frame, 10U, 100U);
    const auto reverse = packet_view(reverse_frame, 11U, 101U);

    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_process_packet(context, &forward, batch));
    EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_process_packet(context, &reverse, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    EXPECT_EQ(1U, record.metadata.flow_id);
    EXPECT_EQ(10U, record.metadata.first_packet_sequence);
    EXPECT_EQ(11U, record.metadata.last_packet_sequence);
    EXPECT_EQ(17U, record.metadata.flow_key.protocol);
    ASSERT_EQ(2U, record.value_count);
    EXPECT_FLOAT_EQ(2.0F, record.values[0]);
    EXPECT_FLOAT_EQ(0.0F, record.values[1]);

    const auto statistics = get_statistics(context);
    EXPECT_EQ(2U, statistics.received_packets);
    EXPECT_EQ(2U, statistics.decoded_packets);
    EXPECT_EQ(1U, statistics.created_flows);
    EXPECT_EQ(1U, statistics.completed_flows);
    EXPECT_EQ(1U, statistics.emitted_flows);
    EXPECT_EQ(0U, statistics.active_flows);
    EXPECT_EQ(1U, statistics.peak_active_flows);
    EXPECT_EQ(1U, counts.flow_create.load(std::memory_order_relaxed));
    EXPECT_EQ(2U, counts.packet.load(std::memory_order_relaxed));
    EXPECT_EQ(1U, counts.finish.load(std::memory_order_relaxed));
    EXPECT_EQ(1U, counts.flow_destroy.load(std::memory_order_relaxed));

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    EXPECT_EQ(1U, counts.provider_destroy.load(std::memory_order_relaxed));
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, IPv6UdpUsesFullDirectionAndPacketView)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 2U, 1000U, 1000U);
    ktag_feature_batch *batch =
        ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    std::array<uint8_t, 16> first{};
    std::array<uint8_t, 16> second{};
    first[0] = 0x20U;
    first[1] = 0x01U;
    first[15] = 1U;
    second[0] = 0x20U;
    second[1] = 0x01U;
    second[15] = 2U;
    const auto forward_frame = ipv6_udp_frame(
        second,
        50000U,
        first,
        443U,
        {1U, 2U, 3U},
        2U);
    const auto reverse_frame = ipv6_udp_frame(
        first,
        443U,
        second,
        50000U,
        {4U, 5U});
    const auto forward =
        packet_view(forward_frame, 10U, 100U);
    const auto reverse =
        packet_view(reverse_frame, 11U, 101U);

    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &forward, batch));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &reverse, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_batch_get_record(batch, 0U, &record));
    ktag_feature_record_details details{};
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_batch_get_record_details(batch, 0U, &details));
    EXPECT_EQ(6U, record.metadata.flow_key.address_a.version);
    EXPECT_EQ(17U, record.metadata.flow_key.protocol);
    EXPECT_EQ(6U, details.source_address.version);
    EXPECT_EQ(2U, details.source_address.bytes[15U]);
    EXPECT_EQ(1U, details.destination_address.bytes[15U]);
    EXPECT_EQ(50000U, details.source_port);
    EXPECT_EQ(443U, details.destination_port);
    EXPECT_EQ(1U, details.ipv6_extension_packet_count);
    EXPECT_EQ(0U, details.ip_fragment_first_packet_count);
    EXPECT_FLOAT_EQ(2.0F, record.values[0]);
    EXPECT_EQ(
        1U, counts.flow_key_ipv6.load(std::memory_order_relaxed));
    EXPECT_EQ(
        0U,
        counts.packet_view_errors.load(std::memory_order_relaxed));
    EXPECT_EQ(
        3U,
        counts.packet_direction_mask.load(
            std::memory_order_relaxed));
    EXPECT_EQ(
        6U,
        counts.packet_network_version.load(
            std::memory_order_relaxed));
    EXPECT_EQ(
        17U,
        counts.packet_transport_protocol.load(
            std::memory_order_relaxed));
    EXPECT_EQ(
        5U,
        counts.packet_payload_bytes.load(
            std::memory_order_relaxed));
    EXPECT_EQ(
        2U,
        counts.ipv6_extension_headers.load(
            std::memory_order_relaxed));

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, IPv6TcpExposesFlagsSequenceAckAndPayload)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch =
        ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    std::array<uint8_t, 16> source{};
    std::array<uint8_t, 16> destination{};
    source[0] = 0x20U;
    source[1] = 0x01U;
    source[15] = 10U;
    destination[0] = 0x20U;
    destination[1] = 0x01U;
    destination[15] = 20U;
    constexpr uint32_t sequence = 0x10203040U;
    constexpr uint32_t acknowledgment = 0x50607080U;
    const auto frame = ipv6_tcp_frame(
        source,
        51000U,
        destination,
        443U,
        TH_ACK,
        sequence,
        acknowledgment,
        {0xdeU, 0xadU});
    const auto packet = packet_view(frame, 1U, 1U);

    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &packet, batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));
    EXPECT_EQ(
        0U,
        counts.packet_view_errors.load(std::memory_order_relaxed));
    EXPECT_EQ(
        6U,
        counts.packet_transport_protocol.load(
            std::memory_order_relaxed));
    EXPECT_EQ(
        2U,
        counts.packet_payload_bytes.load(
            std::memory_order_relaxed));
    EXPECT_EQ(
        static_cast<uint32_t>(TH_ACK),
        counts.tcp_flags.load(std::memory_order_relaxed));
    EXPECT_EQ(
        sequence,
        counts.tcp_sequence.load(std::memory_order_relaxed));
    EXPECT_EQ(
        acknowledgment,
        counts.tcp_acknowledgment.load(
            std::memory_order_relaxed));

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, NonInitialIPv6FragmentIsSkippedPerPacket)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch =
        ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    std::array<uint8_t, 16> source{};
    std::array<uint8_t, 16> destination{};
    source[0] = 0x20U;
    source[1] = 0x01U;
    source[15] = 1U;
    destination[0] = 0x20U;
    destination[1] = 0x01U;
    destination[15] = 2U;
    auto fragment = ipv6_udp_frame(
        source, 52000U, destination, 443U, {}, 1U);
    fragment[14U + 6U] = 44U;
    write_be16(&fragment, 14U + 40U + 2U, 8U);
    const auto skipped = packet_view(fragment, 1U, 1U);
    auto valid_frame = ipv6_udp_frame(
        source, 52000U, destination, 443U, {1U}, 1U);
    valid_frame[14U + 6U] = 44U;
    const auto valid = packet_view(valid_frame, 2U, 2U);

    EXPECT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &skipped, batch));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &valid, batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    EXPECT_EQ(1U, ktag_feature_batch_get_count(batch));
    const auto statistics = get_statistics(context);
    EXPECT_EQ(2U, statistics.received_packets);
    EXPECT_EQ(1U, statistics.decoded_packets);
    EXPECT_EQ(1U, statistics.rejected_packets);
    EXPECT_EQ(
        1U, counts.packet.load(std::memory_order_relaxed));
    EXPECT_EQ(
        static_cast<uint32_t>(KTAG_IP_FRAGMENT_ATOMIC),
        counts.fragment_state.load(std::memory_order_relaxed));

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, DnsAaaaAssociatesDomainAndRawBowWithIPv6Flow)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_domain_store_config store_config{};
    store_config.struct_size = sizeof(store_config);
    store_config.api_version = KTAG_API_VERSION;
    store_config.max_entries = 16U;
    store_config.journal_capacity = 8U;
    ktag_domain_store *store =
        ktag_domain_store_create(&store_config);
    ASSERT_NE(nullptr, store);
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U, store);
    ktag_feature_batch *batch =
        ktag_feature_batch_create(schema, 2U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    std::array<uint8_t, 16> client_v6{};
    std::array<uint8_t, 16> server_v6{};
    std::array<uint8_t, 16> resolver_v6{};
    client_v6[0] = 0x20U;
    client_v6[1] = 0x01U;
    client_v6[15] = 1U;
    server_v6[0] = 0x20U;
    server_v6[1] = 0x01U;
    server_v6[15] = 2U;
    resolver_v6[0] = 0x20U;
    resolver_v6[1] = 0x01U;
    resolver_v6[15] = 53U;
    const auto dns_payload = dns_aaaa_response(server_v6);
    const auto dns_frame = ipv6_udp_frame(
        resolver_v6,
        53U,
        client_v6,
        53000U,
        dns_payload);
    const auto dns_packet = packet_view(dns_frame, 1U, 1U);
    const auto ipv6_frame = ipv6_udp_frame(
        client_v6,
        54000U,
        server_v6,
        443U,
        {0x01U});
    const auto ipv6_packet = packet_view(ipv6_frame, 2U, 2U);

    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &dns_packet, batch));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &ipv6_packet, batch));
    EXPECT_EQ(
        1U,
        counts.packet_domain_present.load(
            std::memory_order_relaxed));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(2U, ktag_feature_batch_get_count(batch));

    bool found_ipv6 = false;
    for (uint32_t index = 0U; index < 2U; ++index) {
        ktag_feature_record record{};
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_batch_get_record(batch, index, &record));
        ASSERT_EQ(2U, record.value_count);
        if (record.metadata.flow_key.address_a.version == 6U &&
            record.values[1] == 1.0F) {
            found_ipv6 = true;
        }
    }
    EXPECT_TRUE(found_ipv6);
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_domain_store_destroy(store);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, LegacyLifecycleCapsUdpAndEmitsOnlyAtFlush)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_feature_context *context = make_context(
        schema,
        registry,
        8U,
        2U,
        10U,
        10U,
        nullptr,
        KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    const auto frame = udp_frame(
        {10U, 0U, 0U, 1U}, 50000U,
        {10U, 0U, 0U, 2U}, 443U);
    for (uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
        const auto packet = packet_view(
            frame, sequence, static_cast<int64_t>(sequence * 100U));
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_process_packet(context, &packet, batch));
        EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_expire(context, sequence * 100000000U, batch));
        EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
    }

    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));
    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    ASSERT_EQ(2U, record.value_count);
    EXPECT_FLOAT_EQ(2.0F, record.values[0]);
    EXPECT_EQ(2U, record.metadata.last_packet_sequence);
    EXPECT_EQ(2U, counts.packet.load(std::memory_order_relaxed));

    const auto statistics = get_statistics(context);
    EXPECT_EQ(1U, statistics.created_flows);
    EXPECT_EQ(1U, statistics.completed_flows);
    EXPECT_EQ(1U, statistics.emitted_flows);
    EXPECT_EQ(1U, statistics.flushed_flows);
    EXPECT_EQ(0U, statistics.expired_udp_flows);
    EXPECT_EQ(0U, statistics.active_flows);

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, RejectsWrongBatchAndFinishesTcpReset)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    ktag_feature_schema *wrong_schema =
        ktag_feature_schema_create("wrong_feature_context_test", 1U);
    ASSERT_NE(nullptr, wrong_schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_schema_add_registered(
            wrong_schema, registry, "context.packet_count"));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(wrong_schema));
    ktag_feature_batch *wrong_batch =
        ktag_feature_batch_create(wrong_schema, 1U);
    ASSERT_NE(nullptr, wrong_batch);

    const std::array<uint8_t, 4> first = {172U, 16U, 0U, 1U};
    const std::array<uint8_t, 4> second = {172U, 16U, 0U, 2U};
    const auto first_frame = tcp_frame(
        first, 45000U, second, 443U, TH_ACK);
    const auto reset_frame = tcp_frame(
        second, 443U, first, 45000U, TH_RST);
    const auto first_packet = packet_view(first_frame, 20U, 10U);
    const auto reset_packet = packet_view(reset_frame, 21U, 11U);

    EXPECT_EQ(
        KTAG_ERROR_SCHEMA_MISMATCH,
        ktag_feature_process_packet(context, &first_packet, wrong_batch));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &first_packet, batch));
    EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &reset_packet, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    EXPECT_EQ(20U, record.metadata.first_packet_sequence);
    EXPECT_EQ(21U, record.metadata.last_packet_sequence);
    ASSERT_EQ(2U, record.value_count);
    EXPECT_FLOAT_EQ(1.0F, record.values[0]);

    const auto statistics = get_statistics(context);
    EXPECT_EQ(2U, statistics.received_packets);
    EXPECT_EQ(1U, statistics.created_flows);
    EXPECT_EQ(1U, statistics.completed_flows);
    EXPECT_EQ(1U, statistics.emitted_flows);
    EXPECT_EQ(0U, statistics.active_flows);
    EXPECT_EQ(2U, counts.packet.load(std::memory_order_relaxed));

    ktag_feature_batch_destroy(wrong_batch);
    ktag_feature_schema_destroy(wrong_schema);
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, CapacityUdpAndTcpExpireFlushAndThreadOwnership)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_feature_context *context = make_context(
        schema, registry, 2U, 100U, 20U, 10U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 4U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    const std::array<uint8_t, 4> first = {192U, 0U, 2U, 1U};
    const std::array<uint8_t, 4> second = {192U, 0U, 2U, 2U};
    const std::array<uint8_t, 4> third = {192U, 0U, 2U, 3U};
    const auto first_frame = udp_frame(first, 10001U, second, 20001U);
    const auto second_frame = udp_frame(first, 10002U, second, 20002U);
    const auto third_frame = udp_frame(first, 10003U, third, 20003U);
    auto packet = packet_view(first_frame, 1U, 0U, 100U);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_process_packet(context, &packet, batch));
    packet = packet_view(second_frame, 2U, 0U, 101U);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_process_packet(context, &packet, batch));
    packet = packet_view(third_frame, 3U, 0U, 102U);
    EXPECT_EQ(KTAG_ERROR_CAPACITY, ktag_feature_process_packet(context, &packet, batch));

    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_expire(context, 109U, batch));
    EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_expire(context, 110U, batch));
    EXPECT_EQ(1U, ktag_feature_batch_get_count(batch));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    EXPECT_EQ(1U, ktag_feature_batch_get_count(batch));

    const auto tcp = tcp_frame(first, 30000U, second, 443U, TH_ACK);
    packet = packet_view(tcp, 4U, 0U, 200U);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_process_packet(context, &packet, batch));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_expire(context, 219U, batch));
    EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_expire(context, 220U, batch));
    EXPECT_EQ(1U, ktag_feature_batch_get_count(batch));

    std::atomic<ktag_status> foreign_status{KTAG_SUCCESS};
    std::thread foreign([context, &foreign_status]() {
        ktag_feature_statistics statistics{};
        statistics.struct_size = sizeof(statistics);
        statistics.api_version = KTAG_API_VERSION;
        foreign_status.store(
            ktag_feature_get_statistics(context, &statistics),
            std::memory_order_relaxed);
    });
    foreign.join();
    EXPECT_EQ(
        KTAG_ERROR_INVALID_STATE,
        foreign_status.load(std::memory_order_relaxed));

    const auto statistics = get_statistics(context);
    EXPECT_EQ(3U, statistics.created_flows);
    EXPECT_EQ(3U, statistics.completed_flows);
    EXPECT_EQ(1U, statistics.expired_udp_flows);
    EXPECT_EQ(1U, statistics.expired_tcp_flows);
    EXPECT_EQ(1U, statistics.flushed_flows);
    EXPECT_EQ(1U, statistics.capacity_rejections);
    EXPECT_EQ(1U, statistics.thread_violations);
    EXPECT_EQ(0U, statistics.active_flows);

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

/*
 * This is the migration gate: the Provider path and the frozen fixed-vector
 * extractor receive the same bidirectional flow and must agree on every one
 * of the 354 Legacy V1 values. Representative-value tests cannot replace this
 * differential check.
 */
TEST(KTAGFeatureContext, LegacySchemaMatchesCompatibilityExtractor)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_traffic_features_register(registry));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    feature_extractor *legacy = extractor_init(DLT_EN10MB);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);
    ASSERT_NE(nullptr, legacy);

    const std::array<uint8_t, 4> first{10U, 10U, 0U, 1U};
    const std::array<uint8_t, 4> second{10U, 10U, 0U, 2U};
    const std::vector<std::vector<uint8_t>> frames = {
        udp_frame_with_payload(first, 12345U, second, 443U, {1U, 2U, 3U}),
        udp_frame_with_payload(
            second, 443U, first, 12345U,
            {4U, 5U, 6U, 7U, 8U, 9U, 10U}),
        udp_frame_with_payload(
            first, 12345U, second, 443U,
            {11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U}),
        udp_frame_with_payload(second, 443U, first, 12345U, {22U})};
    const std::array<int32_t, 4U> microseconds = {8, 108, 2008, 102008};
    for (size_t index = 0U; index < frames.size(); ++index) {
        const auto packet = packet_view(
            frames[index], index + 1U, 7U, microseconds[index]);
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_process_packet(context, &packet, batch));

        feature_vector packet_output{};
        int has_feature = 0;
        timeval timestamp{};
        timestamp.tv_sec = 7;
        timestamp.tv_usec = microseconds[index];
        ASSERT_EQ(
            EXTRACTOR_SUCCESS,
            process_packet(
                legacy,
                frames[index].data(),
                static_cast<uint32_t>(frames[index].size()),
                &timestamp,
                &packet_output,
                &has_feature));
        EXPECT_EQ(0, has_feature);
    }
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));
    feature_vector legacy_output{};
    feature_vector_list legacy_list{};
    legacy_list.vectors = &legacy_output;
    legacy_list.capacity = 1U;
    ASSERT_EQ(EXTRACTOR_SUCCESS, extractor_finalize(legacy, &legacy_list));
    ASSERT_EQ(1U, legacy_list.count);

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    ASSERT_EQ(KTAG_LEGACY_V1_VALUE_COUNT, record.value_count);
    for (uint32_t index = 0U; index < record.value_count; ++index) {
        EXPECT_FLOAT_EQ(legacy_output.features[index], record.values[index])
            << "legacy value mismatch at index " << index;
    }

    extractor_destroy(legacy);
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, LegacySchemaKeepsIPv6IpFeaturesAtZero)
{
    ktag_feature_registry *registry =
        ktag_feature_registry_create();
    ktag_feature_schema *schema =
        ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_traffic_features_register(registry));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch =
        ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    std::array<uint8_t, 16> source{};
    std::array<uint8_t, 16> destination{};
    source[0] = 0x20U;
    source[1] = 0x01U;
    source[15] = 1U;
    destination[0] = 0x20U;
    destination[1] = 0x01U;
    destination[15] = 2U;
    const auto frame = ipv6_udp_frame(
        source, 55000U, destination, 443U, {1U, 2U});
    const auto packet = packet_view(frame, 1U, 1U);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &packet, batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    uint32_t src_ip_offset = 0U;
    uint32_t dst_ip_offset = 0U;
    ktag_feature_descriptor descriptor{};
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_schema_find(
            schema,
            "network.src_ip",
            &descriptor,
            &src_ip_offset));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_schema_find(
            schema,
            "network.dst_ip",
            &descriptor,
            &dst_ip_offset));
    ktag_feature_record record{};
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_batch_get_record(batch, 0U, &record));
    ASSERT_EQ(KTAG_LEGACY_V1_VALUE_COUNT, record.value_count);
    EXPECT_FLOAT_EQ(0.0F, record.values[src_ip_offset]);
    EXPECT_FLOAT_EQ(0.0F, record.values[dst_ip_offset]);
    EXPECT_EQ(6U, record.metadata.flow_key.address_a.version);

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, LegacySchemaNativeProvidersProduceStableValues)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_traffic_features_register(registry));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    const std::array<uint8_t, 4> first{10U, 10U, 0U, 1U};
    const std::array<uint8_t, 4> second{10U, 10U, 0U, 2U};
    const std::vector<std::vector<uint8_t>> frames = {
        udp_frame_with_payload(first, 12345U, second, 443U, {1U, 2U, 3U}),
        udp_frame_with_payload(
            second, 443U, first, 12345U,
            {4U, 5U, 6U, 7U, 8U, 9U, 10U}),
        udp_frame_with_payload(
            first, 12345U, second, 443U,
            {11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U}),
        udp_frame_with_payload(second, 443U, first, 12345U, {22U})};
    const std::array<int32_t, 4U> microseconds = {8, 108, 2008, 102008};
    for (size_t index = 0U; index < frames.size(); ++index) {
        const auto packet = packet_view(
            frames[index], index + 1U, 7U, microseconds[index]);
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_process_packet(context, &packet, batch));
    }
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    ASSERT_EQ(KTAG_LEGACY_V1_VALUE_COUNT, record.value_count);
    const auto value = [schema, &record](const char *feature_id) {
        ktag_feature_descriptor descriptor{};
        uint32_t offset = 0U;
        EXPECT_EQ(
            KTAG_SUCCESS,
            ktag_feature_schema_find(
                schema, feature_id, &descriptor, &offset));
        EXPECT_EQ(1U, descriptor.dimension);
        return record.values[offset];
    };
    EXPECT_FLOAT_EQ(17.0F, value("network.protocol"));
    EXPECT_FLOAT_EQ(2.0F, value("traffic.send.packet_count"));
    EXPECT_FLOAT_EQ(2.0F, value("traffic.receive.packet_count"));
    EXPECT_FLOAT_EQ(3.0F, value("traffic.send.payload.minimum"));
    EXPECT_FLOAT_EQ(11.0F, value("traffic.send.payload.maximum"));
    EXPECT_FLOAT_EQ(7.0F, value("traffic.send.payload.mean"));
    EXPECT_FLOAT_EQ(1.0F, value("traffic.receive.payload.minimum"));
    EXPECT_FLOAT_EQ(7.0F, value("traffic.receive.payload.maximum"));
    EXPECT_FLOAT_EQ(4.0F, value("traffic.receive.payload.mean"));

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, LegacyLifecycleStopsAccumulatingPastUdpLimit)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_traffic_features_register(registry));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 10U, 10U, nullptr,
        KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    feature_extractor *legacy = extractor_init(DLT_EN10MB);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);
    ASSERT_NE(nullptr, legacy);

    const auto frame = udp_frame(
        {10U, 30U, 0U, 1U}, 12345U,
        {10U, 30U, 0U, 2U}, 443U);
    for (uint64_t sequence = 1U; sequence <= 101U; ++sequence) {
        const auto packet = packet_view(
            frame, sequence, static_cast<int64_t>(sequence * 100U));
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_process_packet(context, &packet, batch));
        ASSERT_EQ(0U, ktag_feature_batch_get_count(batch));

        feature_vector packet_output{};
        int has_feature = 0;
        timeval timestamp{};
        timestamp.tv_sec = static_cast<time_t>(sequence * 100U);
        ASSERT_EQ(
            EXTRACTOR_SUCCESS,
            process_packet(
                legacy,
                frame.data(),
                static_cast<uint32_t>(frame.size()),
                &timestamp,
                &packet_output,
                &has_feature));
        ASSERT_EQ(0, has_feature);
    }
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_expire(context, 20000000000ULL, batch));
    ASSERT_EQ(0U, ktag_feature_batch_get_count(batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    feature_vector legacy_output{};
    feature_vector_list legacy_list{};
    legacy_list.vectors = &legacy_output;
    legacy_list.capacity = 1U;
    ASSERT_EQ(EXTRACTOR_SUCCESS, extractor_finalize(legacy, &legacy_list));
    ASSERT_EQ(1U, legacy_list.count);

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    EXPECT_EQ(100U, record.metadata.last_packet_sequence);
    ASSERT_EQ(KTAG_LEGACY_V1_VALUE_COUNT, record.value_count);
    for (uint32_t index = 0U; index < record.value_count; ++index) {
        EXPECT_FLOAT_EQ(legacy_output.features[index], record.values[index])
            << "legacy UDP-limit value mismatch at index " << index;
    }
    ktag_feature_descriptor descriptor{};
    uint32_t offset = 0U;
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_schema_find(
            schema, "traffic.send.packet_count", &descriptor, &offset));
    EXPECT_FLOAT_EQ(100.0F, record.values[offset]);

    extractor_destroy(legacy);
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, SelectedLegacySlotRunsWithoutFull354Schema)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_traffic_features_register(registry));

    /* Selecting one descriptor must activate only its owning feature-family
     * Provider; the other 353 legacy values are not part of this schema. */
    ktag_feature_schema *schema =
        ktag_feature_schema_create("selected_payload_slot", 1U);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_schema_add_registered(
            schema, registry, "traffic.send.payload.mean"));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    ASSERT_EQ(1U, ktag_feature_schema_get_feature_count(schema));
    ASSERT_EQ(1U, ktag_feature_schema_get_value_count(schema));

    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    const auto frame = udp_frame_with_payload(
        {10U, 40U, 0U, 1U}, 12345U,
        {10U, 40U, 0U, 2U}, 443U,
        {1U, 2U, 3U, 4U, 5U});
    const auto packet = packet_view(frame, 1U, 7U, 8U);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &packet, batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    ASSERT_EQ(1U, record.value_count);
    EXPECT_FLOAT_EQ(5.0F, record.values[0U]);

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, SelectedRawbowSlotOwnsHttpPacketState)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(KTAG_SUCCESS, ktag_traffic_features_register(registry));
    ktag_feature_schema *schema =
        ktag_feature_schema_create("selected_rawbow_slot", 1U);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_schema_add_registered(
            schema, registry, "rawbow.crc_histogram"));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    ASSERT_EQ(32U, ktag_feature_schema_get_value_count(schema));

    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);
    const std::string request =
        "GET /video/123 HTTP/1.1\r\nHost: api.bilibili.com\r\n\r\n";
    const auto frame = tcp_frame_with_payload(
        {10U, 50U, 0U, 1U}, 12345U,
        {10U, 50U, 0U, 2U}, 80U, TH_ACK,
        std::vector<uint8_t>(request.begin(), request.end()));
    const auto packet = packet_view(frame, 1U, 7U, 8U);
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_process_packet(context, &packet, batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    float histogram_sum = 0.0F;
    for (uint32_t index = 0U; index < record.value_count; ++index) {
        histogram_sum += record.values[index];
    }
    EXPECT_FLOAT_EQ(40.0F, histogram_sum);

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, FlowFinishRefreshesSharedDomainBeforeCallback)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_domain_store_config store_config{};
    store_config.struct_size = sizeof(store_config);
    store_config.api_version = KTAG_API_VERSION;
    store_config.max_entries = 16U;
    store_config.journal_capacity = 8U;
    ktag_domain_store *store = ktag_domain_store_create(&store_config);
    ASSERT_NE(nullptr, store);
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U, store);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    /* The first-packet source sorts after the destination. This verifies that
     * the public FORWARD direction is not confused with the canonical tuple. */
    const auto frame = udp_frame(
        {10U, 20U, 0U, 2U}, 50000U,
        {10U, 20U, 0U, 1U}, 443U);
    const auto packet = packet_view(frame, 1U, 1U);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &packet, batch));
    ASSERT_EQ(0U, ktag_feature_batch_get_count(batch));

    ktag_ip_address address{};
    address.version = 4U;
    address.bytes[0] = 10U;
    address.bytes[1] = 20U;
    address.bytes[2] = 0U;
    address.bytes[3] = 2U;
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_domain_store_put(
            store, &address, "late.example.", 0U, nullptr));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));
    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0U, &record));
    ASSERT_EQ(2U, record.value_count);
    EXPECT_FLOAT_EQ(1.0F, record.values[0]);
    EXPECT_FLOAT_EQ(1.0F, record.values[1]);
    EXPECT_EQ(
        1U, counts.forward_domain_present.load(std::memory_order_relaxed));
    EXPECT_EQ(
        0U, counts.reverse_domain_present.load(std::memory_order_relaxed));

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_domain_store_destroy(store);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, SharedDomainVersionInvalidatesFlowMiss)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));
    ktag_domain_store_config store_config{};
    store_config.struct_size = sizeof(store_config);
    store_config.api_version = KTAG_API_VERSION;
    store_config.max_entries = 16U;
    store_config.journal_capacity = 8U;
    ktag_domain_store *store = ktag_domain_store_create(&store_config);
    ASSERT_NE(nullptr, store);
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U, store);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    const std::array<uint8_t, 4> source = {10U, 30U, 0U, 1U};
    const auto frame = udp_frame(
        source, 50000U, {10U, 30U, 0U, 2U}, 443U);
    const auto first_packet = packet_view(frame, 1U, 1U);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &first_packet, batch));
    EXPECT_EQ(
        0U, counts.packet_domain_present.load(std::memory_order_relaxed));

    ktag_ip_address address{};
    address.version = 4U;
    std::memcpy(address.bytes, source.data(), source.size());
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_domain_store_put(
            store, &address, "updated.example.", 0U, nullptr));

    const auto second_packet = packet_view(frame, 2U, 2U);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &second_packet, batch));
    EXPECT_EQ(
        1U, counts.packet_domain_present.load(std::memory_order_relaxed));

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_domain_store_destroy(store);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureContext, SeparateContextsRunConcurrentlyWithSharedDomainStore)
{
    callback_counts counts;
    ktag_feature_registry *registry = nullptr;
    ktag_feature_schema *schema = nullptr;
    ASSERT_TRUE(make_contract(&counts, &registry, &schema));

    ktag_domain_store_config store_config{};
    store_config.struct_size = sizeof(store_config);
    store_config.api_version = KTAG_API_VERSION;
    store_config.max_entries = 64U;
    store_config.journal_capacity = 16U;
    ktag_domain_store *store = ktag_domain_store_create(&store_config);
    ASSERT_NE(nullptr, store);

    ktag_feature_context *first_context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U, store);
    ktag_feature_context *second_context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U, store);
    ktag_feature_batch *first_batch = ktag_feature_batch_create(schema, 1U);
    ktag_feature_batch *second_batch = ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, first_context);
    ASSERT_NE(nullptr, second_context);
    ASSERT_NE(nullptr, first_batch);
    ASSERT_NE(nullptr, second_batch);

    /* Each Context Cache keeps the shared Store alive. */
    ktag_domain_store_destroy(store);
    std::atomic<uint32_t> failures{0U};
    const auto first_frame = udp_frame(
        {198U, 51U, 100U, 1U}, 41001U,
        {198U, 51U, 100U, 2U}, 42001U);
    const auto second_frame = udp_frame(
        {203U, 0U, 113U, 1U}, 41002U,
        {203U, 0U, 113U, 2U}, 42002U);

    std::thread first_worker([&]() {
        const auto packet = packet_view(first_frame, 1U, 1U);
        if (ktag_feature_process_packet(
                first_context, &packet, first_batch) != KTAG_SUCCESS ||
            ktag_feature_flush(first_context, first_batch) != KTAG_SUCCESS ||
            ktag_feature_batch_get_count(first_batch) != 1U) {
            failures.fetch_add(1U, std::memory_order_relaxed);
        }
    });
    std::thread second_worker([&]() {
        const auto packet = packet_view(second_frame, 2U, 2U);
        if (ktag_feature_process_packet(
                second_context, &packet, second_batch) != KTAG_SUCCESS ||
            ktag_feature_flush(second_context, second_batch) != KTAG_SUCCESS ||
            ktag_feature_batch_get_count(second_batch) != 1U) {
            failures.fetch_add(1U, std::memory_order_relaxed);
        }
    });
    first_worker.join();
    second_worker.join();

    EXPECT_EQ(0U, failures.load(std::memory_order_relaxed));
    EXPECT_EQ(2U, counts.provider_create.load(std::memory_order_relaxed));
    EXPECT_EQ(2U, counts.flow_create.load(std::memory_order_relaxed));
    EXPECT_EQ(2U, counts.finish.load(std::memory_order_relaxed));
    EXPECT_EQ(2U, counts.flow_destroy.load(std::memory_order_relaxed));

    ktag_feature_batch_destroy(first_batch);
    ktag_feature_batch_destroy(second_batch);
    ktag_feature_destroy(first_context);
    ktag_feature_destroy(second_context);
    EXPECT_EQ(2U, counts.provider_destroy.load(std::memory_order_relaxed));
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}
