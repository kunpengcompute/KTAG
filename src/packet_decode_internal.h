/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PACKET_DECODE_INTERNAL_H
#define KTAG_PACKET_DECODE_INTERNAL_H

#include <cstdint>

#include "ktag_capture.h"

enum class ktag_internal_fragment_state : uint8_t {
    none = 0,
    atomic = 1,
    first = 2,
    non_initial = 3
};

struct ktag_decoded_transport_packet {
    ktag_flow_key flow_key{};
    ktag_ip_address source_address{};
    ktag_ip_address destination_address{};
    const uint8_t *network = nullptr;
    const uint8_t *transport = nullptr;
    const uint8_t *payload = nullptr;
    uint32_t link_header_length = 0U;
    uint32_t network_packet_length = 0U;
    uint32_t network_header_length = 0U;
    uint32_t transport_packet_length = 0U;
    uint32_t transport_header_length = 0U;
    uint32_t payload_length = 0U;
    uint32_t ipv6_extension_header_count = 0U;
    uint32_t ipv6_extension_header_bytes = 0U;
    uint32_t tcp_sequence = 0U;
    uint32_t tcp_acknowledgment = 0U;
    uint16_t source_port = 0U;
    uint16_t destination_port = 0U;
    uint8_t network_version = 0U;
    uint8_t transport_protocol = 0U;
    uint8_t tcp_flags = 0U;
    ktag_internal_fragment_state fragment_state =
        ktag_internal_fragment_state::none;
    bool transport_header_complete = false;
};

/*
 * Shared bounded L2/L3/L4 decoder used by Capture and Feature.
 *
 * KTAG_ERROR_NOT_FOUND means a non-initial fragment cannot form a five-tuple.
 * KTAG_ERROR_NOT_SUPPORTED means the packet is well-formed but outside the
 * supported IPv4/IPv6 TCP/UDP boundary.
 */
ktag_status ktag_decode_transport_packet_internal(
    const ktag_packet *packet,
    ktag_decoded_transport_packet *decoded);

#endif /* KTAG_PACKET_DECODE_INTERNAL_H */
