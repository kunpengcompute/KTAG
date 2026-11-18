/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "feature_plugin_loader.h"
#include "../../../src/feature/feature_plugin_validation.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace {

bool valid_identifier(const char *value)
{
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    for (size_t index = 0U; index <= KTAG_PROVIDER_ID_MAX_LENGTH; ++index) {
        const unsigned char character =
            static_cast<unsigned char>(value[index]);
        if (character == '\0') {
            return true;
        }
        if (!std::isalnum(character) && character != '.' &&
            character != '_' && character != '-') {
            return false;
        }
    }
    return false;
}

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

void destroy_plugin_state(
    void (*destroy)(void *),
    void *plugin_state) noexcept
{
    if (destroy == nullptr) {
        return;
    }
    try {
        destroy(plugin_state);
    } catch (...) {
        /* A plugin exception must never escape teardown across the C ABI. */
    }
}

} // namespace

ktag_demo_feature_plugin_loader::~ktag_demo_feature_plugin_loader()
{
    unload_all();
}

bool ktag_demo_feature_plugin_loader::load(
    const std::string &path,
    ktag_feature_registry *registry,
    std::string *error)
{
    if (path.empty() || registry == nullptr) {
        set_error(error, "plugin path and Feature Registry are required");
        return false;
    }
    if (std::any_of(
            plugins_.cbegin(), plugins_.cend(),
            [&path](const loaded_plugin &plugin) {
                return plugin.path == path;
            })) {
        set_error(error, "plugin path is already loaded: " + path);
        return false;
    }

    if (!ktag_feature_plugin_internal::validate_file(path.c_str(), error)) {
        return false;
    }

    dlerror();
    void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char *message = dlerror();
        set_error(
            error,
            "failed to load plugin '" + path + "': " +
                (message == nullptr ? "unknown dlopen error" : message));
        return false;
    }

    dlerror();
    void *symbol = dlsym(library, KTAG_FEATURE_PLUGIN_ENTRYPOINT);
    const char *symbol_error = dlerror();
    if (symbol == nullptr || symbol_error != nullptr) {
        set_error(
            error,
            "plugin '" + path + "' does not export " +
                KTAG_FEATURE_PLUGIN_ENTRYPOINT);
        dlclose(library);
        return false;
    }

    auto query = reinterpret_cast<ktag_feature_plugin_query_fn>(symbol);
    ktag_feature_plugin_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    ktag_status status = KTAG_ERROR_INTERNAL;
    try {
        status = query(KTAG_FEATURE_PLUGIN_ABI_VERSION, &descriptor);
    } catch (...) {
        set_error(error, "plugin query crossed the C ABI with an exception");
        dlclose(library);
        return false;
    }
    if (status != KTAG_SUCCESS) {
        set_error(error, "plugin query rejected host ABI for '" + path + "'");
        dlclose(library);
        return false;
    }
    if (descriptor.struct_size != sizeof(descriptor) ||
        descriptor.api_version != KTAG_API_VERSION ||
        descriptor.plugin_abi_version != KTAG_FEATURE_PLUGIN_ABI_VERSION ||
        descriptor.reserved != 0U ||
        !valid_identifier(descriptor.plugin_id) ||
        !valid_identifier(descriptor.plugin_version) ||
        descriptor.register_providers == nullptr ||
        (descriptor.plugin_create != nullptr &&
         descriptor.plugin_destroy == nullptr)) {
        set_error(error, "plugin descriptor is invalid or ABI-incompatible: " + path);
        dlclose(library);
        return false;
    }
    if (std::any_of(
            plugins_.cbegin(), plugins_.cend(),
            [&descriptor](const loaded_plugin &plugin) {
                return plugin.id == descriptor.plugin_id;
            })) {
        set_error(
            error,
            "plugin ID is already loaded: " +
                std::string(descriptor.plugin_id));
        dlclose(library);
        return false;
    }

    /* Complete all host-side allocations before callbacks can mutate the
     * Registry. Once registration succeeds, recording the plugin must not
     * fail and leave callbacks pointing into an unloaded .so. */
    loaded_plugin plugin;
    try {
        plugin.library = library;
        plugin.descriptor = descriptor;
        plugin.path = path;
        plugin.id = descriptor.plugin_id;
        plugin.version = descriptor.plugin_version;
        plugins_.reserve(plugins_.size() + 1U);
    } catch (...) {
        dlclose(library);
        set_error(error, "out of memory preparing loaded plugin metadata");
        return false;
    }

    void *plugin_state = nullptr;
    try {
        if (descriptor.plugin_create != nullptr) {
            status = descriptor.plugin_create(&plugin_state);
            if (status != KTAG_SUCCESS) {
                destroy_plugin_state(
                    descriptor.plugin_destroy, plugin_state);
                set_error(error, "plugin_create failed for '" + path + "'");
                dlclose(library);
                return false;
            }
        }
        status = descriptor.register_providers(plugin_state, registry);
    } catch (...) {
        status = KTAG_ERROR_INTERNAL;
    }
    if (status != KTAG_SUCCESS) {
        destroy_plugin_state(descriptor.plugin_destroy, plugin_state);
        set_error(error, "plugin provider registration failed for '" + path + "'");
        dlclose(library);
        return false;
    }

    plugin.plugin_state = plugin_state;
    plugins_.push_back(std::move(plugin));
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void ktag_demo_feature_plugin_loader::unload_all()
{
    for (auto plugin = plugins_.rbegin(); plugin != plugins_.rend(); ++plugin) {
        destroy_plugin_state(
            plugin->descriptor.plugin_destroy, plugin->plugin_state);
        plugin->plugin_state = nullptr;
        if (plugin->library != nullptr) {
            dlclose(plugin->library);
            plugin->library = nullptr;
        }
    }
    plugins_.clear();
}

size_t ktag_demo_feature_plugin_loader::size() const
{
    return plugins_.size();
}

const std::string &ktag_demo_feature_plugin_loader::plugin_id(
    size_t index) const
{
    static const std::string empty;
    return index < plugins_.size() ? plugins_[index].id : empty;
}
