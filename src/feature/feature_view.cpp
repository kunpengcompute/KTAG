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

KTAG_API ktag_status ktag_flow_view_get_length_samples(
    const ktag_flow_view *flow, ktag_direction direction,
    const uint32_t **header_lengths, const uint32_t **payload_lengths,
    uint32_t *sample_count) {
  const auto *data = get_direction_data(flow, direction);
  if (data == nullptr || header_lengths == nullptr ||
      payload_lengths == nullptr || sample_count == nullptr ||
      data->header_len.size() < data->packets_cnt ||
      data->payload_len.size() < data->packets_cnt) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *header_lengths = data->header_len.data();
  *payload_lengths = data->payload_len.data();
  *sample_count = data->packets_cnt;
  return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_flow_view_get_packet_sample(
    const ktag_flow_view *flow, ktag_direction direction, uint32_t index,
    ktag_flow_packet_sample *sample) {
  const auto *data = get_direction_data(flow, direction);
  if (data == nullptr || sample == nullptr || index >= data->packets_cnt ||
      index >= data->header_len.size() || index >= data->payload_len.size() ||
      index >= data->ts.size()) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  sample->header_length = data->header_len[index];
  sample->payload_length = data->payload_len[index];
  sample->timestamp.seconds = data->ts[index].tv_sec;
  sample->timestamp.microseconds =
      static_cast<int32_t>(data->ts[index].tv_usec);
  return KTAG_SUCCESS;
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

KTAG_API uint32_t ktag_flow_view_get_rawbow_count(const ktag_flow_view *flow) {
  if (flow == nullptr || flow->node == nullptr) {
    return 0U;
  }
  return flow->node->rawbow_cnt > MAX_RAWBOW_CNT ? MAX_RAWBOW_CNT
                                                 : flow->node->rawbow_cnt;
}

KTAG_API const char *ktag_flow_view_get_rawbow_token(const ktag_flow_view *flow,
                                                     uint32_t index) {
  if (flow == nullptr || flow->node == nullptr || index >= MAX_RAWBOW_CNT ||
      index >= flow->node->rawbow_cnt) {
    return nullptr;
  }
  return flow->node->rawbow[index].c_str();
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

} // extern "C"
