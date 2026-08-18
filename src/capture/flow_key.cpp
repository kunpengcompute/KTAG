/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_capture.h"

#include <cstring>

#include "packet_decode_internal.h"

namespace {

constexpr uint8_t PROTOCOL_TCP = 6U;
constexpr uint8_t PROTOCOL_UDP = 17U;
constexpr uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV1A_PRIME = 1099511628211ULL;

int compare_endpoint(const uint8_t *first_address, uint16_t first_port,
                     const uint8_t *second_address, uint16_t second_port,
                     uint32_t address_length) {
  const int address_order =
      std::memcmp(first_address, second_address, address_length);
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

bool key_is_valid(const ktag_flow_key *key) {
  if (key == nullptr || key->struct_size < sizeof(*key) ||
      key->api_version != KTAG_API_VERSION ||
      key->address_a.version != key->address_b.version ||
      (key->address_a.version != 4U && key->address_a.version != 6U) ||
      (key->protocol != PROTOCOL_TCP && key->protocol != PROTOCOL_UDP)) {
    return false;
  }
  const uint32_t address_length =
      key->address_a.version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
  if (key->address_a.version == 4U) {
    for (uint32_t index = 4U; index < KTAG_IPV6_ADDRESS_SIZE; ++index) {
      if (key->address_a.bytes[index] != 0U ||
          key->address_b.bytes[index] != 0U) {
        return false;
      }
    }
  }
  return compare_endpoint(key->address_a.bytes, key->port_a,
                          key->address_b.bytes, key->port_b,
                          address_length) <= 0;
}

void hash_byte(uint64_t *hash, uint8_t value) {
  *hash ^= value;
  *hash *= FNV1A_PRIME;
}

void hash_u16(uint64_t *hash, uint16_t value) {
  hash_byte(hash, static_cast<uint8_t>(value >> 8U));
  hash_byte(hash, static_cast<uint8_t>(value & 0xffU));
}

} // namespace

extern "C" {

KTAG_API ktag_status ktag_packet_get_flow_key(const ktag_packet *packet,
                                              ktag_flow_key *key) {
  if (key == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  ktag_decoded_transport_packet decoded{};
  const ktag_status status =
      ktag_decode_transport_packet_internal(packet, &decoded);
  if (status == KTAG_SUCCESS) {
    *key = decoded.flow_key;
  }
  return status;
}

KTAG_API uint64_t ktag_flow_hash(const ktag_flow_key *key, uint64_t seed) {
  if (!key_is_valid(key)) {
    return 0U;
  }
  uint64_t hash = FNV1A_OFFSET_BASIS;
  for (uint32_t shift = 0U; shift < 64U; shift += 8U) {
    hash_byte(&hash, static_cast<uint8_t>((seed >> shift) & 0xffU));
  }
  hash_byte(&hash, key->address_a.version);
  hash_byte(&hash, key->protocol);
  const uint32_t address_length =
      key->address_a.version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
  for (uint32_t index = 0U; index < address_length; ++index) {
    hash_byte(&hash, key->address_a.bytes[index]);
  }
  hash_u16(&hash, key->port_a);
  for (uint32_t index = 0U; index < address_length; ++index) {
    hash_byte(&hash, key->address_b.bytes[index]);
  }
  hash_u16(&hash, key->port_b);
  return hash;
}

KTAG_API uint32_t ktag_flow_select_worker(const ktag_flow_key *key,
                                          uint32_t worker_count,
                                          uint64_t seed) {
  if (worker_count == 0U || !key_is_valid(key)) {
    return KTAG_WORKER_INDEX_INVALID;
  }
  return static_cast<uint32_t>(ktag_flow_hash(key, seed) % worker_count);
}

} // extern "C"
