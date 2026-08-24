/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "feature_view_internal.h"

namespace {

const flow_raw_data *get_direction_data(const ktag_flow_view *flow,
                                        ktag_direction direction) {
  if (flow == nullptr || flow->node == nullptr) {
    return nullptr;
  }
  if (direction == KTAG_DIRECTION_FORWARD) {
    return &flow->node->raw_data;
  }
  if (direction == KTAG_DIRECTION_REVERSE) {
    return &flow->node->reverse_raw_data;
  }
  return nullptr;
}

} // namespace

const flow_raw_data *ktag_flow_view_get_direction_data_internal(
    const ktag_flow_view *flow,
    ktag_direction direction) {
  return get_direction_data(flow, direction);
}

extern "C" {

KTAG_API uint64_t ktag_flow_view_get_flow_id(const ktag_flow_view *flow) {
  return flow == nullptr ? 0U : flow->flow_id;
}

KTAG_API ktag_status ktag_flow_view_get_flow_key(const ktag_flow_view *flow,
                                                 ktag_flow_key *key) {
  if (flow == nullptr || key == nullptr || !flow->has_flow_key) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *key = flow->flow_key;
  return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_flow_view_get_legacy_tuple(
    const ktag_flow_view *flow, uint32_t *src_ip, uint16_t *src_port,
    uint32_t *dst_ip, uint16_t *dst_port, uint8_t *protocol) {
  if (flow == nullptr || flow->node == nullptr || src_ip == nullptr ||
      src_port == nullptr || dst_ip == nullptr || dst_port == nullptr ||
      protocol == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (flow->has_flow_key && flow->flow_key.address_a.version == 6U) {
    return KTAG_ERROR_NOT_SUPPORTED;
  }
  *src_ip = flow->node->tuple.src_ip;
  *src_port = flow->node->tuple.src_port;
  *dst_ip = flow->node->tuple.dst_ip;
  *dst_port = flow->node->tuple.dst_port;
  *protocol = flow->node->tuple.protocol;
  return KTAG_SUCCESS;
}

KTAG_API uint32_t ktag_flow_view_get_packet_count(const ktag_flow_view *flow,
                                                  ktag_direction direction) {
  const auto *data = get_direction_data(flow, direction);
  return data == nullptr ? 0U : data->packets_cnt;
}

KTAG_API uint64_t ktag_flow_view_get_total_bytes(const ktag_flow_view *flow,
                                                 ktag_direction direction) {
  const auto *data = get_direction_data(flow, direction);
  return data == nullptr ? 0U : data->total_size;
}

KTAG_API const char *ktag_flow_view_get_domain(const ktag_flow_view *flow,
                                               ktag_direction direction) {
  if (flow == nullptr || flow->node == nullptr) {
    return nullptr;
  }
  const bool forward_is_tuple_source =
      flow->has_flow_key ? flow->forward_is_node_source
                         : flow->node->flow_src_ip == flow->node->tuple.src_ip;
  if (direction == KTAG_DIRECTION_FORWARD) {
    return forward_is_tuple_source ? flow->node->src_ip_domain_name.c_str()
                                   : flow->node->dst_ip_domain_name.c_str();
  }
  if (direction == KTAG_DIRECTION_REVERSE) {
    return forward_is_tuple_source ? flow->node->dst_ip_domain_name.c_str()
                                   : flow->node->src_ip_domain_name.c_str();
  }
  return nullptr;
}

KTAG_API const uint8_t *
ktag_packet_view_get_data(const ktag_packet_view *packet, uint32_t *length) {
  if (packet == nullptr || length == nullptr) {
    return nullptr;
  }
  *length = packet->length;
  return packet->data;
}

KTAG_API ktag_status ktag_packet_view_get_timestamp(
    const ktag_packet_view *packet, ktag_timestamp *timestamp) {
  if (packet == nullptr || timestamp == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *timestamp = packet->timestamp;
  return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_packet_view_get_transport(
    const ktag_packet_view *packet, ktag_packet_transport_view *transport) {
  if (packet == nullptr || transport == nullptr ||
      transport->struct_size < sizeof(*transport) ||
      transport->api_version != KTAG_API_VERSION) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *transport = packet->transport;
  return KTAG_SUCCESS;
}

KTAG_API const uint8_t *
ktag_packet_view_get_payload(const ktag_packet_view *packet, uint32_t *length) {
  if (packet == nullptr || length == nullptr) {
    return nullptr;
  }
  *length = packet->payload_length;
  return packet->payload;
}

KTAG_API ktag_status ktag_packet_view_get_capture_lengths(
    const ktag_packet_view *packet, uint32_t *packet_length,
    uint32_t *header_length, uint32_t *payload_length) {
  if (packet == nullptr || packet_length == nullptr ||
      header_length == nullptr || payload_length == nullptr ||
      packet->captured_header_length > packet->length) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *packet_length = packet->length;
  *header_length = packet->captured_header_length;
  *payload_length = packet->length - packet->captured_header_length;
  return KTAG_SUCCESS;
}

} // extern "C"
