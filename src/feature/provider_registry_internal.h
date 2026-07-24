/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROVIDER_REGISTRY_INTERNAL_H
#define KTAG_PROVIDER_REGISTRY_INTERNAL_H

#include <string>
#include <vector>

#include "feature_schema_internal.h"

struct ktag_feature_provider_storage {
    std::string provider_id;
    uint32_t provider_version = 0;
    std::vector<ktag_feature_descriptor_storage> features;
    ktag_feature_callbacks callbacks{};
    ktag_feature_callback_error_policy callback_error_policy =
        KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW;
    void *user_data = nullptr;
};

struct ktag_feature_registry {
    std::vector<ktag_feature_provider_storage> providers;
};

const ktag_feature_provider_storage *ktag_feature_registry_find_provider_internal(
    const ktag_feature_registry *registry,
    const std::string &provider_id);

const ktag_feature_descriptor_storage *ktag_feature_registry_find_feature_internal(
    const ktag_feature_registry *registry,
    const std::string &feature_id,
    const ktag_feature_provider_storage **provider);

#endif /* KTAG_PROVIDER_REGISTRY_INTERNAL_H */
