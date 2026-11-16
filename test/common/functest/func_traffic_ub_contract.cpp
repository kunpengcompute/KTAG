#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "ktag_feature.h"
#include "ktag_traffic_features.h"
#include "ub_feature_wire.h"

TEST(KTAGTrafficUbContract, Extracted354ValuesSurvivePocWireInBothLifecycles) {
    std::unique_ptr<ktag_feature_schema, decltype(&ktag_feature_schema_destroy)> schema(
        ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
    std::unique_ptr<ktag_feature_registry, decltype(&ktag_feature_registry_destroy)> registry(
        ktag_feature_registry_create(), ktag_feature_registry_destroy);
    ASSERT_NE(nullptr, schema);
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(KTAG_SUCCESS, ktag_traffic_features_register(registry.get()));
    ASSERT_EQ(11U, ktag_feature_registry_get_provider_count(registry.get()));
    ASSERT_EQ(354U, ktag_feature_schema_get_value_count(schema.get()));
    ASSERT_EQ(0xdd48882b53bebfa5ULL, ktag_feature_schema_get_hash(schema.get()));

    // Ethernet + IPv4 + UDP + payload. Both packets belong to the same flow.
    std::vector<uint8_t> frame(46U, 0U);
    frame[12] = 8U;
    frame[14] = 0x45U;
    frame[17] = 32U;
    frame[22] = 64U;
    frame[23] = 17U;
    frame[26] = 10U; frame[29] = 1U;
    frame[30] = 10U; frame[33] = 2U;
    frame[34] = 0x27U; frame[35] = 0x10U;
    frame[36] = 1U; frame[37] = 0xbbU;
    frame[39] = 12U;
    frame[42] = 't'; frame[43] = 'e'; frame[44] = 's'; frame[45] = 't';

    for (const auto lifecycle : {KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT,
                                 KTAG_FEATURE_LIFECYCLE_MODERN}) {
        ktag_feature_config config{};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.schema = schema.get();
        config.registry = registry.get();
        config.udp_packet_limit = 2U;
        config.lifecycle_mode = lifecycle;
        std::unique_ptr<ktag_feature_context, decltype(&ktag_feature_destroy)> context(
            ktag_feature_create(&config), ktag_feature_destroy);
        std::unique_ptr<ktag_feature_batch, decltype(&ktag_feature_batch_destroy)> batch(
            ktag_feature_batch_create(schema.get(), 1U), ktag_feature_batch_destroy);
        ASSERT_NE(nullptr, context);
        ASSERT_NE(nullptr, batch);
        for (uint64_t sequence = 1U; sequence <= 2U; ++sequence) {
            ktag_packet packet{};
            ktag_packet_init(&packet);
            packet.data = frame.data();
            packet.captured_length = packet.original_length = static_cast<uint32_t>(frame.size());
            packet.link_type = KTAG_CAPTURE_LINKTYPE_ETHERNET;
            packet.sequence = sequence;
            packet.timestamp.seconds = 1;
            packet.timestamp.microseconds = static_cast<int32_t>(sequence * 1000U);
            ASSERT_EQ(KTAG_SUCCESS, ktag_feature_process_packet(context.get(), &packet, batch.get()));
        }
        if (lifecycle == KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT) {
            ASSERT_EQ(0U, ktag_feature_batch_get_count(batch.get()));
            ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context.get(), batch.get()));
        }
        ASSERT_EQ(1U, ktag_feature_batch_get_count(batch.get()));
        ktag_feature_record view{};
        ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch.get(), 0U, &view));
        ASSERT_EQ(354U, view.value_count);
        ktag_demo_schema_feature_record record;
        ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record_details(batch.get(), 0U, &record.details));
        record.source_index = 7U;
        record.values.assign(view.values, view.values + view.value_count);
        for (float value : record.values) ASSERT_TRUE(std::isfinite(value));

        ktag_demo_ub_frame_contract contract{};
        contract.schema_hash = view.schema_hash;
        contract.frontend_id_hash = 123U;
        contract.epoch = 17U;
        contract.sequence = 1U;
        contract.ruleset_hash = 31U;
        std::vector<uint8_t> wire(ktag_demo_ub_feature_frame_size(view.value_count));
        size_t encoded = 0U;
        std::string error;
        ASSERT_TRUE(ktag_demo_ub_encode_feature_frame(wire.data(), wire.size(), contract,
                                                     record, true, &encoded, &error)) << error;
        ktag_demo_ub_frame_info info{};
        ktag_demo_schema_feature_record decoded;
        ASSERT_TRUE(ktag_demo_ub_decode_feature_frame(wire.data(), encoded, &info, &decoded, &error)) << error;
        EXPECT_EQ(contract.schema_hash, info.contract.schema_hash);
        EXPECT_EQ(record.source_index, decoded.source_index);
        EXPECT_EQ(record.values, decoded.values);
        EXPECT_EQ(1U, decoded.details.metadata.first_packet_sequence);
        EXPECT_EQ(2U, decoded.details.metadata.last_packet_sequence);
    }
}
