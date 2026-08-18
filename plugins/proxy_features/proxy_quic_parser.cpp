/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_quic_parser.h"

#include <limits>

namespace ktag_proxy_features {
namespace {

constexpr uint8_t HEADER_FORM_BIT = 0x80U;
constexpr uint8_t FIXED_BIT = 0x40U;
constexpr uint8_t LONG_PACKET_TYPE_MASK = 0x30U;
constexpr uint8_t LONG_PACKET_TYPE_SHIFT = 4U;
constexpr uint8_t MAX_CONNECTION_ID_LENGTH = 20U;
constexpr size_t RETRY_INTEGRITY_TAG_LENGTH = 16U;

uint32_t read_be32(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0U]) << 24U) |
         (static_cast<uint32_t>(data[1U]) << 16U) |
         (static_cast<uint32_t>(data[2U]) << 8U) |
         static_cast<uint32_t>(data[3U]);
}

bool has_bytes(size_t offset, size_t required, size_t length) {
  return offset <= length && required <= length - offset;
}

bool is_known_version(uint32_t version) {
  return version == KTAG_QUIC_VERSION_1 || version == KTAG_QUIC_VERSION_2;
}

quic_header_type decode_long_header_type(uint32_t version, uint8_t first_byte) {
  const uint8_t wire_type = static_cast<uint8_t>(
      (first_byte & LONG_PACKET_TYPE_MASK) >> LONG_PACKET_TYPE_SHIFT);
  if (version == KTAG_QUIC_VERSION_1) {
    switch (wire_type) {
    case 0U:
      return quic_header_type::initial;
    case 1U:
      return quic_header_type::zero_rtt;
    case 2U:
      return quic_header_type::handshake;
    case 3U:
      return quic_header_type::retry;
    default:
      return quic_header_type::unknown;
    }
  }
  if (version == KTAG_QUIC_VERSION_2) {
    switch (wire_type) {
    case 0U:
      return quic_header_type::retry;
    case 1U:
      return quic_header_type::initial;
    case 2U:
      return quic_header_type::zero_rtt;
    case 3U:
      return quic_header_type::handshake;
    default:
      return quic_header_type::unknown;
    }
  }
  return quic_header_type::unknown;
}

quic_parse_result malformed(quic_parse_result result) {
  result.status = quic_parse_status::malformed_candidate;
  return result;
}

bool parse_connection_ids(const uint8_t *data, size_t length, size_t *offset,
                          bool enforce_v1_v2_limit, quic_parse_result *result) {
  if (data == nullptr || offset == nullptr || result == nullptr ||
      !has_bytes(*offset, 1U, length)) {
    return false;
  }
  const uint8_t dcid_length = data[(*offset)++];
  if ((enforce_v1_v2_limit && dcid_length > MAX_CONNECTION_ID_LENGTH) ||
      !has_bytes(*offset, dcid_length, length)) {
    return false;
  }
  *offset += dcid_length;

  if (!has_bytes(*offset, 1U, length)) {
    return false;
  }
  const uint8_t scid_length = data[(*offset)++];
  if ((enforce_v1_v2_limit && scid_length > MAX_CONNECTION_ID_LENGTH) ||
      !has_bytes(*offset, scid_length, length)) {
    return false;
  }
  *offset += scid_length;

  result->connection_ids_available = true;
  result->dcid_length = dcid_length;
  result->scid_length = scid_length;
  return true;
}

bool parse_packet_length(const uint8_t *data, size_t length, size_t *offset) {
  uint64_t packet_length = 0U;
  if (!read_quic_varint(data, length, offset, &packet_length) ||
      packet_length == 0U ||
      packet_length > static_cast<uint64_t>(length - *offset)) {
    return false;
  }
  return true;
}

} // namespace

bool read_quic_varint(const uint8_t *data, size_t length, size_t *offset,
                      uint64_t *value) {
  if (data == nullptr || offset == nullptr || value == nullptr ||
      !has_bytes(*offset, 1U, length)) {
    return false;
  }
  const uint8_t first = data[*offset];
  const size_t encoded_length = static_cast<size_t>(1U) << (first >> 6U);
  if (!has_bytes(*offset, encoded_length, length)) {
    return false;
  }

  uint64_t decoded = first & 0x3fU;
  for (size_t index = 1U; index < encoded_length; ++index) {
    if (decoded > (std::numeric_limits<uint64_t>::max() >> 8U)) {
      return false;
    }
    decoded = (decoded << 8U) | data[*offset + index];
  }
  *offset += encoded_length;
  *value = decoded;
  return true;
}

quic_parse_result parse_quic_udp_payload(const uint8_t *data, size_t length) {
  quic_parse_result result{};
  if (data == nullptr || length == 0U) {
    return result;
  }

  const uint8_t first = data[0U];
  result.fixed_bit = (first & FIXED_BIT) != 0U;
  if ((first & HEADER_FORM_BIT) == 0U) {
    if (!result.fixed_bit) {
      return result;
    }
    result.short_header = true;
    result.header_type = quic_header_type::short_header;
    result.status = length >= 2U ? quic_parse_status::parsed
                                 : quic_parse_status::malformed_candidate;
    return result;
  }

  result.long_header = true;
  if (length < 5U) {
    return result.fixed_bit ? malformed(result) : result;
  }
  result.version_available = true;
  result.version = read_be32(data + 1U);
  const bool known_version = is_known_version(result.version);
  if (result.version == 0U) {
    result.header_type = quic_header_type::version_negotiation;
  } else if (known_version) {
    result.header_type = decode_long_header_type(result.version, first);
  }
  if (!result.fixed_bit && result.version != 0U && !known_version) {
    result.long_header = false;
    return result;
  }
  if (!result.fixed_bit && known_version) {
    return malformed(result);
  }

  size_t offset = 5U;
  if (!parse_connection_ids(data, length, &offset, known_version, &result)) {
    return malformed(result);
  }

  if (result.version == 0U) {
    const size_t remaining = length - offset;
    if (remaining < 4U || (remaining & 3U) != 0U) {
      return malformed(result);
    }
    for (size_t index = offset; index < length; index += 4U) {
      if (read_be32(data + index) == 0U) {
        return malformed(result);
      }
    }
    result.status = quic_parse_status::parsed;
    return result;
  }

  if (!known_version) {
    result.status = quic_parse_status::parsed;
    return result;
  }

  switch (result.header_type) {
  case quic_header_type::initial: {
    uint64_t token_length = 0U;
    if (!read_quic_varint(data, length, &offset, &token_length) ||
        token_length > static_cast<uint64_t>(length - offset)) {
      return malformed(result);
    }
    result.token_length_available = true;
    result.token_length = token_length;
    offset += static_cast<size_t>(token_length);
    if (!parse_packet_length(data, length, &offset)) {
      return malformed(result);
    }
    break;
  }
  case quic_header_type::zero_rtt:
  case quic_header_type::handshake:
    if (!parse_packet_length(data, length, &offset)) {
      return malformed(result);
    }
    break;
  case quic_header_type::retry:
    if (!has_bytes(offset, RETRY_INTEGRITY_TAG_LENGTH, length)) {
      return malformed(result);
    }
    break;
  default:
    return malformed(result);
  }

  result.status = quic_parse_status::parsed;
  return result;
}

} // namespace ktag_proxy_features
