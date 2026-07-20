#include "ktag_feature_plugin.h"

static ktag_status register_nothing(
    void *plugin_state,
    ktag_feature_registry *registry)
{
    (void)plugin_state;
    (void)registry;
    return KTAG_SUCCESS;
}

KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor)
{
    (void)host_abi_version;
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->api_version = KTAG_API_VERSION;
    descriptor->plugin_abi_version = KTAG_FEATURE_PLUGIN_ABI_VERSION + 1U;
    descriptor->plugin_id = "test.bad_abi";
    descriptor->plugin_version = "1.0.0";
    descriptor->register_providers = register_nothing;
    return KTAG_SUCCESS;
}
