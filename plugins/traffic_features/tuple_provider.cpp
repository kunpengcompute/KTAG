/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "tuple_provider.h"

#include <arpa/inet.h>

#include "traffic_feature_catalog.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

enum tuple_feature_index : size_t {
  SOURCE_IP = 0U,
  DESTINATION_IP,
  SOURCE_PORT,
  DESTINATION_PORT,
  PROTOCOL,
};

ktag_status tuple_finish(
    void *,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output) {
  if (flow == nullptr || output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  uint32_t source_ip = 0U;
  uint32_t destination_ip = 0U;
  uint16_t source_port = 0U;
  uint16_t destination_port = 0U;
  uint8_t protocol = 0U;
  ktag_status tuple_status = ktag_flow_view_get_legacy_tuple(
      flow, &source_ip, &source_port, &destination_ip, &destination_port,
      &protocol);
  if (tuple_status == KTAG_ERROR_NOT_SUPPORTED) {
    /* Legacy V1 encoded IPv6 addresses as zero and retained the canonical
     * transport ports in network byte order. Reconstruct that frozen value
     * contract from the public, address-family-neutral flow key. */
    ktag_flow_key key{};
    tuple_status = ktag_flow_view_get_flow_key(flow, &key);
    if (tuple_status == KTAG_SUCCESS) {
      source_port = htons(key.port_a);
      destination_port = htons(key.port_b);
      protocol = key.protocol;
    }
  }
  if (tuple_status != KTAG_SUCCESS) {
    return tuple_status;
  }

  if (set_scalar(output, KTAG_TUPLE_FEATURES[SOURCE_IP], source_ip) !=
          KTAG_SUCCESS ||
      set_scalar(output, KTAG_TUPLE_FEATURES[DESTINATION_IP], destination_ip) !=
          KTAG_SUCCESS ||
      set_scalar(output, KTAG_TUPLE_FEATURES[SOURCE_PORT], source_port) !=
          KTAG_SUCCESS ||
      set_scalar(output, KTAG_TUPLE_FEATURES[DESTINATION_PORT],
                 destination_port) != KTAG_SUCCESS ||
      set_scalar(output, KTAG_TUPLE_FEATURES[PROTOCOL], protocol) !=
          KTAG_SUCCESS) {
    return KTAG_ERROR_INTERNAL;
  }
  return KTAG_SUCCESS;
}

} // namespace

ktag_status register_tuple_provider(ktag_feature_registry *registry) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.flow_finish = tuple_finish;
  return register_provider(registry, KTAG_TUPLE_CATALOG, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace ktag_traffic_features
