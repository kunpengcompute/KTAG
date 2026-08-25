/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <gtest/gtest.h>

#include "feature/feature_pipeline_internal.h"

namespace {

struct lifecycle_counts {
    uint32_t provider_create = 0;
    uint32_t provider_destroy = 0;
    uint32_t flow_create = 0;
    uint32_t flow_destroy = 0;
    uint32_t packet = 0;
    uint32_t finish = 0;
};

ktag_feature_descriptor descriptor(
    const char *feature_id,
    const char *display_name,
    uint32_t dimension)
{
    ktag_feature_descriptor value{};
    value.struct_size = sizeof(value);
    value.api_version = KTAG_API_VERSION;
    value.feature_id = feature_id;
    value.display_name = display_name;
    value.provider_id = "test.lifecycle";
    value.provider_version = 1;
    value.value_type = KTAG_FEATURE_FLOAT32;
    value.dimension = dimension;
    return value;
}

ktag_status provider_create(
    const ktag_feature_provider_config *config,
    void **provider_state)
{
    if (config == nullptr || provider_state == nullptr || config->user_data == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    auto *counts = static_cast<lifecycle_counts *>(config->user_data);
    ++counts->provider_create;
    *provider_state = counts;
    return KTAG_SUCCESS;
}

void provider_destroy(void *provider_state)
{
    ++static_cast<lifecycle_counts *>(provider_state)->provider_destroy;
}

ktag_status flow_create(
    void *provider_state,
    const ktag_flow_view *flow,
    void **flow_state)
{
    if (provider_state == nullptr || flow == nullptr || flow_state == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    auto *counts = static_cast<lifecycle_counts *>(provider_state);
    ++counts->flow_create;
    *flow_state = counts;
    return KTAG_SUCCESS;
}

void flow_destroy(void *, void *flow_state)
{
    ++static_cast<lifecycle_counts *>(flow_state)->flow_destroy;
}

ktag_status on_packet(
    void *,
    void *flow_state,
    const ktag_packet_view *packet,
    const ktag_flow_view *)
{
    uint32_t length = 0;
    ktag_timestamp timestamp{};
    const uint8_t *data = ktag_packet_view_get_data(packet, &length);
    if (flow_state == nullptr || data == nullptr || length != 3U || data[0] != 0x11U ||
        ktag_packet_view_get_timestamp(packet, &timestamp) != KTAG_SUCCESS ||
        timestamp.seconds != 7 || timestamp.microseconds != 8) {
        return KTAG_ERROR_INTERNAL;
    }
    ++static_cast<lifecycle_counts *>(flow_state)->packet;
    return KTAG_SUCCESS;
}

ktag_status set_if_enabled(
    ktag_feature_output *output,
    const char *feature_id,
    const float *values,
    uint32_t value_count)
{
    const ktag_status status = ktag_feature_output_set(output, feature_id, values, value_count);
    return status == KTAG_ERROR_NOT_FOUND ? KTAG_SUCCESS : status;
}

ktag_status flow_finish(
    void *,
    void *flow_state,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    if (flow_state == nullptr) {
        return KTAG_ERROR_INTERNAL;
    }
    auto *counts = static_cast<lifecycle_counts *>(flow_state);
    ++counts->finish;
    const float vector[] = {
        static_cast<float>(counts->packet),
        static_cast<float>(ktag_flow_view_get_flow_id(flow))};
    const float forward_packets = static_cast<float>(
        ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_FORWARD));
    if (set_if_enabled(output, "test.vector", vector, 2) != KTAG_SUCCESS ||
        set_if_enabled(output, "test.forward_packets", &forward_packets, 1) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    return KTAG_SUCCESS;
}

ktag_status failing_finish(
    void *,
    void *,
    const ktag_flow_view *,
    ktag_feature_output *output)
{
    const float partial = 42.0F;
    (void)ktag_feature_output_set(output, "test.failure", &partial, 1);
    return KTAG_ERROR_INTERNAL;
}

ktag_status annotation_finish(
    void *,
    void *,
    const ktag_flow_view *,
    ktag_feature_output *output)
{
    const float value = 1.0F;
    if (ktag_feature_output_set(output, "test.annotation", &value, 1U) !=
            KTAG_SUCCESS ||
        ktag_feature_output_annotations_enabled(output) == 0U) {
        return KTAG_ERROR_INTERNAL;
    }
    return ktag_feature_output_set_annotation(
        output, "test.note", "provider-owned text");
}

ktag_feature_provider_registration registration(
    const ktag_feature_descriptor *features,
    uint32_t feature_count,
    lifecycle_counts *counts)
{
    ktag_feature_provider_registration value{};
    value.struct_size = sizeof(value);
    value.api_version = KTAG_API_VERSION;
    value.provider_id = "test.lifecycle";
    value.provider_version = 1;
    value.features = features;
    value.feature_count = feature_count;
    value.callbacks.struct_size = sizeof(value.callbacks);
    value.callbacks.api_version = KTAG_API_VERSION;
    value.callbacks.provider_create = provider_create;
    value.callbacks.flow_create = flow_create;
    value.callbacks.on_packet = on_packet;
    value.callbacks.flow_finish = flow_finish;
    value.callbacks.flow_destroy = flow_destroy;
    value.callbacks.provider_destroy = provider_destroy;
    value.user_data = counts;
    return value;
}

} // namespace

TEST(KTAGFeatureProvider, RunsCustomCallbacksByFeatureIdAndSchemaOrder)
{
    lifecycle_counts counts{};
    const ktag_feature_descriptor features[] = {
        descriptor("test.vector", "vector", 2),
        descriptor("test.forward_packets", "forward_packets", 1)};
    auto provider = registration(features, 2, &counts);

    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create("custom_execution", 1);
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(
                                registry, &provider, KTAG_FEATURE_REGISTER_DEFAULT));

    /* Schema order intentionally differs from callback write order. */
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_schema_add_registered(schema, registry, "test.forward_packets"));
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_schema_add_registered(schema, registry, "test.vector"));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));

    ktag_feature_pipeline *pipeline = ktag_feature_pipeline_create_internal(schema, registry);
    ASSERT_NE(nullptr, pipeline);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
    EXPECT_EQ(1U, counts.provider_create);

    ktag_feature_flow_runtime runtime{};
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_flow_runtime_create_internal(pipeline, 77, &runtime));
    runtime.flow.directions[0U].packet_count = 3U;
    const uint8_t packet[] = {0x11U, 0x22U, 0x33U};
    const ktag_timestamp timestamp{7, 8};
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flow_runtime_on_packet_internal(
                                &runtime, packet, sizeof(packet), &timestamp));

    float output[] = {-1.0F, -1.0F, -1.0F};
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_flow_runtime_finish_internal(&runtime, output, 3));
    EXPECT_FLOAT_EQ(3.0F, output[0]);
    EXPECT_FLOAT_EQ(1.0F, output[1]);
    EXPECT_FLOAT_EQ(77.0F, output[2]);
    EXPECT_EQ(1U, counts.flow_create);
    EXPECT_EQ(1U, counts.packet);
    EXPECT_EQ(1U, counts.finish);
    EXPECT_EQ(1U, counts.flow_destroy);

    ktag_feature_pipeline_destroy_internal(pipeline);
    EXPECT_EQ(1U, counts.provider_destroy);
}

TEST(KTAGFeatureProvider, FillZeroPolicyClearsPartialCallbackOutput)
{
    auto feature = descriptor("test.failure", "failure", 1);
    ktag_feature_provider_registration provider{};
    provider.struct_size = sizeof(provider);
    provider.api_version = KTAG_API_VERSION;
    provider.provider_id = "test.lifecycle";
    provider.provider_version = 1;
    provider.features = &feature;
    provider.feature_count = 1;
    provider.callbacks.struct_size = sizeof(provider.callbacks);
    provider.callbacks.api_version = KTAG_API_VERSION;
    provider.callbacks.flow_finish = failing_finish;
    provider.callback_error_policy = KTAG_FEATURE_CALLBACK_ERROR_FILL_ZERO;

    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create("fill_zero", 1);
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(
                                registry, &provider, KTAG_FEATURE_REGISTER_DEFAULT));
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_schema_add_registered(schema, registry, "test.failure"));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    ktag_feature_pipeline *pipeline = ktag_feature_pipeline_create_internal(schema, registry);
    ASSERT_NE(nullptr, pipeline);

    ktag_feature_flow_runtime runtime{};
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_flow_runtime_create_internal(pipeline, 1, &runtime));
    float output = -1.0F;
    EXPECT_EQ(KTAG_SUCCESS,
              ktag_feature_flow_runtime_finish_internal(&runtime, &output, 1));
    EXPECT_FLOAT_EQ(0.0F, output);

    ktag_feature_pipeline_destroy_internal(pipeline);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGFeatureProvider, CollectsOptionalProviderAnnotations)
{
    auto feature = descriptor("test.annotation", "annotation", 1);
    ktag_feature_provider_registration provider{};
    provider.struct_size = sizeof(provider);
    provider.api_version = KTAG_API_VERSION;
    provider.provider_id = "test.lifecycle";
    provider.provider_version = 1;
    provider.features = &feature;
    provider.feature_count = 1;
    provider.callbacks.struct_size = sizeof(provider.callbacks);
    provider.callbacks.api_version = KTAG_API_VERSION;
    provider.callbacks.flow_finish = annotation_finish;

    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema =
        ktag_feature_schema_create("provider_annotation", 1);
    ASSERT_NE(nullptr, registry);
    ASSERT_NE(nullptr, schema);
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(
                                registry, &provider,
                                KTAG_FEATURE_REGISTER_DEFAULT));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add_registered(
                                schema, registry, "test.annotation"));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));

    ktag_feature_pipeline *pipeline =
        ktag_feature_pipeline_create_internal(schema, registry);
    ASSERT_NE(nullptr, pipeline);
    pipeline->collect_annotations = true;

    ktag_feature_flow_runtime runtime{};
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_flow_runtime_create_internal(pipeline, 1, &runtime));
    float output = 0.0F;
    ASSERT_EQ(KTAG_SUCCESS,
              ktag_feature_flow_runtime_finish_internal(&runtime, &output, 1));
    ASSERT_EQ(1U, runtime.annotations.count("test.note"));
    EXPECT_EQ("provider-owned text", runtime.annotations.at("test.note"));

    ktag_feature_flow_runtime_destroy_internal(&runtime);
    ktag_feature_pipeline_destroy_internal(pipeline);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}
