/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "metadata_provider.h"

#include "traffic_feature_catalog.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

ktag_status metadata_finish(
    void *,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output) {
  return set_scalar(
      output, KTAG_METADATA_FEATURES[0U],
      static_cast<double>(ktag_flow_view_get_flow_id(flow)));
}

} // namespace

ktag_status register_metadata_provider(ktag_feature_registry *registry) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.flow_finish = metadata_finish;
  return register_provider(registry, KTAG_METADATA_CATALOG, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace ktag_traffic_features
