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

typedef struct ktag_loaded_feature_plugin ktag_loaded_feature_plugin;

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
    ktag_status (*register_providers)(void *plugin_state,
                                      ktag_feature_registry *registry);

    /* Optional, but required when plugin_create is supplied. */
    void (*plugin_destroy)(void *plugin_state);
} ktag_feature_plugin_descriptor;

typedef ktag_status (*ktag_feature_plugin_query_fn)(
    uint32_t host_abi_version, ktag_feature_plugin_descriptor *descriptor);

/*
 * Loads one Provider plugin, validates its ABI descriptor and registers its
 * Providers into registry. The returned handle owns the dynamic library and
 * optional plugin state. The Registry and every Feature Context created from
 * it must be destroyed before the handle is unloaded.
 */
KTAG_API ktag_loaded_feature_plugin *
ktag_feature_plugin_load(const char *path, ktag_feature_registry *registry);

KTAG_API void ktag_feature_plugin_unload(ktag_loaded_feature_plugin *plugin);

/* Returned strings are borrowed until the plugin is unloaded. */
KTAG_API const char *
ktag_feature_plugin_get_id(const ktag_loaded_feature_plugin *plugin);

KTAG_API const char *
ktag_feature_plugin_get_version(const ktag_loaded_feature_plugin *plugin);

/* Thread-local detail for the most recent host-side plugin loading failure. */
KTAG_API const char *ktag_feature_plugin_get_last_error(void);

/* Every Feature Provider plugin exports this exact C symbol. The host
 * initializes descriptor.struct_size/api_version before calling it. */
KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version, ktag_feature_plugin_descriptor *descriptor);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_FEATURE_PLUGIN_H */
