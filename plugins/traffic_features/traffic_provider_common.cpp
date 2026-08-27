/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "traffic_provider_common.h"

#include <new>
#include <vector>

namespace ktag_traffic_features {

ktag_status register_provider(ktag_feature_registry *registry,
                              const provider_catalog &catalog,
                              const ktag_feature_callbacks &callbacks,
                              ktag_feature_callback_error_policy error_policy,
                              void *user_data) {
    if (registry == nullptr || catalog.features == nullptr ||
        catalog.feature_count == 0U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::vector<ktag_feature_descriptor> descriptors(catalog.feature_count);
        for (size_t index = 0U; index < catalog.feature_count; ++index) {
            auto &descriptor = descriptors[index];
            descriptor.struct_size = sizeof(descriptor);
            descriptor.api_version = KTAG_API_VERSION;
            descriptor.feature_id = catalog.features[index].feature_id;
            descriptor.display_name = catalog.features[index].display_name;
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
        registration.feature_count = static_cast<uint32_t>(descriptors.size());
        registration.callbacks = callbacks;
        registration.callback_error_policy = error_policy;
        registration.user_data = user_data;
        registration.contract_provider_id = catalog.contract_provider_id;
        return ktag_feature_registry_register(registry, &registration,
                                              KTAG_FEATURE_REGISTER_DEFAULT);
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

ktag_status set_optional(ktag_feature_output *output,
                         const feature_definition &feature, const float *values,
                         uint32_t value_count) {
    const ktag_status status = ktag_feature_output_set(
        output, feature.feature_id, values, value_count);
    return status == KTAG_ERROR_NOT_FOUND ? KTAG_SUCCESS : status;
}

ktag_status set_scalar(ktag_feature_output *output,
                       const feature_definition &feature, double value) {
    const float converted = static_cast<float>(value);
    return set_optional(output, feature, &converted, 1U);
}

} // namespace ktag_traffic_features
