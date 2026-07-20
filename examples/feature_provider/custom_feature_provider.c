/*
 * Minimal customer Feature Provider example. This file is intentionally C so
 * the callback ABI can be used without exposing any C++ implementation type.
 */
#include "ktag_feature.h"

static ktag_status customer_flow_finish(
    void *provider_state,
    void *flow_state,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    (void)provider_state;
    (void)flow_state;
    const float packet_counts[2] = {
        (float)ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_FORWARD),
        (float)ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_REVERSE)};
    return ktag_feature_output_set(
        output, "customer.packet_counts", packet_counts, 2);
}

ktag_status ktag_example_register_customer_provider(
    ktag_feature_registry *registry)
{
    ktag_feature_descriptor feature = {0};
    feature.struct_size = sizeof(feature);
    feature.api_version = KTAG_API_VERSION;
    feature.feature_id = "customer.packet_counts";
    feature.display_name = "customer_packet_counts";
    feature.provider_id = "customer.example";
    feature.provider_version = 1;
    feature.value_type = KTAG_FEATURE_FLOAT32;
    feature.dimension = 2;

    ktag_feature_provider_registration provider = {0};
    provider.struct_size = sizeof(provider);
    provider.api_version = KTAG_API_VERSION;
    provider.provider_id = "customer.example";
    provider.provider_version = 1;
    provider.features = &feature;
    provider.feature_count = 1;
    provider.callbacks.struct_size = sizeof(provider.callbacks);
    provider.callbacks.api_version = KTAG_API_VERSION;
    provider.callbacks.flow_finish = customer_flow_finish;
    provider.callback_error_policy = KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW;

    return ktag_feature_registry_register(
        registry, &provider, KTAG_FEATURE_REGISTER_DEFAULT);
}
