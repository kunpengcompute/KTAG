/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_capture.h"

#include <cstring>

namespace {

constexpr uint16_t ETHER_TYPE_IPV4 = 0x0800U;
constexpr uint16_t ETHER_TYPE_IPV6 = 0x86ddU;
constexpr uint16_t ETHER_TYPE_VLAN = 0x8100U;
constexpr uint16_t ETHER_TYPE_QINQ = 0x88a8U;
constexpr uint16_t ETHER_TYPE_VLAN_9100 = 0x9100U;
constexpr uint8_t PROTOCOL_TCP = 6U;
constexpr uint8_t PROTOCOL_UDP = 17U;
constexpr uint8_t IPV6_HOP_BY_HOP = 0U;
constexpr uint8_t IPV6_ROUTING = 43U;
constexpr uint8_t IPV6_FRAGMENT = 44U;
constexpr uint8_t IPV6_AH = 51U;
constexpr uint8_t IPV6_DESTINATION = 60U;
constexpr uint32_t MAX_IPV6_EXTENSION_HEADERS = 8U;
constexpr uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV1A_PRIME = 1099511628211ULL;

uint16_t read_be16(const uint8_t *data)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8U) |
        static_cast<uint16_t>(data[1]));
}

int compare_endpoint(
    const uint8_t *first_address,
    uint16_t first_port,
    const uint8_t *second_address,
    uint16_t second_port,
    uint32_t address_length)
{
    const int address_order = std::memcmp(
        first_address, second_address, address_length);
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

void store_address(
    ktag_ip_address *destination,
    const uint8_t *source,
    uint8_t version)
{
    std::memset(destination, 0, sizeof(*destination));
    const uint32_t length = version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
    std::memcpy(destination->bytes, source, length);
    destination->version = version;
}

void normalize_key(
    ktag_flow_key *key,
    const uint8_t *source_address,
    uint16_t source_port,
    const uint8_t *destination_address,
    uint16_t destination_port,
    uint8_t protocol,
    uint8_t version)
{
    *key = {};
    key->struct_size = sizeof(*key);
    key->api_version = KTAG_API_VERSION;
    key->protocol = protocol;
    const uint32_t address_length = version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
    if (compare_endpoint(
            source_address, source_port,
            destination_address, destination_port,
            address_length) <= 0) {
        store_address(&key->address_a, source_address, version);
        store_address(&key->address_b, destination_address, version);
        key->port_a = source_port;
        key->port_b = destination_port;
    } else {
        store_address(&key->address_a, destination_address, version);
        store_address(&key->address_b, source_address, version);
        key->port_a = destination_port;
        key->port_b = source_port;
    }
}

ktag_status parse_ipv4(
    const uint8_t *data,
    uint32_t length,
    ktag_flow_key *key)
{
    if (length < 20U || (data[0] >> 4U) != 4U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t header_length = static_cast<uint32_t>(data[0] & 0x0fU) * 4U;
    if (header_length < 20U || header_length > length) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t total_length = read_be16(data + 2U);
    if (total_length < header_length + 4U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (total_length < length) {
        length = total_length;
    }
    const uint16_t fragment = read_be16(data + 6U);
    if ((fragment & 0x1fffU) != 0U) {
        return KTAG_ERROR_NOT_FOUND;
    }
    const uint8_t protocol = data[9];
    if (protocol != PROTOCOL_TCP && protocol != PROTOCOL_UDP) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }
    if (length - header_length < 4U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }

    const uint8_t *transport = data + header_length;
    normalize_key(
        key,
        data + 12U, read_be16(transport),
        data + 16U, read_be16(transport + 2U),
        protocol, 4U);
    return KTAG_SUCCESS;
}

ktag_status parse_ipv6(
    const uint8_t *data,
    uint32_t length,
    ktag_flow_key *key)
{
    if (length < 40U || (data[0] >> 4U) != 6U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t payload_length = read_be16(data + 4U);
    if (payload_length != 0U && 40U + payload_length < length) {
        length = 40U + payload_length;
    }
    uint8_t next_header = data[6];
    uint32_t offset = 40U;

    for (uint32_t count = 0; count < MAX_IPV6_EXTENSION_HEADERS; ++count) {
        uint32_t extension_length = 0;
        if (next_header == IPV6_FRAGMENT) {
            if (length - offset < 8U) {
                return KTAG_ERROR_INVALID_ARGUMENT;
            }
            if ((read_be16(data + offset + 2U) & 0xfff8U) != 0U) {
                return KTAG_ERROR_NOT_FOUND;
            }
            const uint8_t following = data[offset];
            offset += 8U;
            next_header = following;
            continue;
        }
        if (next_header == IPV6_HOP_BY_HOP || next_header == IPV6_ROUTING ||
            next_header == IPV6_DESTINATION || next_header == IPV6_AH) {
            if (length - offset < 2U) {
                return KTAG_ERROR_INVALID_ARGUMENT;
            }
            extension_length = next_header == IPV6_AH ?
                (static_cast<uint32_t>(data[offset + 1U]) + 2U) * 4U :
                (static_cast<uint32_t>(data[offset + 1U]) + 1U) * 8U;
            if (extension_length < 8U || extension_length > length - offset) {
                return KTAG_ERROR_INVALID_ARGUMENT;
            }
            const uint8_t following = data[offset];
            offset += extension_length;
            next_header = following;
            continue;
        }
        break;
    }

    if (next_header != PROTOCOL_TCP && next_header != PROTOCOL_UDP) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }
    if (offset > length || length - offset < 4U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    normalize_key(
        key,
        data + 8U, read_be16(data + offset),
        data + 24U, read_be16(data + offset + 2U),
        next_header, 6U);
    return KTAG_SUCCESS;
}

ktag_status locate_network_packet(
    const ktag_packet *packet,
    const uint8_t **network,
    uint32_t *network_length,
    uint16_t *ether_type)
{
    const uint8_t *data = packet->data;
    uint32_t length = packet->captured_length;
    uint32_t offset = 0;
    uint16_t type = 0;

    switch (packet->link_type) {
        case KTAG_CAPTURE_LINKTYPE_ETHERNET:
            if (length < 14U) {
                return KTAG_ERROR_INVALID_ARGUMENT;
            }
            offset = 14U;
            type = read_be16(data + 12U);
            for (uint32_t vlan = 0;
                 vlan < 4U && (type == ETHER_TYPE_VLAN || type == ETHER_TYPE_QINQ ||
                               type == ETHER_TYPE_VLAN_9100);
                 ++vlan) {
                if (length - offset < 4U) {
                    return KTAG_ERROR_INVALID_ARGUMENT;
                }
                type = read_be16(data + offset + 2U);
                offset += 4U;
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
            type = (data[0] >> 4U) == 4U ? ETHER_TYPE_IPV4 :
                   (data[0] >> 4U) == 6U ? ETHER_TYPE_IPV6 : 0U;
            break;
        default:
            return KTAG_ERROR_NOT_SUPPORTED;
    }

    if (offset > length || (type != ETHER_TYPE_IPV4 && type != ETHER_TYPE_IPV6)) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }
    *network = data + offset;
    *network_length = length - offset;
    *ether_type = type;
    return KTAG_SUCCESS;
}

bool key_is_valid(const ktag_flow_key *key)
{
    if (key == nullptr || key->struct_size < sizeof(*key) ||
        key->api_version != KTAG_API_VERSION ||
        key->address_a.version != key->address_b.version ||
        (key->address_a.version != 4U && key->address_a.version != 6U) ||
        (key->protocol != PROTOCOL_TCP && key->protocol != PROTOCOL_UDP)) {
        return false;
    }
    const uint32_t address_length = key->address_a.version == 4U ?
        4U : KTAG_IPV6_ADDRESS_SIZE;
    if (key->address_a.version == 4U) {
        for (uint32_t index = 4U; index < KTAG_IPV6_ADDRESS_SIZE; ++index) {
            if (key->address_a.bytes[index] != 0U || key->address_b.bytes[index] != 0U) {
                return false;
            }
        }
    }
    return compare_endpoint(
        key->address_a.bytes, key->port_a,
        key->address_b.bytes, key->port_b,
        address_length) <= 0;
}

void hash_byte(uint64_t *hash, uint8_t value)
{
    *hash ^= value;
    *hash *= FNV1A_PRIME;
}

void hash_u16(uint64_t *hash, uint16_t value)
{
    hash_byte(hash, static_cast<uint8_t>(value >> 8U));
    hash_byte(hash, static_cast<uint8_t>(value & 0xffU));
}

} // namespace

extern "C" {

KTAG_API ktag_status ktag_packet_get_flow_key(
    const ktag_packet *packet,
    ktag_flow_key *key)
{
    if (packet == nullptr || key == nullptr || packet->struct_size < sizeof(*packet) ||
        packet->api_version != KTAG_API_VERSION ||
        (packet->data == nullptr && packet->captured_length != 0U)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const uint8_t *network = nullptr;
    uint32_t network_length = 0;
    uint16_t ether_type = 0;
    const ktag_status status = locate_network_packet(
        packet, &network, &network_length, &ether_type);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    return ether_type == ETHER_TYPE_IPV4 ?
        parse_ipv4(network, network_length, key) :
        parse_ipv6(network, network_length, key);
}

KTAG_API uint64_t ktag_flow_hash(
    const ktag_flow_key *key,
    uint64_t seed)
{
    if (!key_is_valid(key)) {
        return 0U;
    }
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (uint32_t shift = 0; shift < 64U; shift += 8U) {
        hash_byte(&hash, static_cast<uint8_t>((seed >> shift) & 0xffU));
    }
    hash_byte(&hash, key->address_a.version);
    hash_byte(&hash, key->protocol);
    const uint32_t address_length = key->address_a.version == 4U ?
        4U : KTAG_IPV6_ADDRESS_SIZE;
    for (uint32_t index = 0; index < address_length; ++index) {
        hash_byte(&hash, key->address_a.bytes[index]);
    }
    hash_u16(&hash, key->port_a);
    for (uint32_t index = 0; index < address_length; ++index) {
        hash_byte(&hash, key->address_b.bytes[index]);
    }
    hash_u16(&hash, key->port_b);
    return hash;
}

KTAG_API uint32_t ktag_flow_select_worker(
    const ktag_flow_key *key,
    uint32_t worker_count,
    uint64_t seed)
{
    if (worker_count == 0U || !key_is_valid(key)) {
        return KTAG_WORKER_INDEX_INVALID;
    }
    return static_cast<uint32_t>(ktag_flow_hash(key, seed) % worker_count);
}

} // extern "C"
