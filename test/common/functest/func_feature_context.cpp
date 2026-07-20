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

namespace {

struct callback_counts {
    std::atomic<uint32_t> provider_create{0U};
    std::atomic<uint32_t> provider_destroy{0U};
    std::atomic<uint32_t> flow_create{0U};
    std::atomic<uint32_t> flow_destroy{0U};
    std::atomic<uint32_t> packet{0U};
    std::atomic<uint32_t> finish{0U};
    std::atomic<uint32_t> forward_domain_present{0U};
    std::atomic<uint32_t> reverse_domain_present{0U};
};

void write_be16(std::vector<uint8_t> *data, size_t offset, uint16_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 8U);
    (*data)[offset + 1U] = static_cast<uint8_t>(value & 0xffU);
}

std::vector<uint8_t> udp_frame(
    const std::array<uint8_t, 4> &source,
    uint16_t source_port,
    const std::array<uint8_t, 4> &destination,
    uint16_t destination_port)
{
    constexpr size_t network_offset = 14U;
    constexpr size_t ip_length = 20U;
    constexpr size_t udp_length = 8U;
    std::vector<uint8_t> frame(network_offset + ip_length + udp_length, 0U);
    write_be16(&frame, 12U, 0x0800U);
    frame[network_offset] = 0x45U;
    write_be16(&frame, network_offset + 2U, ip_length + udp_length);
    frame[network_offset + 8U] = 64U;
    frame[network_offset + 9U] = 17U;
    std::memcpy(frame.data() + network_offset + 12U, source.data(), source.size());
    std::memcpy(frame.data() + network_offset + 16U, destination.data(), destination.size());
    write_be16(&frame, network_offset + ip_length, source_port);
    write_be16(&frame, network_offset + ip_length + 2U, destination_port);
    write_be16(&frame, network_offset + ip_length + 4U, udp_length);
    return frame;
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
    const ktag_flow_view *,
    void **flow_state)
{
    auto *counts = static_cast<callback_counts *>(provider_state);
    counts->flow_create.fetch_add(1U, std::memory_order_relaxed);
    *flow_state = counts;
    return KTAG_SUCCESS;
}

ktag_status on_packet(
    void *provider_state,
    void *,
    const ktag_packet_view *,
    const ktag_flow_view *)
{
    auto *counts = static_cast<callback_counts *>(provider_state);
    counts->packet.fetch_add(1U, std::memory_order_relaxed);
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
    config.domain_authority_lookup_on_miss = 1U;
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

TEST(KTAGFeatureContext, LegacySchemaMatchesCompatibilityExtractor)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_registry_register_legacy_builtins(registry));
    ktag_feature_context *context = make_context(
        schema, registry, 8U, 100U, 1000U, 1000U);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1U);
    feature_extractor *legacy = extractor_init(DLT_EN10MB);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);
    ASSERT_NE(nullptr, legacy);

    const auto frame = udp_frame(
        {10U, 10U, 0U, 1U}, 12345U,
        {10U, 10U, 0U, 2U}, 443U);
    const auto packet = packet_view(frame, 1U, 7U, 8U);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &packet, batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    feature_vector packet_output{};
    int has_feature = 0;
    timeval timestamp{};
    timestamp.tv_sec = 7;
    timestamp.tv_usec = 8;
    ASSERT_EQ(
        EXTRACTOR_SUCCESS,
        process_packet(
            legacy,
            frame.data(),
            static_cast<uint32_t>(frame.size()),
            &timestamp,
            &packet_output,
            &has_feature));
    EXPECT_EQ(0, has_feature);
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

TEST(KTAGFeatureContext, LegacyLifecycleMatchesExtractorPastUdpLimit)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_registry_register_legacy_builtins(registry));
    ktag_feature_context *context = make_context(
        schema,
        registry,
        8U,
        100U,
        10U,
        10U,
        nullptr,
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
    ASSERT_EQ(KTAG_LEGACY_V1_VALUE_COUNT, record.value_count);
    EXPECT_EQ(100U, record.metadata.last_packet_sequence);
    for (uint32_t index = 0U; index < record.value_count; ++index) {
        EXPECT_FLOAT_EQ(legacy_output.features[index], record.values[index])
            << "legacy UDP-limit value mismatch at index " << index;
    }

    extractor_destroy(legacy);
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
