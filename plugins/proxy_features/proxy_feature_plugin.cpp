/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature_plugin.h"

#include <array>

#include "proxy_feature_catalog.h"
#include "proxy_quic_provider.h"
#include "proxy_sequence_provider.h"
#include "proxy_tcp_signature_provider.h"

namespace {

using register_provider_fn = ktag_status (*)(ktag_feature_registry *registry);

ktag_status register_proxy_providers(void *, ktag_feature_registry *registry) {
  if (registry == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  const std::array<register_provider_fn, 3U> register_provider{{
      ktag_proxy_features::register_sequence_provider,
      ktag_proxy_features::register_tcp_signature_provider,
      ktag_proxy_features::register_quic_provider,
  }};

  size_t registered_count = 0U;
  for (; registered_count < register_provider.size(); ++registered_count) {
    const ktag_status status = register_provider[registered_count](registry);
    if (status == KTAG_SUCCESS) {
      continue;
    }
    while (registered_count > 0U) {
      --registered_count;
      (void)ktag_feature_registry_unregister(
          registry,
          ktag_proxy_features::KTAG_PROXY_PROVIDER_CATALOG[registered_count]
              ->provider_id);
    }
    return status;
  }
  return KTAG_SUCCESS;
}

} // namespace

extern "C" {

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
  result.plugin_id = "ktag.proxy.features";
  result.plugin_version = "1.0.0";
  result.register_providers = register_proxy_providers;
  *descriptor = result;
  return KTAG_SUCCESS;
}

} // extern "C"
