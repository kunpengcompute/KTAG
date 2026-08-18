/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROXY_QUIC_PARSER_H
#define KTAG_PROXY_QUIC_PARSER_H

#include <cstddef>
#include <cstdint>

namespace ktag_proxy_features {

inline constexpr uint32_t KTAG_QUIC_VERSION_1 = 0x00000001U;
inline constexpr uint32_t KTAG_QUIC_VERSION_2 = 0x6b3343cfU;

enum class quic_parse_status : uint8_t {
  not_candidate = 0U,
  parsed,
  malformed_candidate,
};

enum class quic_header_type : uint8_t {
  unknown = 0U,
  version_negotiation = 1U,
  initial = 2U,
  zero_rtt = 3U,
  handshake = 4U,
  retry = 5U,
  short_header = 6U,
};

struct quic_parse_result {
  quic_parse_status status = quic_parse_status::not_candidate;
  quic_header_type header_type = quic_header_type::unknown;
  bool fixed_bit = false;
  bool long_header = false;
  bool short_header = false;
  bool version_available = false;
  uint32_t version = 0U;
  bool connection_ids_available = false;
  uint8_t dcid_length = 0U;
  uint8_t scid_length = 0U;
  bool token_length_available = false;
  uint64_t token_length = 0U;
};

bool read_quic_varint(const uint8_t *data, size_t length, size_t *offset,
                      uint64_t *value);

quic_parse_result parse_quic_udp_payload(const uint8_t *data, size_t length);

} // namespace ktag_proxy_features

#endif /* KTAG_PROXY_QUIC_PARSER_H */
