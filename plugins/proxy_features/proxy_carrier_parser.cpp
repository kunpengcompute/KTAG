/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_carrier_parser.h"

#include <array>
#include <cstring>
#include <string_view>

namespace ktag_proxy_features {
namespace {

constexpr std::string_view HTTP2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

uint8_t ascii_lower(uint8_t value) {
  return value >= static_cast<uint8_t>('A') &&
                 value <= static_cast<uint8_t>('Z')
             ? static_cast<uint8_t>(value + ('a' - 'A'))
             : value;
}

bool starts_with(const uint8_t *data, size_t length, std::string_view value) {
  return data != nullptr && length >= value.size() &&
         std::memcmp(data, value.data(), value.size()) == 0;
}

bool contains_exact(const uint8_t *data, size_t length,
                    std::string_view value) {
  if (data == nullptr || value.empty() || length < value.size()) {
    return false;
  }
  for (size_t offset = 0U; offset <= length - value.size(); ++offset) {
    if (std::memcmp(data + offset, value.data(), value.size()) == 0) {
      return true;
    }
  }
  return false;
}

bool contains_case_insensitive(const uint8_t *data, size_t length,
                               std::string_view value) {
  if (data == nullptr || value.empty() || length < value.size()) {
    return false;
  }
  for (size_t offset = 0U; offset <= length - value.size(); ++offset) {
    bool equal = true;
    for (size_t index = 0U; index < value.size(); ++index) {
      if (ascii_lower(data[offset + index]) !=
          ascii_lower(static_cast<uint8_t>(value[index]))) {
        equal = false;
        break;
      }
    }
    if (equal) {
      return true;
    }
  }
  return false;
}

bool is_http1_request(const uint8_t *data, size_t length) {
  constexpr std::array<std::string_view, 9U> METHODS{{
      "GET ",
      "POST ",
      "PUT ",
      "DELETE ",
      "HEAD ",
      "OPTIONS ",
      "CONNECT ",
      "TRACE ",
      "PATCH ",
  }};
  bool method_found = false;
  for (const auto method : METHODS) {
    if (starts_with(data, length, method)) {
      method_found = true;
      break;
    }
  }
  if (!method_found) {
    return false;
  }

  size_t line_length = 0U;
  while (line_length + 1U < length &&
         !(data[line_length] == '\r' && data[line_length + 1U] == '\n')) {
    ++line_length;
  }
  if (line_length + 1U >= length) {
    return false;
  }
  return contains_exact(data, line_length, " HTTP/1.");
}

bool is_http2_frame_header(const uint8_t *data, size_t length) {
  constexpr size_t FRAME_HEADER_LENGTH = 9U;
  constexpr uint32_t DEFAULT_MAX_FRAME_SIZE = 16384U;
  if (data == nullptr || length < FRAME_HEADER_LENGTH) {
    return false;
  }
  const uint32_t payload_length = (static_cast<uint32_t>(data[0U]) << 16U) |
                                  (static_cast<uint32_t>(data[1U]) << 8U) |
                                  static_cast<uint32_t>(data[2U]);
  const uint8_t frame_type = data[3U];
  const uint32_t stream_id = (static_cast<uint32_t>(data[5U] & 0x7fU) << 24U) |
                             (static_cast<uint32_t>(data[6U]) << 16U) |
                             (static_cast<uint32_t>(data[7U]) << 8U) |
                             static_cast<uint32_t>(data[8U]);
  if (payload_length > DEFAULT_MAX_FRAME_SIZE || frame_type > 9U) {
    return false;
  }
  if ((frame_type == 4U || frame_type == 6U || frame_type == 7U) &&
      stream_id != 0U) {
    return false;
  }
  if ((frame_type == 0U || frame_type == 1U || frame_type == 2U ||
       frame_type == 5U || frame_type == 9U) &&
      stream_id == 0U) {
    return false;
  }
  return true;
}

} // namespace

void parse_cleartext_carrier(const uint8_t *data, size_t length,
                             carrier_parse_result *result) {
  if (result == nullptr) {
    return;
  }
  *result = carrier_parse_result{};
  if (data == nullptr || length == 0U) {
    return;
  }

  result->http1_request = is_http1_request(data, length);
  const bool upgrade_header =
      contains_case_insensitive(data, length, "upgrade: websocket");
  const bool connection_upgrade =
      contains_case_insensitive(data, length, "connection: upgrade");
  result->websocket_upgrade =
      result->http1_request && upgrade_header && connection_upgrade;
  result->grpc_content_type =
      contains_case_insensitive(data, length, "content-type: application/grpc");

  result->http2_preface = starts_with(data, length, HTTP2_PREFACE);
  const size_t frame_offset = result->http2_preface ? HTTP2_PREFACE.size() : 0U;
  result->http2_frame =
      frame_offset <= length &&
      is_http2_frame_header(data + frame_offset, length - frame_offset);
}

} // namespace ktag_proxy_features
