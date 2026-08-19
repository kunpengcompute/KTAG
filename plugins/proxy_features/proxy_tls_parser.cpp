/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_tls_parser.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace ktag_proxy_features {
namespace {

constexpr uint8_t TLS_HANDSHAKE = 22U;
constexpr uint8_t CLIENT_HELLO = 1U;
constexpr size_t TLS_RECORD_HEADER_LENGTH = 5U;
constexpr uint32_t TLS_RECORD_MAX_FRAGMENT_LENGTH = 18432U;
constexpr size_t TLS_HANDSHAKE_CAPACITY = 16384U;

uint16_t read_be16(const uint8_t *data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0U]) << 8U) |
                               static_cast<uint16_t>(data[1U]));
}

uint32_t read_be24(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0U]) << 16U) |
         (static_cast<uint32_t>(data[1U]) << 8U) |
         static_cast<uint32_t>(data[2U]);
}

bool is_tls_content_type(uint8_t value) { return value >= 20U && value <= 24U; }

bool has_bytes(size_t offset, size_t required, size_t length) {
  return offset <= length && required <= length - offset;
}

bool parse_alpn(const uint8_t *data, size_t length, tls_parse_result *result) {
  if (length < 2U || result == nullptr) {
    return false;
  }
  const size_t list_length = read_be16(data);
  if (list_length != length - 2U) {
    return false;
  }

  size_t offset = 2U;
  while (offset < length) {
    const size_t name_length = data[offset++];
    if (name_length == 0U || !has_bytes(offset, name_length, length)) {
      return false;
    }
    if (name_length == 2U && std::memcmp(data + offset, "h2", 2U) == 0) {
      result->alpn_h2_present = true;
    }
    if (name_length == 8U && std::memcmp(data + offset, "http/1.1", 8U) == 0) {
      result->alpn_http11_present = true;
    }
    offset += name_length;
  }
  return true;
}

bool parse_supported_versions(const uint8_t *data, size_t length,
                              tls_parse_result *result) {
  if (length < 1U || result == nullptr) {
    return false;
  }
  const size_t list_length = data[0U];
  if (list_length == 0U || (list_length & 1U) != 0U ||
      list_length != length - 1U) {
    return false;
  }
  for (size_t offset = 1U; offset < length; offset += 2U) {
    const uint16_t version = read_be16(data + offset);
    if (version >= 0x0300U && version <= 0x0304U) {
      result->supported_version_max =
          std::max(result->supported_version_max, version);
    }
  }
  return true;
}

bool parse_client_hello_body(const uint8_t *data, size_t length,
                             tls_parse_result *result) {
  constexpr size_t VERSION_AND_RANDOM_LENGTH = 34U;
  if (data == nullptr || result == nullptr ||
      length < VERSION_AND_RANDOM_LENGTH + 1U) {
    return false;
  }

  result->legacy_version = read_be16(data);
  size_t offset = VERSION_AND_RANDOM_LENGTH;
  const size_t session_id_length = data[offset++];
  if (!has_bytes(offset, session_id_length, length)) {
    return false;
  }
  result->session_id_length = static_cast<uint32_t>(session_id_length);
  offset += session_id_length;

  if (!has_bytes(offset, 2U, length)) {
    return false;
  }
  const size_t cipher_suite_bytes = read_be16(data + offset);
  offset += 2U;
  if (cipher_suite_bytes == 0U || (cipher_suite_bytes & 1U) != 0U ||
      !has_bytes(offset, cipher_suite_bytes, length)) {
    return false;
  }
  result->cipher_suite_count = static_cast<uint32_t>(cipher_suite_bytes / 2U);
  result->cipher_suite_hash = proxy_fnv1a_32(data + offset, cipher_suite_bytes);
  offset += cipher_suite_bytes;

  if (!has_bytes(offset, 1U, length)) {
    return false;
  }
  const size_t compression_length = data[offset++];
  if (compression_length == 0U ||
      !has_bytes(offset, compression_length, length)) {
    return false;
  }
  offset += compression_length;
  if (offset == length) {
    result->client_hello_complete = true;
    return true;
  }

  if (!has_bytes(offset, 2U, length)) {
    return false;
  }
  const size_t extensions_length = read_be16(data + offset);
  offset += 2U;
  if (extensions_length != length - offset) {
    return false;
  }

  uint32_t extension_hash = 2166136261U;
  size_t extensions_end = offset + extensions_length;
  while (offset < extensions_end) {
    if (!has_bytes(offset, 4U, extensions_end)) {
      return false;
    }
    const uint16_t extension_type = read_be16(data + offset);
    const size_t extension_length = read_be16(data + offset + 2U);
    const std::array<uint8_t, 2U> type_bytes{{
        static_cast<uint8_t>(extension_type >> 8U),
        static_cast<uint8_t>(extension_type & 0xffU),
    }};
    for (uint8_t value : type_bytes) {
      extension_hash ^= value;
      extension_hash *= 16777619U;
    }
    offset += 4U;
    if (!has_bytes(offset, extension_length, extensions_end)) {
      return false;
    }

    ++result->extension_count;
    switch (extension_type) {
    case 0U:
      result->sni_present = true;
      break;
    case 16U:
      if (!parse_alpn(data + offset, extension_length, result)) {
        return false;
      }
      break;
    case 41U:
      result->psk_present = true;
      break;
    case 42U:
      result->early_data_present = true;
      break;
    case 43U:
      if (!parse_supported_versions(data + offset, extension_length, result)) {
        return false;
      }
      break;
    default:
      break;
    }
    offset += extension_length;
  }

  result->extension_order_hash =
      result->extension_count == 0U ? 0U : extension_hash;
  result->client_hello_complete = true;
  return true;
}

void parse_handshake_stream(const uint8_t *data, size_t length,
                            tls_parse_result *result) {
  size_t offset = 0U;
  while (offset < length) {
    if (!has_bytes(offset, 4U, length)) {
      ++result->parse_failure_count;
      return;
    }
    const uint8_t message_type = data[offset];
    const size_t message_length = read_be24(data + offset + 1U);
    offset += 4U;
    if (!has_bytes(offset, message_length, length)) {
      ++result->parse_failure_count;
      return;
    }
    if (message_type == CLIENT_HELLO && !result->client_hello_complete) {
      if (!parse_client_hello_body(data + offset, message_length, result)) {
        ++result->parse_failure_count;
        return;
      }
    }
    offset += message_length;
  }
}

} // namespace

uint32_t proxy_fnv1a_32(const uint8_t *data, size_t length) {
  if (data == nullptr && length != 0U) {
    return 0U;
  }
  uint32_t hash = 2166136261U;
  for (size_t index = 0U; index < length; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

void parse_tls_prefix(const uint8_t *data, size_t length,
                      tls_parse_result *result) {
  if (result == nullptr) {
    return;
  }
  *result = tls_parse_result{};
  if (data == nullptr || length == 0U) {
    return;
  }

  if (!is_tls_content_type(data[0U]) || (length >= 2U && data[1U] != 3U) ||
      (length >= 3U && data[2U] > 4U)) {
    return;
  }
  result->record_candidate = true;

  std::array<uint8_t, TLS_HANDSHAKE_CAPACITY> handshake{};
  size_t handshake_length = 0U;
  size_t offset = 0U;
  while (offset < length) {
    if (!has_bytes(offset, TLS_RECORD_HEADER_LENGTH, length)) {
      ++result->parse_failure_count;
      break;
    }
    const uint8_t content_type = data[offset];
    const uint8_t major_version = data[offset + 1U];
    const uint8_t minor_version = data[offset + 2U];
    const size_t fragment_length = read_be16(data + offset + 3U);
    if (!is_tls_content_type(content_type) || major_version != 3U ||
        minor_version > 4U ||
        fragment_length > TLS_RECORD_MAX_FRAGMENT_LENGTH) {
      ++result->parse_failure_count;
      break;
    }
    offset += TLS_RECORD_HEADER_LENGTH;
    if (!has_bytes(offset, fragment_length, length)) {
      ++result->parse_failure_count;
      break;
    }

    if (result->stored_record_count < result->record_lengths.size()) {
      result->record_lengths[result->stored_record_count++] =
          static_cast<uint16_t>(fragment_length);
    }
    if (result->complete_record_count < std::numeric_limits<uint32_t>::max()) {
      ++result->complete_record_count;
    }

    if (content_type == TLS_HANDSHAKE && fragment_length != 0U) {
      const size_t available = handshake.size() - handshake_length;
      const size_t copy_length = std::min(fragment_length, available);
      std::memcpy(handshake.data() + handshake_length, data + offset,
                  copy_length);
      handshake_length += copy_length;
      if (copy_length != fragment_length) {
        ++result->parse_failure_count;
        break;
      }
    }
    offset += fragment_length;
  }

  if (handshake_length != 0U) {
    parse_handshake_stream(handshake.data(), handshake_length, result);
  }
}

} // namespace ktag_proxy_features
