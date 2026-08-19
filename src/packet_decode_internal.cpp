/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "packet_decode_internal.h"

#include <cstring>

namespace {

constexpr uint16_t ETHER_TYPE_IPV4 = 0x0800U;
constexpr uint16_t ETHER_TYPE_IPV6 = 0x86ddU;
constexpr uint16_t ETHER_TYPE_VLAN = 0x8100U;
constexpr uint16_t ETHER_TYPE_QINQ = 0x88a8U;
constexpr uint16_t ETHER_TYPE_VLAN_9100 = 0x9100U;
constexpr uint8_t PROTOCOL_HOP_BY_HOP = 0U;
constexpr uint8_t PROTOCOL_TCP = 6U;
constexpr uint8_t PROTOCOL_UDP = 17U;
constexpr uint8_t PROTOCOL_ROUTING = 43U;
constexpr uint8_t PROTOCOL_FRAGMENT = 44U;
constexpr uint8_t PROTOCOL_AH = 51U;
constexpr uint8_t PROTOCOL_DESTINATION = 60U;
constexpr uint32_t MAX_VLAN_HEADERS = 4U;
constexpr uint32_t MAX_IPV6_EXTENSION_HEADERS = 8U;

uint16_t read_be16(const uint8_t *data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) |
                               static_cast<uint16_t>(data[1]));
}

uint32_t read_be32(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24U) |
         (static_cast<uint32_t>(data[1]) << 16U) |
         (static_cast<uint32_t>(data[2]) << 8U) |
         static_cast<uint32_t>(data[3]);
}

bool is_vlan(uint16_t ether_type) {
  return ether_type == ETHER_TYPE_VLAN || ether_type == ETHER_TYPE_QINQ ||
         ether_type == ETHER_TYPE_VLAN_9100;
}

bool is_ipv6_extension(uint8_t protocol) {
  return protocol == PROTOCOL_HOP_BY_HOP || protocol == PROTOCOL_ROUTING ||
         protocol == PROTOCOL_FRAGMENT || protocol == PROTOCOL_AH ||
         protocol == PROTOCOL_DESTINATION;
}

void store_address(ktag_ip_address *destination, const uint8_t *source,
                   uint8_t version) {
  std::memset(destination, 0, sizeof(*destination));
  const uint32_t length = version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
  std::memcpy(destination->bytes, source, length);
  destination->version = version;
}

int compare_endpoint(const ktag_ip_address &first_address, uint16_t first_port,
                     const ktag_ip_address &second_address,
                     uint16_t second_port) {
  const uint32_t address_length =
      first_address.version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
  const int address_order =
      std::memcmp(first_address.bytes, second_address.bytes, address_length);
  if (address_order != 0) {
    return address_order;
  }
  if (first_port < second_port) {
    return -1;
  }
  if (first_port > second_port) {
    return 1;
  }
  return 0;
}

void normalize_key(ktag_decoded_transport_packet *decoded) {
  ktag_flow_key &key = decoded->flow_key;
  key = {};
  key.struct_size = sizeof(key);
  key.api_version = KTAG_API_VERSION;
  key.protocol = decoded->transport_protocol;
  if (compare_endpoint(decoded->source_address, decoded->source_port,
                       decoded->destination_address,
                       decoded->destination_port) <= 0) {
    key.address_a = decoded->source_address;
    key.port_a = decoded->source_port;
    key.address_b = decoded->destination_address;
    key.port_b = decoded->destination_port;
  } else {
    key.address_a = decoded->destination_address;
    key.port_a = decoded->destination_port;
    key.address_b = decoded->source_address;
    key.port_b = decoded->source_port;
  }
}

ktag_status locate_network_packet(const ktag_packet &packet,
                                  ktag_decoded_transport_packet *decoded,
                                  uint16_t *ether_type) {
  const uint8_t *data = packet.data;
  const uint32_t length = packet.captured_length;
  uint32_t offset = 0U;
  uint16_t type = 0U;

  switch (packet.link_type) {
  case KTAG_CAPTURE_LINKTYPE_ETHERNET:
    if (length < 14U) {
      return KTAG_ERROR_INVALID_ARGUMENT;
    }
    offset = 14U;
    type = read_be16(data + 12U);
    for (uint32_t count = 0U; count < MAX_VLAN_HEADERS && is_vlan(type);
         ++count) {
      if (length - offset < 4U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
      }
      type = read_be16(data + offset + 2U);
      offset += 4U;
    }
    if (is_vlan(type)) {
      return KTAG_ERROR_NOT_SUPPORTED;
    }
    break;
  case KTAG_CAPTURE_LINKTYPE_LINUX_SLL:
    if (length < 16U) {
      return KTAG_ERROR_INVALID_ARGUMENT;
    }
    type = read_be16(data + 14U);
    offset = 16U;
    break;
  case KTAG_CAPTURE_LINKTYPE_LINUX_SLL2:
    if (length < 20U) {
      return KTAG_ERROR_INVALID_ARGUMENT;
    }
    type = read_be16(data);
    offset = 20U;
    break;
  case KTAG_CAPTURE_LINKTYPE_RAW:
    if (length == 0U) {
      return KTAG_ERROR_INVALID_ARGUMENT;
    }
    type = (data[0] >> 4U) == 4U   ? ETHER_TYPE_IPV4
           : (data[0] >> 4U) == 6U ? ETHER_TYPE_IPV6
                                   : 0U;
    break;
  default:
    return KTAG_ERROR_NOT_SUPPORTED;
  }

  if (offset > length) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (type != ETHER_TYPE_IPV4 && type != ETHER_TYPE_IPV6) {
    return KTAG_ERROR_NOT_SUPPORTED;
  }
  decoded->link_header_length = offset;
  decoded->network = data + offset;
  *ether_type = type;
  return KTAG_SUCCESS;
}

ktag_status decode_ipv4(uint32_t available,
                        ktag_decoded_transport_packet *decoded) {
  const uint8_t *data = decoded->network;
  if (available < 20U || (data[0] >> 4U) != 4U) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  const uint32_t header_length = static_cast<uint32_t>(data[0] & 0x0fU) * 4U;
  const uint32_t total_length = read_be16(data + 2U);
  if (header_length < 20U || header_length > available ||
      total_length < header_length || total_length > available) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  decoded->network_version = 4U;
  decoded->network_packet_length = total_length;
  decoded->network_header_length = header_length;
  decoded->transport_protocol = data[9U];
  store_address(&decoded->source_address, data + 12U, 4U);
  store_address(&decoded->destination_address, data + 16U, 4U);

  const uint16_t fragment = read_be16(data + 6U);
  const uint16_t fragment_offset = fragment & 0x1fffU;
  const bool more_fragments = (fragment & 0x2000U) != 0U;
  if (fragment_offset != 0U) {
    decoded->fragment_state = ktag_internal_fragment_state::non_initial;
    return KTAG_ERROR_NOT_FOUND;
  }
  if (more_fragments) {
    decoded->fragment_state = ktag_internal_fragment_state::first;
  }
  return KTAG_SUCCESS;
}

ktag_status decode_ipv6(uint32_t available,
                        ktag_decoded_transport_packet *decoded) {
  const uint8_t *data = decoded->network;
  if (available < 40U || (data[0] >> 4U) != 6U) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  const uint32_t payload_length = read_be16(data + 4U);
  if (payload_length == 0U) {
    return KTAG_ERROR_NOT_SUPPORTED;
  }
  const uint32_t packet_length = 40U + payload_length;
  if (packet_length > available) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  decoded->network_version = 6U;
  decoded->network_packet_length = packet_length;
  store_address(&decoded->source_address, data + 8U, 6U);
  store_address(&decoded->destination_address, data + 24U, 6U);

  uint8_t next_header = data[6U];
  uint32_t offset = 40U;
  while (is_ipv6_extension(next_header)) {
    if (decoded->ipv6_extension_header_count >= MAX_IPV6_EXTENSION_HEADERS) {
      return KTAG_ERROR_NOT_SUPPORTED;
    }
    if (offset > packet_length || packet_length - offset < 2U) {
      return KTAG_ERROR_INVALID_ARGUMENT;
    }

    uint32_t extension_length = 0U;
    if (next_header == PROTOCOL_FRAGMENT) {
      extension_length = 8U;
      if (packet_length - offset < extension_length) {
        return KTAG_ERROR_INVALID_ARGUMENT;
      }
      const uint16_t fragment = read_be16(data + offset + 2U);
      const uint16_t fragment_offset = fragment & 0xfff8U;
      const bool more_fragments = (fragment & 0x0001U) != 0U;
      if (fragment_offset != 0U) {
        decoded->fragment_state = ktag_internal_fragment_state::non_initial;
        return KTAG_ERROR_NOT_FOUND;
      }
      decoded->fragment_state = more_fragments
                                    ? ktag_internal_fragment_state::first
                                    : ktag_internal_fragment_state::atomic;
    } else if (next_header == PROTOCOL_AH) {
      extension_length = (static_cast<uint32_t>(data[offset + 1U]) + 2U) * 4U;
    } else {
      extension_length = (static_cast<uint32_t>(data[offset + 1U]) + 1U) * 8U;
    }
    if (extension_length < 8U || extension_length > packet_length - offset) {
      return KTAG_ERROR_INVALID_ARGUMENT;
    }
    next_header = data[offset];
    offset += extension_length;
    ++decoded->ipv6_extension_header_count;
    decoded->ipv6_extension_header_bytes += extension_length;
  }

  decoded->transport_protocol = next_header;
  decoded->network_header_length = offset;
  return KTAG_SUCCESS;
}

ktag_status decode_transport(ktag_decoded_transport_packet *decoded) {
  if (decoded->transport_protocol != PROTOCOL_TCP &&
      decoded->transport_protocol != PROTOCOL_UDP) {
    return KTAG_ERROR_NOT_SUPPORTED;
  }
  if (decoded->network_header_length > decoded->network_packet_length) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  decoded->transport = decoded->network + decoded->network_header_length;
  decoded->transport_packet_length =
      decoded->network_packet_length - decoded->network_header_length;
  if (decoded->transport_packet_length < 4U) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  decoded->source_port = read_be16(decoded->transport);
  decoded->destination_port = read_be16(decoded->transport + 2U);
  normalize_key(decoded);

  if (decoded->transport_protocol == PROTOCOL_TCP) {
    if (decoded->transport_packet_length < 20U) {
      return KTAG_SUCCESS;
    }
    const uint32_t header_length =
        static_cast<uint32_t>(decoded->transport[12U] >> 4U) * 4U;
    if (header_length < 20U ||
        header_length > decoded->transport_packet_length) {
      return KTAG_SUCCESS;
    }
    decoded->transport_header_length = header_length;
    decoded->tcp_flags = decoded->transport[13U];
    decoded->tcp_sequence = read_be32(decoded->transport + 4U);
    decoded->tcp_acknowledgment = read_be32(decoded->transport + 8U);
  } else {
    if (decoded->transport_packet_length < 8U) {
      return KTAG_SUCCESS;
    }
    const uint32_t udp_length = read_be16(decoded->transport + 4U);
    if (udp_length < 8U) {
      return KTAG_SUCCESS;
    }
    if (udp_length <= decoded->transport_packet_length) {
      decoded->transport_packet_length = udp_length;
    } else if (decoded->fragment_state != ktag_internal_fragment_state::first) {
      return KTAG_SUCCESS;
    }
    decoded->transport_header_length = 8U;
  }

  decoded->payload = decoded->transport + decoded->transport_header_length;
  decoded->payload_length =
      decoded->transport_packet_length - decoded->transport_header_length;
  decoded->transport_header_complete = true;
  return KTAG_SUCCESS;
}

} // namespace

ktag_status
ktag_decode_transport_packet_internal(const ktag_packet *packet,
                                      ktag_decoded_transport_packet *decoded) {
  if (packet == nullptr || decoded == nullptr ||
      packet->struct_size < sizeof(*packet) ||
      packet->api_version != KTAG_API_VERSION ||
      (packet->data == nullptr && packet->captured_length != 0U)) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *decoded = {};
  uint16_t ether_type = 0U;
  ktag_status status = locate_network_packet(*packet, decoded, &ether_type);
  if (status != KTAG_SUCCESS) {
    return status;
  }
  const uint32_t available =
      packet->captured_length - decoded->link_header_length;
  status = ether_type == ETHER_TYPE_IPV4 ? decode_ipv4(available, decoded)
                                         : decode_ipv6(available, decoded);
  if (status != KTAG_SUCCESS) {
    return status;
  }
  return decode_transport(decoded);
}
