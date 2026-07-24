/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature.h"

#include <string>

namespace {

bool add_feature(
    ktag_feature_schema *schema,
    const std::string &feature_id,
    const std::string &display_name,
    const char *provider_id,
    uint32_t dimension = 1)
{
    ktag_feature_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    descriptor.feature_id = feature_id.c_str();
    descriptor.display_name = display_name.c_str();
    descriptor.provider_id = provider_id;
    descriptor.provider_version = 1;
    descriptor.value_type = KTAG_FEATURE_FLOAT32;
    descriptor.dimension = dimension;
    return ktag_feature_schema_add(schema, &descriptor) == KTAG_SUCCESS;
}

bool add_direction_features(ktag_feature_schema *schema, const std::string &direction)
{
    const std::string prefix = "traffic." + direction;
    const char *provider = "builtin.legacy.traffic_statistics";
    if (!add_feature(schema, prefix + ".byte_count", direction + "_byte_nums", provider) ||
        !add_feature(schema, prefix + ".packet_count", direction + "_packet_nums", provider)) {
        return false;
    }

    const char *type_ids[] = {"header", "payload", "interval"};
    const char *type_names[] = {"hdr", "pld", "interval"};
    const char *stat_ids[] = {"minimum", "maximum", "mean", "variance"};
    const char *stat_names[] = {"min", "max", "mean", "var"};

    for (uint32_t type_index = 0; type_index < 3; ++type_index) {
        const std::string type_prefix = prefix + "." + type_ids[type_index];
        const std::string name_prefix = direction + "_" + type_names[type_index];
        for (uint32_t stat_index = 0; stat_index < 4; ++stat_index) {
            if (!add_feature(schema, type_prefix + "." + stat_ids[stat_index],
                             name_prefix + "_" + stat_names[stat_index], provider)) {
                return false;
            }
        }
        if (!add_feature(schema, type_prefix + ".histogram.relative", name_prefix + "_hist_rel", provider, 16) ||
            !add_feature(schema, type_prefix + ".histogram.absolute", name_prefix + "_hist_abs", provider, 16) ||
            !add_feature(schema, type_prefix + ".histogram.normalized", name_prefix + "_hist_nor", provider, 16)) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" {

KTAG_API ktag_feature_schema *ktag_feature_schema_create_legacy_v1(void)
{
    ktag_feature_schema *schema = ktag_feature_schema_create("ktag_legacy_v1", 1);
    if (schema == nullptr) {
        return nullptr;
    }

    bool success =
        add_feature(schema, "legacy.flow_id", "flow_id", "builtin.legacy.metadata") &&
        add_feature(schema, "network.src_ip", "src_ip", "builtin.legacy.tuple") &&
        add_feature(schema, "network.dst_ip", "dst_ip", "builtin.legacy.tuple") &&
        add_feature(schema, "network.src_port", "src_port", "builtin.legacy.tuple") &&
        add_feature(schema, "network.dst_port", "dst_port", "builtin.legacy.tuple") &&
        add_feature(schema, "network.protocol", "protocol", "builtin.legacy.tuple") &&
        add_direction_features(schema, "send") &&
        add_direction_features(schema, "receive") &&
        add_feature(schema, "rawbow.crc_histogram", "raw_bow_crc_hist",
                    "builtin.legacy.rawbow", 32);

    if (!success || ktag_feature_schema_get_value_count(schema) != KTAG_LEGACY_V1_VALUE_COUNT ||
        ktag_feature_schema_freeze(schema) != KTAG_SUCCESS) {
        ktag_feature_schema_destroy(schema);
        return nullptr;
    }
    return schema;
}

} // extern "C"
