#include "ktag_feature_plugin.h"

static ktag_status create_failure(void **plugin_state)
{
    if (plugin_state != NULL) {
        *plugin_state = NULL;
    }
    return KTAG_ERROR_INTERNAL;
}

static ktag_status register_nothing(
    void *plugin_state,
    ktag_feature_registry *registry)
{
    (void)plugin_state;
    (void)registry;
    return KTAG_SUCCESS;
}

static void destroy_nothing(void *plugin_state)
{
    (void)plugin_state;
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
    descriptor->plugin_id = "test.create_fail";
    descriptor->plugin_version = "1.0.0";
    descriptor->plugin_create = create_failure;
    descriptor->register_providers = register_nothing;
    descriptor->plugin_destroy = destroy_nothing;
    return KTAG_SUCCESS;
}
