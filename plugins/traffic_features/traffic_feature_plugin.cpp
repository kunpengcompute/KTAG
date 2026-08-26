/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature_plugin.h"
#include "ktag_traffic_features.h"

#include <array>
#include "feature/traffic_contract.h"
#include "metadata_provider.h"
#include "rawbow_provider.h"
#include "traffic_statistics_provider.h"
#include "tuple_provider.h"

namespace {

using register_provider_fn =
    ktag_status (*)(ktag_feature_registry *registry);

struct provider_entry {
  const char *provider_id;
  register_provider_fn register_provider;
};

ktag_status register_traffic_providers(void *,
                                       ktag_feature_registry *registry) {
  if (registry == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  const std::array<provider_entry, 11U> providers{{
      {ktag_traffic_features::KTAG_METADATA_PROVIDER_ID,
       ktag_traffic_features::register_metadata_provider},
      {ktag_traffic_features::KTAG_TUPLE_PROVIDER_ID,
       ktag_traffic_features::register_tuple_provider},
      {ktag_traffic_features::KTAG_SEND_COUNTERS_PROVIDER_ID,
       ktag_traffic_features::register_send_counters_provider},
      {ktag_traffic_features::KTAG_SEND_HEADER_PROVIDER_ID,
       ktag_traffic_features::register_send_header_provider},
      {ktag_traffic_features::KTAG_SEND_PAYLOAD_PROVIDER_ID,
       ktag_traffic_features::register_send_payload_provider},
      {ktag_traffic_features::KTAG_SEND_INTERVAL_PROVIDER_ID,
       ktag_traffic_features::register_send_interval_provider},
      {ktag_traffic_features::KTAG_RECEIVE_COUNTERS_PROVIDER_ID,
       ktag_traffic_features::register_receive_counters_provider},
      {ktag_traffic_features::KTAG_RECEIVE_HEADER_PROVIDER_ID,
       ktag_traffic_features::register_receive_header_provider},
      {ktag_traffic_features::KTAG_RECEIVE_PAYLOAD_PROVIDER_ID,
       ktag_traffic_features::register_receive_payload_provider},
      {ktag_traffic_features::KTAG_RECEIVE_INTERVAL_PROVIDER_ID,
       ktag_traffic_features::register_receive_interval_provider},
      {ktag_traffic_features::KTAG_RAWBOW_PROVIDER_ID,
       ktag_traffic_features::register_rawbow_provider},
  }};

  size_t registered_count = 0U;
  for (; registered_count < providers.size(); ++registered_count) {
    const ktag_status status =
        providers[registered_count].register_provider(registry);
    if (status == KTAG_SUCCESS) {
      continue;
    }
    while (registered_count > 0U) {
      --registered_count;
      (void)ktag_feature_registry_unregister(
          registry, providers[registered_count].provider_id);
    }
    return status;
  }
  return KTAG_SUCCESS;
}

} // namespace

extern "C" {

KTAG_TRAFFIC_FEATURES_API ktag_status ktag_traffic_features_register(
    ktag_feature_registry *registry) {
  return register_traffic_providers(nullptr, registry);
}

KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version, ktag_feature_plugin_descriptor *descriptor) {
  if (host_abi_version != KTAG_FEATURE_PLUGIN_ABI_VERSION ||
      descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor) ||
      descriptor->api_version != KTAG_API_VERSION) {
    return KTAG_ERROR_NOT_SUPPORTED;
  }

  ktag_feature_plugin_descriptor result{};
  result.struct_size = sizeof(result);
  result.api_version = KTAG_API_VERSION;
  result.plugin_abi_version = KTAG_FEATURE_PLUGIN_ABI_VERSION;
  result.plugin_id = "ktag.traffic.features";
  result.plugin_version = "1.0.0";
  result.register_providers = register_traffic_providers;
  *descriptor = result;
  return KTAG_SUCCESS;
}

} // extern "C"
