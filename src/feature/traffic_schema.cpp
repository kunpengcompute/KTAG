/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature.h"

#include "feature_schema_internal.h"
#include "feature/traffic_contract.h"

namespace {

bool add_catalog(
    ktag_feature_schema *schema,
    const ktag_traffic_features::provider_catalog &catalog) {
  for (size_t index = 0U; index < catalog.feature_count; ++index) {
    const auto &feature = catalog.features[index];
    ktag_feature_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    descriptor.feature_id = feature.feature_id;
    descriptor.display_name = feature.display_name;
    descriptor.provider_id = catalog.provider_id;
    descriptor.provider_version = catalog.provider_version;
    descriptor.value_type = KTAG_FEATURE_FLOAT32;
    descriptor.dimension = feature.dimension;
    if (ktag_feature_schema_add_contract(
            schema, &descriptor, catalog.contract_provider_id) !=
        KTAG_SUCCESS) {
      return false;
    }
  }
  return true;
}

} // namespace

extern "C" {

KTAG_API ktag_feature_schema *ktag_feature_schema_create_legacy_v1(void) {
  ktag_feature_schema *schema =
      ktag_feature_schema_create("ktag_legacy_v1", 1U);
  if (schema == nullptr) {
    return nullptr;
  }

  for (const auto *catalog :
       ktag_traffic_features::KTAG_APPLICATION_SCHEMA_CATALOG) {
    if (catalog == nullptr || !add_catalog(schema, *catalog)) {
      ktag_feature_schema_destroy(schema);
      return nullptr;
    }
  }
  if (ktag_feature_schema_get_value_count(schema) !=
          KTAG_LEGACY_V1_VALUE_COUNT ||
      ktag_feature_schema_freeze(schema) != KTAG_SUCCESS) {
    ktag_feature_schema_destroy(schema);
    return nullptr;
  }
  return schema;
}

} // extern "C"
