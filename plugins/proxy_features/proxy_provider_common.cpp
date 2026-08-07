/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_provider_common.h"

#include <array>
#include <new>
#include <vector>

namespace ktag_proxy_features {
namespace {

constexpr size_t MAX_FEATURE_DIMENSION = 32U;

ktag_status zero_provider_create(
    const ktag_feature_provider_config *config,
    void **provider_state)
{
    if (config == nullptr || provider_state == nullptr ||
        config->struct_size < sizeof(*config) ||
        config->api_version != KTAG_API_VERSION ||
        config->user_data == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    *provider_state = config->user_data;
    return KTAG_SUCCESS;
}

ktag_status zero_flow_finish(
    void *provider_state,
    void *,
    const ktag_flow_view *,
    ktag_feature_output *output)
{
    const auto *catalog =
        static_cast<const provider_catalog *>(provider_state);
    if (catalog == nullptr || output == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const std::array<float, MAX_FEATURE_DIMENSION> zeros{};
    for (size_t index = 0U; index < catalog->feature_count; ++index) {
        const auto &feature = catalog->features[index];
        if (feature.dimension > zeros.size()) {
            return KTAG_ERROR_INVALID_STATE;
        }
        const ktag_status status = ktag_feature_output_set(
            output,
            feature.feature_id,
            zeros.data(),
            feature.dimension);
        if (status != KTAG_SUCCESS &&
            status != KTAG_ERROR_NOT_FOUND) {
            return status;
        }
    }
    return KTAG_SUCCESS;
}

} // namespace

ktag_status register_provider(
    ktag_feature_registry *registry,
    const provider_catalog &catalog,
    const ktag_feature_callbacks &callbacks,
    ktag_feature_callback_error_policy error_policy,
    void *user_data)
{
    if (registry == nullptr || catalog.features == nullptr ||
        catalog.feature_count == 0U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::vector<ktag_feature_descriptor> descriptors(
            catalog.feature_count);
        for (size_t index = 0U; index < catalog.feature_count; ++index) {
            auto &descriptor = descriptors[index];
            descriptor.struct_size = sizeof(descriptor);
            descriptor.api_version = KTAG_API_VERSION;
            descriptor.feature_id = catalog.features[index].feature_id;
            descriptor.display_name = catalog.features[index].feature_id;
            descriptor.provider_id = catalog.provider_id;
            descriptor.provider_version = catalog.provider_version;
            descriptor.value_type = KTAG_FEATURE_FLOAT32;
            descriptor.dimension = catalog.features[index].dimension;
        }

        ktag_feature_provider_registration registration{};
        registration.struct_size = sizeof(registration);
        registration.api_version = KTAG_API_VERSION;
        registration.provider_id = catalog.provider_id;
        registration.provider_version = catalog.provider_version;
        registration.features = descriptors.data();
        registration.feature_count =
            static_cast<uint32_t>(descriptors.size());
        registration.callbacks = callbacks;
        registration.callback_error_policy = error_policy;
        registration.user_data = user_data;
        return ktag_feature_registry_register(
            registry,
            &registration,
            KTAG_FEATURE_REGISTER_DEFAULT);
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

ktag_status register_zero_provider(
    ktag_feature_registry *registry,
    const provider_catalog &catalog)
{
    ktag_feature_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.api_version = KTAG_API_VERSION;
    callbacks.provider_create = zero_provider_create;
    callbacks.flow_finish = zero_flow_finish;
    return register_provider(
        registry,
        catalog,
        callbacks,
        KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW,
        const_cast<provider_catalog *>(&catalog));
}

} // namespace ktag_proxy_features
