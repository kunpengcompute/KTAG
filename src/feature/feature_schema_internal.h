/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_SCHEMA_INTERNAL_H
#define KTAG_FEATURE_SCHEMA_INTERNAL_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ktag_feature.h"

struct ktag_feature_descriptor_storage {
    std::string feature_id;
    std::string display_name;
    std::string provider_id;
    /* Provider identity serialized into the model contract hash. */
    std::string contract_provider_id;
    uint32_t provider_version = 0;
    ktag_feature_value_type value_type = KTAG_FEATURE_FLOAT32;
    uint32_t dimension = 0;
    uint32_t value_offset = 0;
};

struct ktag_feature_schema {
    std::string name;
    uint32_t version = 0;
    std::vector<ktag_feature_descriptor_storage> descriptors;
    std::unordered_map<std::string_view, uint32_t> feature_indices;
    uint32_t value_count = 0;
    uint64_t schema_hash = 0;
    bool frozen = false;
};

bool ktag_feature_descriptor_is_valid(const ktag_feature_descriptor *descriptor);
ktag_status ktag_feature_schema_add_contract(
    ktag_feature_schema *schema,
    const ktag_feature_descriptor *descriptor,
    const char *contract_provider_id);
void ktag_feature_copy_descriptor(
    const ktag_feature_descriptor_storage &source,
    ktag_feature_descriptor *destination);

#endif /* KTAG_FEATURE_SCHEMA_INTERNAL_H */
