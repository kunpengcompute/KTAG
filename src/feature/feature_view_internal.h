/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_VIEW_INTERNAL_H
#define KTAG_FEATURE_VIEW_INTERNAL_H

#include "flow_hash.h"
#include "ktag_feature.h"

struct ktag_flow_view {
  const flow_hash_node *node = nullptr;
  uint64_t flow_id = 0;
  ktag_flow_key flow_key{};
  ktag_ip_address forward_address{};
  uint16_t forward_port = 0U;
  bool has_flow_key = false;
  bool forward_is_node_source = true;
};

struct ktag_packet_view {
  const uint8_t *data = nullptr;
  uint32_t length = 0;
  const uint8_t *payload = nullptr;
  uint32_t payload_length = 0U;
  ktag_timestamp timestamp{};
  ktag_packet_transport_view transport{};
};

const flow_raw_data *ktag_flow_view_get_direction_data_internal(
    const ktag_flow_view *flow,
    ktag_direction direction);

#endif /* KTAG_FEATURE_VIEW_INTERNAL_H */
