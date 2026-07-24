/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_PLUGIN_H
#define KTAG_FEATURE_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#include "ktag_feature.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KTAG_FEATURE_PLUGIN_ABI_VERSION 1U
#define KTAG_FEATURE_PLUGIN_ENTRYPOINT "ktag_feature_plugin_query"

#if defined(_WIN32) || defined(__CYGWIN__)
#define KTAG_FEATURE_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#define KTAG_FEATURE_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define KTAG_FEATURE_PLUGIN_EXPORT
#endif

typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t plugin_abi_version;
    uint32_t reserved; /* Must be zero for ABI v1. */
    const char *plugin_id;
    const char *plugin_version;

    /* Optional. State remains owned by the plugin until plugin_destroy. */
    ktag_status (*plugin_create)(void **plugin_state);

    /* Required. Providers are registered through the core callbacks API. */
    ktag_status (*register_providers)(
        void *plugin_state,
        ktag_feature_registry *registry);

    /* Optional, but required when plugin_create is supplied. */
    void (*plugin_destroy)(void *plugin_state);
} ktag_feature_plugin_descriptor;

typedef ktag_status (*ktag_feature_plugin_query_fn)(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor);

/* Every Feature Provider plugin exports this exact C symbol. The host
 * initializes descriptor.struct_size/api_version before calling it. */
KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_FEATURE_PLUGIN_H */
