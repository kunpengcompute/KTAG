/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROXY_TLS_PARSER_H
#define KTAG_PROXY_TLS_PARSER_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace ktag_proxy_features {

inline constexpr size_t KTAG_PROXY_TLS_RECORD_CAPACITY = 16U;

struct tls_parse_result {
  bool record_candidate = false;
  bool client_hello_complete = false;
  uint16_t legacy_version = 0U;
  uint16_t supported_version_max = 0U;
  uint32_t cipher_suite_count = 0U;
  uint32_t extension_count = 0U;
  uint32_t cipher_suite_hash = 0U;
  uint32_t extension_order_hash = 0U;
  bool sni_present = false;
  bool alpn_h2_present = false;
  bool alpn_http11_present = false;
  uint32_t session_id_length = 0U;
  bool psk_present = false;
  bool early_data_present = false;
  std::array<uint16_t, KTAG_PROXY_TLS_RECORD_CAPACITY> record_lengths{};
  uint32_t stored_record_count = 0U;
  uint32_t complete_record_count = 0U;
  uint32_t parse_failure_count = 0U;
};

uint32_t proxy_fnv1a_32(const uint8_t *data, size_t length);

void parse_tls_prefix(const uint8_t *data, size_t length,
                      tls_parse_result *result);

} // namespace ktag_proxy_features

#endif /* KTAG_PROXY_TLS_PARSER_H */
