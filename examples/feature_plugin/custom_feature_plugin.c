/*
 * Minimal dynamically loaded Feature Provider plugin. The demo loader owns
 * the .so lifetime; feature computation still uses the core callbacks ABI.
 */
#include "ktag_feature_plugin.h"

static ktag_status packet_counts_finish(
    void *provider_state,
    void *flow_state,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    (void)provider_state;
    (void)flow_state;
    const float values[2] = {
        (float)ktag_flow_view_get_packet_count(
            flow, KTAG_DIRECTION_FORWARD),
        (float)ktag_flow_view_get_packet_count(
            flow, KTAG_DIRECTION_REVERSE)};
    return ktag_feature_output_set(
        output, "customer.packet_counts", values, 2U);
}

static ktag_status register_customer_provider(
    void *plugin_state,
    ktag_feature_registry *registry)
{
    (void)plugin_state;
    ktag_feature_descriptor feature = {0};
    feature.struct_size = sizeof(feature);
    feature.api_version = KTAG_API_VERSION;
    feature.feature_id = "customer.packet_counts";
    feature.display_name = "customer_packet_counts";
    feature.provider_id = "customer.example.plugin";
    feature.provider_version = 1U;
    feature.value_type = KTAG_FEATURE_FLOAT32;
    feature.dimension = 2U;

    ktag_feature_provider_registration provider = {0};
    provider.struct_size = sizeof(provider);
    provider.api_version = KTAG_API_VERSION;
    provider.provider_id = "customer.example.plugin";
    provider.provider_version = 1U;
    provider.features = &feature;
    provider.feature_count = 1U;
    provider.callbacks.struct_size = sizeof(provider.callbacks);
    provider.callbacks.api_version = KTAG_API_VERSION;
    provider.callbacks.flow_finish = packet_counts_finish;
    provider.callback_error_policy =
        KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW;
    return ktag_feature_registry_register(
        registry, &provider, KTAG_FEATURE_REGISTER_DEFAULT);
}

KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor)
{
    if (host_abi_version != KTAG_FEATURE_PLUGIN_ABI_VERSION ||
        descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->api_version = KTAG_API_VERSION;
    descriptor->plugin_abi_version = KTAG_FEATURE_PLUGIN_ABI_VERSION;
    descriptor->plugin_id = "customer.example.plugin";
    descriptor->plugin_version = "1.0.0";
    descriptor->register_providers = register_customer_provider;
    return KTAG_SUCCESS;
}
