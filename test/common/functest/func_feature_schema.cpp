/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ktag_feature.h"

namespace {

ktag_feature_descriptor make_descriptor(
    const char *feature_id,
    const char *display_name,
    uint32_t dimension = 1,
    uint32_t provider_version = 1)
{
    ktag_feature_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    descriptor.feature_id = feature_id;
    descriptor.display_name = display_name;
    descriptor.provider_id = "test.provider";
    descriptor.provider_version = provider_version;
    descriptor.value_type = KTAG_FEATURE_FLOAT32;
    descriptor.dimension = dimension;
    return descriptor;
}

ktag_status test_flow_finish(
    void *,
    void *,
    const ktag_flow_view *,
    ktag_feature_output *)
{
    return KTAG_SUCCESS;
}

ktag_feature_provider_registration make_registration(
    const char *provider_id,
    uint32_t provider_version,
    const ktag_feature_descriptor *features,
    uint32_t feature_count)
{
    ktag_feature_provider_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.api_version = KTAG_API_VERSION;
    registration.provider_id = provider_id;
    registration.provider_version = provider_version;
    registration.features = features;
    registration.feature_count = feature_count;
    registration.callbacks.struct_size = sizeof(registration.callbacks);
    registration.callbacks.api_version = KTAG_API_VERSION;
    registration.callbacks.flow_finish = test_flow_finish;
    return registration;
}

} // namespace

TEST(KTAGFeatureSchema, CreateRejectsInvalidArguments)
{
    EXPECT_EQ(nullptr, ktag_feature_schema_create(nullptr, 1));
    EXPECT_EQ(nullptr, ktag_feature_schema_create("", 1));
    EXPECT_EQ(nullptr, ktag_feature_schema_create("test", 0));
}

TEST(KTAGFeatureSchema, AddRemoveFreezeAndFind)
{
    ktag_feature_schema *schema = ktag_feature_schema_create("test_schema", 1);
    ASSERT_NE(nullptr, schema);

    auto scalar = make_descriptor("test.scalar", "scalar");
    auto vector = make_descriptor("test.vector", "vector", 4);
    auto invalid_version = make_descriptor("test.invalid", "invalid", 1, 0);
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_feature_schema_add(schema, &invalid_version));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema, &scalar));
    EXPECT_EQ(KTAG_ERROR_ALREADY_EXISTS, ktag_feature_schema_add(schema, &scalar));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema, &vector));
    EXPECT_EQ(2U, ktag_feature_schema_get_feature_count(schema));
    EXPECT_EQ(5U, ktag_feature_schema_get_value_count(schema));

    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_remove(schema, "test.scalar"));
    EXPECT_EQ(KTAG_ERROR_NOT_FOUND, ktag_feature_schema_remove(schema, "test.missing"));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema, &scalar));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    EXPECT_EQ(1U, ktag_feature_schema_is_frozen(schema));
    EXPECT_NE(0U, ktag_feature_schema_get_hash(schema));
    EXPECT_EQ(KTAG_ERROR_INVALID_STATE, ktag_feature_schema_remove(schema, "test.scalar"));
    EXPECT_EQ(KTAG_ERROR_INVALID_STATE, ktag_feature_schema_add(schema, &scalar));

    ktag_feature_descriptor found{};
    uint32_t offset = 0;
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_find(schema, "test.vector", &found, &offset));
    EXPECT_EQ(0U, offset);
    EXPECT_STREQ("vector", found.display_name);
    EXPECT_EQ(4U, found.dimension);
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_find(schema, "test.scalar", &found, &offset));
    EXPECT_EQ(4U, offset);

    ktag_feature_schema_destroy(schema);
}

TEST(KTAGFeatureSchema, HashIsStableAndOrderSensitive)
{
    auto first = make_descriptor("test.first", "first");
    auto second = make_descriptor("test.second", "second", 2);

    ktag_feature_schema *schema_a = ktag_feature_schema_create("stable", 1);
    ktag_feature_schema *schema_b = ktag_feature_schema_create("stable", 1);
    ktag_feature_schema *schema_reordered = ktag_feature_schema_create("stable", 1);
    ASSERT_NE(nullptr, schema_a);
    ASSERT_NE(nullptr, schema_b);
    ASSERT_NE(nullptr, schema_reordered);

    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema_a, &first));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema_a, &second));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema_b, &first));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema_b, &second));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema_reordered, &second));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema_reordered, &first));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema_a));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema_b));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema_reordered));

    EXPECT_EQ(ktag_feature_schema_get_hash(schema_a), ktag_feature_schema_get_hash(schema_b));
    EXPECT_NE(ktag_feature_schema_get_hash(schema_a), ktag_feature_schema_get_hash(schema_reordered));

    ktag_feature_schema_destroy(schema_a);
    ktag_feature_schema_destroy(schema_b);
    ktag_feature_schema_destroy(schema_reordered);
}

TEST(KTAGFeatureSchema, LegacySchemaMatchesExistingContract)
{
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, schema);
    EXPECT_EQ(KTAG_LEGACY_V1_VALUE_COUNT, ktag_feature_schema_get_value_count(schema));
    EXPECT_EQ(
        0xdd48882b53bebfa5ULL,
        ktag_feature_schema_get_hash(schema));

    ktag_feature_descriptor descriptor{};
    uint32_t offset = 0;
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_find(schema, "legacy.flow_id", &descriptor, &offset));
    EXPECT_EQ(0U, offset);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_find(schema, "traffic.send.packet_count", &descriptor, &offset));
    EXPECT_EQ(7U, offset);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_find(schema, "traffic.receive.packet_count", &descriptor, &offset));
    EXPECT_EQ(165U, offset);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_find(schema, "rawbow.crc_histogram", &descriptor, &offset));
    EXPECT_EQ(322U, offset);
    EXPECT_EQ(32U, descriptor.dimension);

    ktag_feature_schema_destroy(schema);
}

TEST(KTAGFeatureSchema, LegacySchemaReproducesExistingCsvHeader)
{
    std::vector<std::string> expected = {
        "flow_id", "src_ip", "dst_ip", "src_port", "dst_port", "protocol"};
    const char *directions[] = {"send", "receive"};
    const char *types[] = {"hdr", "pld", "interval"};
    const char *statistics[] = {"min", "max", "mean", "var"};
    const char *histograms[] = {"hist_rel", "hist_abs", "hist_nor"};

    for (const char *direction : directions) {
        expected.emplace_back(std::string(direction) + "_byte_nums");
        expected.emplace_back(std::string(direction) + "_packet_nums");
        for (const char *type : types) {
            const std::string prefix = std::string(direction) + "_" + type + "_";
            for (const char *statistic : statistics) {
                expected.emplace_back(prefix + statistic);
            }
            for (const char *histogram : histograms) {
                for (uint32_t index = 0; index < 16; ++index) {
                    expected.emplace_back(prefix + histogram + "_" + std::to_string(index));
                }
            }
        }
    }
    for (uint32_t index = 0; index < 32; ++index) {
        expected.emplace_back("raw_bow_crc_hist_" + std::to_string(index));
    }

    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, schema);
    std::vector<std::string> actual;
    for (uint32_t index = 0; index < ktag_feature_schema_get_feature_count(schema); ++index) {
        ktag_feature_descriptor descriptor{};
        uint32_t offset = 0;
        ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_get_descriptor(schema, index, &descriptor, &offset));
        for (uint32_t dimension = 0; dimension < descriptor.dimension; ++dimension) {
            if (descriptor.dimension == 1) {
                actual.emplace_back(descriptor.display_name);
            } else {
                actual.emplace_back(std::string(descriptor.display_name) + "_" + std::to_string(dimension));
            }
        }
    }

    EXPECT_EQ(KTAG_LEGACY_V1_VALUE_COUNT, expected.size());
    EXPECT_EQ(expected, actual);
    ktag_feature_schema_destroy(schema);
}

TEST(KTAGFeatureBatch, AppendsDeepCopyAndValidatesSchemaWidth)
{
    ktag_feature_schema *schema = ktag_feature_schema_create("batch", 1);
    ASSERT_NE(nullptr, schema);
    auto descriptor = make_descriptor("batch.values", "values", 3);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema, &descriptor));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));

    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 1);
    ASSERT_NE(nullptr, batch);
    EXPECT_EQ(3U, ktag_feature_batch_get_value_count(batch));
    EXPECT_EQ(ktag_feature_schema_get_hash(schema), ktag_feature_batch_get_schema_hash(batch));

    ktag_feature_record_metadata metadata{};
    metadata.struct_size = sizeof(metadata);
    metadata.api_version = KTAG_API_VERSION;
    metadata.flow_id = 42;
    metadata.first_packet_sequence = 10;
    metadata.last_packet_sequence = 20;
    metadata.flow_key.address_a.version = 4U;
    metadata.flow_key.address_a.bytes[0] = 192U;
    metadata.flow_key.address_a.bytes[1] = 0U;
    metadata.flow_key.address_a.bytes[2] = 2U;
    metadata.flow_key.address_a.bytes[3] = 1U;
    metadata.flow_key.address_b.version = 4U;
    metadata.flow_key.address_b.bytes[0] = 198U;
    metadata.flow_key.address_b.bytes[1] = 51U;
    metadata.flow_key.address_b.bytes[2] = 100U;
    metadata.flow_key.address_b.bytes[3] = 2U;
    metadata.flow_key.port_a = 12345U;
    metadata.flow_key.port_b = 443U;
    metadata.flow_key.protocol = 6U;
    float values[] = {1.0F, 2.0F, 3.0F};

    EXPECT_EQ(KTAG_ERROR_SCHEMA_MISMATCH, ktag_feature_batch_append(batch, &metadata, values, 2));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_append(batch, &metadata, values, 3));
    values[0] = 99.0F;
    EXPECT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_get_record(batch, 0, &record));
    EXPECT_EQ(42U, record.metadata.flow_id);
    EXPECT_EQ(3U, record.value_count);
    EXPECT_FLOAT_EQ(1.0F, record.values[0]);
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_feature_batch_get_record(batch, 1, &record));

    ktag_feature_record_details details{};
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_batch_get_record_details(batch, 0U, &details));
    EXPECT_EQ(42U, details.metadata.flow_id);
    EXPECT_EQ(4U, details.source_address.version);
    EXPECT_EQ(192U, details.source_address.bytes[0]);
    EXPECT_EQ(4U, details.destination_address.version);
    EXPECT_EQ(198U, details.destination_address.bytes[0]);
    EXPECT_EQ(12345U, details.source_port);
    EXPECT_EQ(443U, details.destination_port);
    EXPECT_EQ(0U, details.ipv6_extension_packet_count);
    EXPECT_EQ(0U, details.ip_fragment_first_packet_count);
    EXPECT_EQ(
        KTAG_ERROR_INVALID_ARGUMENT,
        ktag_feature_batch_get_record_details(batch, 1U, &details));

    ktag_feature_batch_clear(batch);
    EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
    ktag_feature_batch_destroy(batch);
    ktag_feature_schema_destroy(schema);
}

TEST(KTAGFeatureRegistry, RegisterAddToSchemaReplaceAndUnregister)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);

    auto feature = make_descriptor("custom.feature", "custom", 2);
    auto registration = make_registration("test.provider", 1, &feature, 1);
    auto invalid_policy = registration;
    invalid_policy.callback_error_policy =
        static_cast<ktag_feature_callback_error_policy>(99);
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT,
              ktag_feature_registry_register(
                  registry, &invalid_policy, KTAG_FEATURE_REGISTER_DEFAULT));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(
                                registry, &registration, KTAG_FEATURE_REGISTER_DEFAULT));
    EXPECT_EQ(KTAG_ERROR_ALREADY_EXISTS, ktag_feature_registry_register(
                                             registry, &registration, KTAG_FEATURE_REGISTER_DEFAULT));
    EXPECT_EQ(1U, ktag_feature_registry_get_provider_count(registry));

    ktag_feature_schema *schema = ktag_feature_schema_create("custom", 1);
    ASSERT_NE(nullptr, schema);
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_add_registered(schema, registry, "custom.feature"));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    EXPECT_EQ(2U, ktag_feature_schema_get_value_count(schema));
    ktag_feature_schema_destroy(schema);

    auto replacement_feature = make_descriptor("custom.feature", "custom_replaced", 3, 2);
    auto replacement = make_registration("test.provider", 2, &replacement_feature, 1);
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(
                                registry, &replacement, KTAG_FEATURE_REGISTER_REPLACE));
    schema = ktag_feature_schema_create("replacement", 1);
    ASSERT_NE(nullptr, schema);
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_add_registered(schema, registry, "custom.feature"));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    EXPECT_EQ(3U, ktag_feature_schema_get_value_count(schema));
    ktag_feature_schema_destroy(schema);

    auto foreign_feature = make_descriptor("custom.feature", "foreign");
    foreign_feature.provider_id = "foreign.provider";
    auto foreign = make_registration("foreign.provider", 1, &foreign_feature, 1);
    EXPECT_EQ(KTAG_ERROR_ALREADY_EXISTS,
              ktag_feature_registry_register(
                  registry, &foreign, KTAG_FEATURE_REGISTER_DEFAULT));

    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_registry_unregister(registry, "test.provider"));
    EXPECT_EQ(KTAG_ERROR_NOT_FOUND, ktag_feature_registry_unregister(registry, "test.provider"));
    EXPECT_EQ(0U, ktag_feature_registry_get_provider_count(registry));
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureRegistry, LegacyBuiltinRegistrationRollsBackOnCollision)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);

    auto feature = make_descriptor("customer.rawbow", "customer_rawbow");
    feature.provider_id = "builtin.legacy.rawbow";
    auto occupied = make_registration("builtin.legacy.rawbow", 1, &feature, 1);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(
                                registry, &occupied, KTAG_FEATURE_REGISTER_DEFAULT));
    EXPECT_EQ(KTAG_ERROR_ALREADY_EXISTS,
              ktag_feature_registry_register_legacy_builtins(registry));
    EXPECT_EQ(1U, ktag_feature_registry_get_provider_count(registry));
    EXPECT_EQ(KTAG_ERROR_NOT_FOUND,
              ktag_feature_registry_unregister(registry, "builtin.legacy.metadata"));
    EXPECT_EQ(KTAG_SUCCESS,
              ktag_feature_registry_unregister(registry, "builtin.legacy.rawbow"));

    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureRegistry, CustomerProviderCanReplaceBuiltinFeatureContract)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register_legacy_builtins(registry));

    auto feature = make_descriptor("legacy.flow_id", "customer_flow_id", 2, 2);
    feature.provider_id = "builtin.legacy.metadata";
    auto replacement = make_registration("builtin.legacy.metadata", 2, &feature, 1);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(
                                registry, &replacement, KTAG_FEATURE_REGISTER_REPLACE));
    EXPECT_EQ(4U, ktag_feature_registry_get_provider_count(registry));

    ktag_feature_schema *schema = ktag_feature_schema_create("replaced_builtin", 1);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_schema_add_registered(schema, registry, "legacy.flow_id"));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    EXPECT_EQ(2U, ktag_feature_schema_get_value_count(schema));

    ktag_feature_descriptor selected{};
    uint32_t offset = 0;
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_schema_find(schema, "legacy.flow_id", &selected, &offset));
    EXPECT_EQ(2U, selected.provider_version);
    EXPECT_EQ(2U, selected.dimension);
    EXPECT_STREQ("customer_flow_id", selected.display_name);

    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureRegistry, LegacyBuiltinsCoverLegacySchema)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register_legacy_builtins(registry));
    EXPECT_EQ(4U, ktag_feature_registry_get_provider_count(registry));

    ktag_feature_schema *schema = ktag_feature_schema_create("selected_builtin_features", 1);
    ASSERT_NE(nullptr, schema);
    EXPECT_EQ(KTAG_SUCCESS,
              ktag_feature_schema_add_registered(schema, registry, "traffic.send.packet_count"));
    EXPECT_EQ(KTAG_SUCCESS,
              ktag_feature_schema_add_registered(schema, registry, "rawbow.crc_histogram"));
    EXPECT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    EXPECT_EQ(33U, ktag_feature_schema_get_value_count(schema));

    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}
