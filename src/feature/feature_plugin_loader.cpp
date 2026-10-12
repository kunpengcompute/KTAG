/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature_plugin.h"

#include <dlfcn.h>

#include <cctype>
#include <exception>
#include <new>
#include <string>

struct ktag_loaded_feature_plugin {
    void *library = nullptr;
    ktag_feature_plugin_descriptor descriptor{};
    void *plugin_state = nullptr;
    std::string id;
    std::string version;
};

namespace {

thread_local std::string plugin_last_error;

void set_last_error(const std::string &message) { plugin_last_error = message; }

bool valid_identifier(const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    for (size_t index = 0U; index <= KTAG_PROVIDER_ID_MAX_LENGTH; ++index) {
        const unsigned char character =
            static_cast<unsigned char>(value[index]);
        if (character == '\0') {
            return true;
        }
        if (!std::isalnum(character) && character != '.' && character != '_' &&
            character != '-') {
            return false;
        }
    }
    return false;
}

void destroy_plugin_state(void (*destroy)(void *),
                          void *plugin_state) noexcept {
    if (destroy == nullptr) {
        return;
    }
    try {
        destroy(plugin_state);
    } catch (...) {
        /* A plugin exception must never escape teardown across the C ABI. */
    }
}

void close_library(void *library) {
    if (library != nullptr) {
        (void)dlclose(library);
    }
}

} // namespace

extern "C" {

KTAG_API ktag_loaded_feature_plugin *
ktag_feature_plugin_load(const char *path, ktag_feature_registry *registry) {
    set_last_error(std::string());
    if (path == nullptr || path[0] == '\0' || registry == nullptr) {
        set_last_error("plugin path and Feature Registry are required");
        return nullptr;
    }

    dlerror();
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char *message = dlerror();
        set_last_error(std::string("failed to load plugin '") + path + "': " +
                       (message == nullptr ? "unknown dlopen error" : message));
        return nullptr;
    }

    dlerror();
    void *symbol = dlsym(library, KTAG_FEATURE_PLUGIN_ENTRYPOINT);
    const char *symbol_error = dlerror();
    if (symbol == nullptr || symbol_error != nullptr) {
        set_last_error(std::string("plugin '") + path + "' does not export " +
                       KTAG_FEATURE_PLUGIN_ENTRYPOINT);
        close_library(library);
        return nullptr;
    }

    auto query = reinterpret_cast<ktag_feature_plugin_query_fn>(symbol);
    ktag_feature_plugin_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    ktag_status status = KTAG_ERROR_INTERNAL;
    try {
        status = query(KTAG_FEATURE_PLUGIN_ABI_VERSION, &descriptor);
    } catch (...) {
        set_last_error("plugin query crossed the C ABI with an exception");
        close_library(library);
        return nullptr;
    }
    if (status != KTAG_SUCCESS) {
        set_last_error(std::string("plugin query rejected host ABI for '") +
                       path + "'");
        close_library(library);
        return nullptr;
    }
    if (descriptor.struct_size != sizeof(descriptor) ||
        descriptor.api_version != KTAG_API_VERSION ||
        descriptor.plugin_abi_version != KTAG_FEATURE_PLUGIN_ABI_VERSION ||
        descriptor.reserved != 0U || !valid_identifier(descriptor.plugin_id) ||
        !valid_identifier(descriptor.plugin_version) ||
        descriptor.register_providers == nullptr ||
        (descriptor.plugin_create != nullptr &&
         descriptor.plugin_destroy == nullptr)) {
        set_last_error(
            std::string("plugin descriptor is invalid or ABI-incompatible: ") +
            path);
        close_library(library);
        return nullptr;
    }

    ktag_loaded_feature_plugin *plugin =
        new (std::nothrow) ktag_loaded_feature_plugin();
    if (plugin == nullptr) {
        set_last_error("out of memory preparing loaded plugin metadata");
        close_library(library);
        return nullptr;
    }
    try {
        plugin->id = descriptor.plugin_id;
        plugin->version = descriptor.plugin_version;
    } catch (...) {
        delete plugin;
        set_last_error("out of memory preparing loaded plugin metadata");
        close_library(library);
        return nullptr;
    }
    plugin->library = library;
    plugin->descriptor = descriptor;

    void *plugin_state = nullptr;
    try {
        if (descriptor.plugin_create != nullptr) {
            status = descriptor.plugin_create(&plugin_state);
            if (status != KTAG_SUCCESS) {
                destroy_plugin_state(descriptor.plugin_destroy, plugin_state);
                set_last_error(std::string("plugin_create failed for '") +
                               path + "'");
                close_library(library);
                delete plugin;
                return nullptr;
            }
        }
        status = descriptor.register_providers(plugin_state, registry);
    } catch (...) {
        status = KTAG_ERROR_INTERNAL;
    }
    if (status != KTAG_SUCCESS) {
        destroy_plugin_state(descriptor.plugin_destroy, plugin_state);
        set_last_error(
            std::string("plugin provider registration failed for '") + path +
            "'");
        close_library(library);
        delete plugin;
        return nullptr;
    }

    plugin->plugin_state = plugin_state;
    return plugin;
}

KTAG_API void ktag_feature_plugin_unload(ktag_loaded_feature_plugin *plugin) {
    if (plugin == nullptr) {
        return;
    }
    destroy_plugin_state(plugin->descriptor.plugin_destroy,
                         plugin->plugin_state);
    close_library(plugin->library);
    delete plugin;
}

KTAG_API const char *
ktag_feature_plugin_get_id(const ktag_loaded_feature_plugin *plugin) {
    return plugin == nullptr ? nullptr : plugin->id.c_str();
}

KTAG_API const char *
ktag_feature_plugin_get_version(const ktag_loaded_feature_plugin *plugin) {
    return plugin == nullptr ? nullptr : plugin->version.c_str();
}

KTAG_API const char *ktag_feature_plugin_get_last_error(void) {
    return plugin_last_error.c_str();
}

} // extern "C"
