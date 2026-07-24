/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "provider_registry_internal.h"

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace {

bool valid_identifier(const char *value, uint32_t maximum_length)
{
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    for (uint32_t index = 0; index <= maximum_length; ++index) {
        if (value[index] == '\0') {
            return true;
        }
    }
    return false;
}

bool callbacks_are_valid(const ktag_feature_callbacks &callbacks)
{
    return callbacks.struct_size >= sizeof(ktag_feature_callbacks) &&
           callbacks.api_version == KTAG_API_VERSION && callbacks.flow_finish != nullptr &&
           (callbacks.provider_create != nullptr || callbacks.provider_destroy == nullptr) &&
           (callbacks.flow_create != nullptr || callbacks.flow_destroy == nullptr);
}

auto find_provider(ktag_feature_registry *registry, const char *provider_id)
{
    return std::find_if(registry->providers.begin(), registry->providers.end(),
                        [provider_id](const ktag_feature_provider_storage &provider) {
                            return provider.provider_id == provider_id;
                        });
}

auto find_provider(const ktag_feature_registry *registry, const char *provider_id)
{
    return std::find_if(registry->providers.cbegin(), registry->providers.cend(),
                        [provider_id](const ktag_feature_provider_storage &provider) {
                            return provider.provider_id == provider_id;
                        });
}

bool registration_is_valid(const ktag_feature_provider_registration *registration)
{
    if (registration == nullptr || registration->struct_size < sizeof(*registration) ||
        registration->api_version != KTAG_API_VERSION ||
        !valid_identifier(registration->provider_id, KTAG_PROVIDER_ID_MAX_LENGTH) ||
        registration->provider_version == 0 || registration->features == nullptr ||
        registration->feature_count == 0 || !callbacks_are_valid(registration->callbacks) ||
        (registration->callback_error_policy != KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW &&
         registration->callback_error_policy != KTAG_FEATURE_CALLBACK_ERROR_FILL_ZERO)) {
        return false;
    }

    for (uint32_t index = 0; index < registration->feature_count; ++index) {
        const auto &feature = registration->features[index];
        if (!ktag_feature_descriptor_is_valid(&feature) ||
            std::string(registration->provider_id) != feature.provider_id ||
            registration->provider_version != feature.provider_version) {
            return false;
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (std::string(registration->features[previous].feature_id) == feature.feature_id) {
                return false;
            }
        }
    }
    return true;
}

ktag_feature_provider_storage copy_registration(
    const ktag_feature_provider_registration &registration)
{
    ktag_feature_provider_storage provider;
    provider.provider_id = registration.provider_id;
    provider.provider_version = registration.provider_version;
    provider.callbacks = registration.callbacks;
    provider.callback_error_policy = registration.callback_error_policy;
    provider.user_data = registration.user_data;
    provider.features.reserve(registration.feature_count);
    for (uint32_t index = 0; index < registration.feature_count; ++index) {
        const auto &source = registration.features[index];
        ktag_feature_descriptor_storage feature;
        feature.feature_id = source.feature_id;
        feature.display_name = source.display_name;
        feature.provider_id = source.provider_id;
        feature.provider_version = source.provider_version;
        feature.value_type = source.value_type;
        feature.dimension = source.dimension;
        provider.features.push_back(std::move(feature));
    }
    return provider;
}

} // namespace

const ktag_feature_provider_storage *ktag_feature_registry_find_provider_internal(
    const ktag_feature_registry *registry,
    const std::string &provider_id)
{
    if (registry == nullptr) {
        return nullptr;
    }
    const auto provider = find_provider(registry, provider_id.c_str());
    return provider == registry->providers.cend() ? nullptr : &*provider;
}

const ktag_feature_descriptor_storage *ktag_feature_registry_find_feature_internal(
    const ktag_feature_registry *registry,
    const std::string &feature_id,
    const ktag_feature_provider_storage **provider)
{
    if (provider != nullptr) {
        *provider = nullptr;
    }
    if (registry == nullptr) {
        return nullptr;
    }
    for (const auto &candidate : registry->providers) {
        const auto feature = std::find_if(candidate.features.cbegin(), candidate.features.cend(),
                                          [&feature_id](const ktag_feature_descriptor_storage &item) {
                                              return item.feature_id == feature_id;
                                          });
        if (feature != candidate.features.cend()) {
            if (provider != nullptr) {
                *provider = &candidate;
            }
            return &*feature;
        }
    }
    return nullptr;
}

extern "C" {

KTAG_API ktag_feature_registry *ktag_feature_registry_create(void)
{
    return new (std::nothrow) ktag_feature_registry();
}

KTAG_API void ktag_feature_registry_destroy(ktag_feature_registry *registry)
{
    delete registry;
}

KTAG_API ktag_status ktag_feature_registry_register(
    ktag_feature_registry *registry,
    const ktag_feature_provider_registration *registration,
    uint32_t flags)
{
    if (registry == nullptr || !registration_is_valid(registration) ||
        (flags & ~KTAG_FEATURE_REGISTER_REPLACE) != 0) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }

    auto existing = find_provider(registry, registration->provider_id);
    if (existing != registry->providers.end() && (flags & KTAG_FEATURE_REGISTER_REPLACE) == 0) {
        return KTAG_ERROR_ALREADY_EXISTS;
    }

    for (uint32_t index = 0; index < registration->feature_count; ++index) {
        const std::string feature_id = registration->features[index].feature_id;
        const ktag_feature_provider_storage *owner = nullptr;
        if (ktag_feature_registry_find_feature_internal(registry, feature_id, &owner) != nullptr &&
            (owner == nullptr || owner->provider_id != registration->provider_id)) {
            return KTAG_ERROR_ALREADY_EXISTS;
        }
    }

    try {
        auto replacement = copy_registration(*registration);
        if (existing == registry->providers.end()) {
            registry->providers.push_back(std::move(replacement));
        } else {
            *existing = std::move(replacement);
        }
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API ktag_status ktag_feature_registry_unregister(
    ktag_feature_registry *registry,
    const char *provider_id)
{
    if (registry == nullptr || !valid_identifier(provider_id, KTAG_PROVIDER_ID_MAX_LENGTH)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const auto provider = find_provider(registry, provider_id);
    if (provider == registry->providers.end()) {
        return KTAG_ERROR_NOT_FOUND;
    }
    registry->providers.erase(provider);
    return KTAG_SUCCESS;
}

KTAG_API uint32_t ktag_feature_registry_get_provider_count(const ktag_feature_registry *registry)
{
    return registry == nullptr ? 0U : static_cast<uint32_t>(registry->providers.size());
}

KTAG_API ktag_status ktag_feature_schema_add_registered(
    ktag_feature_schema *schema,
    const ktag_feature_registry *registry,
    const char *feature_id)
{
    if (schema == nullptr || registry == nullptr ||
        !valid_identifier(feature_id, KTAG_FEATURE_ID_MAX_LENGTH)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const auto *feature = ktag_feature_registry_find_feature_internal(registry, feature_id, nullptr);
    if (feature == nullptr) {
        return KTAG_ERROR_NOT_FOUND;
    }
    ktag_feature_descriptor descriptor{};
    ktag_feature_copy_descriptor(*feature, &descriptor);
    return ktag_feature_schema_add(schema, &descriptor);
}

} // extern "C"
