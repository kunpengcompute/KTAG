/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "feature_view_internal.h"

#include <cstring>

namespace {

const ktag_flow_direction_summary *
get_direction_summary(const ktag_flow_view *flow, ktag_direction direction) {
    if (flow == nullptr) {
        return nullptr;
    }
    if (direction == KTAG_DIRECTION_FORWARD) {
        return &flow->directions[0U];
    }
    if (direction == KTAG_DIRECTION_REVERSE) {
        return &flow->directions[1U];
    }
    return nullptr;
}

} // namespace

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

KTAG_API ktag_status ktag_flow_view_get_endpoint(const ktag_flow_view *flow,
                                                 ktag_direction direction,
                                                 ktag_ip_address *address,
                                                 uint16_t *port) {
    if (flow == nullptr || !flow->has_flow_key || address == nullptr ||
        port == nullptr ||
        (direction != KTAG_DIRECTION_FORWARD &&
         direction != KTAG_DIRECTION_REVERSE)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const bool forward_is_a =
        flow->forward_address.version == flow->flow_key.address_a.version &&
        std::memcmp(flow->forward_address.bytes, flow->flow_key.address_a.bytes,
                    KTAG_IPV6_ADDRESS_SIZE) == 0 &&
        flow->forward_port == flow->flow_key.port_a;
    if (direction == KTAG_DIRECTION_FORWARD) {
        *address = flow->forward_address;
        *port = flow->forward_port;
    } else if (forward_is_a) {
        *address = flow->flow_key.address_b;
        *port = flow->flow_key.port_b;
    } else {
        *address = flow->flow_key.address_a;
        *port = flow->flow_key.port_a;
    }
    return KTAG_SUCCESS;
}

KTAG_API uint32_t ktag_flow_view_get_packet_count(const ktag_flow_view *flow,
                                                  ktag_direction direction) {
    const auto *summary = get_direction_summary(flow, direction);
    return summary == nullptr ? 0U : summary->packet_count;
}

KTAG_API uint64_t ktag_flow_view_get_total_bytes(const ktag_flow_view *flow,
                                                 ktag_direction direction) {
    const auto *summary = get_direction_summary(flow, direction);
    return summary == nullptr ? 0U : summary->total_bytes;
}

KTAG_API const char *ktag_flow_view_get_domain(const ktag_flow_view *flow,
                                               ktag_direction direction) {
    const auto *summary = get_direction_summary(flow, direction);
    return summary == nullptr ? nullptr : summary->domain.c_str();
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
