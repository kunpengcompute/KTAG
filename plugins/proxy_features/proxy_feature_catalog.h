/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROXY_FEATURE_CATALOG_H
#define KTAG_PROXY_FEATURE_CATALOG_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ktag_proxy_features {

inline constexpr uint32_t KTAG_PROXY_PROVIDER_VERSION = 1U;

struct feature_definition {
  const char *feature_id;
  uint32_t dimension;
};

struct provider_catalog {
  const char *provider_id;
  uint32_t provider_version;
  const feature_definition *features;
  size_t feature_count;
};

inline constexpr char KTAG_PROXY_SEQUENCE_PROVIDER_ID[] =
    "ktag.proxy.sequence.v1";
inline constexpr char KTAG_PROXY_TCP_SIGNATURE_PROVIDER_ID[] =
    "ktag.proxy.tcp_signature.v1";
inline constexpr char KTAG_PROXY_QUIC_PROVIDER_ID[] = "ktag.proxy.quic.v1";

inline constexpr std::array<feature_definition, 10U>
    KTAG_PROXY_SEQUENCE_FEATURES{{
        {"proxy.sequence.signed_payload_length", 32U},
        {"proxy.sequence.relative_time_log2_us", 32U},
        {"proxy.sequence.tcp_flags", 32U},
        {"proxy.sequence.valid_count", 1U},
        {"proxy.burst.signed_packet_count", 16U},
        {"proxy.burst.signed_payload_bytes_log2", 16U},
        {"proxy.burst.duration_log2_us", 16U},
        {"proxy.burst.valid_count", 1U},
        {"proxy.direction.switch_count", 1U},
        {"proxy.direction.forward_payload_ratio", 1U},
    }};

inline constexpr std::array<feature_definition, 35U>
    KTAG_PROXY_TCP_SIGNATURE_FEATURES{{
        {"proxy.tcp.forward_prefix_length_log2", 1U},
        {"proxy.tcp.reverse_prefix_length_log2", 1U},
        {"proxy.tcp.forward_prefix_truncated", 1U},
        {"proxy.tcp.reverse_prefix_truncated", 1U},
        {"proxy.tcp.segment_gap_count", 1U},
        {"proxy.tcp.retransmission_count", 1U},
        {"proxy.tcp.out_of_order_count", 1U},
        {"proxy.tcp.forward_entropy", 1U},
        {"proxy.tcp.reverse_entropy", 1U},
        {"proxy.tcp.forward_printable_ratio", 1U},
        {"proxy.tcp.reverse_printable_ratio", 1U},
        {"proxy.tcp.forward_zero_ratio", 1U},
        {"proxy.tcp.reverse_zero_ratio", 1U},
        {"proxy.tls.record_candidate", 1U},
        {"proxy.tls.client_hello_complete", 1U},
        {"proxy.tls.legacy_version", 1U},
        {"proxy.tls.supported_version_max", 1U},
        {"proxy.tls.cipher_suite_count", 1U},
        {"proxy.tls.extension_count", 1U},
        {"proxy.tls.cipher_suite_hash_u16", 2U},
        {"proxy.tls.extension_order_hash_u16", 2U},
        {"proxy.tls.sni_present", 1U},
        {"proxy.tls.alpn_h2_present", 1U},
        {"proxy.tls.alpn_http11_present", 1U},
        {"proxy.tls.session_id_length", 1U},
        {"proxy.tls.psk_present", 1U},
        {"proxy.tls.early_data_present", 1U},
        {"proxy.tls.record_signed_length", 16U},
        {"proxy.tls.record_valid_count", 1U},
        {"proxy.tls.parse_failure_count", 1U},
        {"proxy.carrier.http1_request", 1U},
        {"proxy.carrier.websocket_upgrade", 1U},
        {"proxy.carrier.http2_preface", 1U},
        {"proxy.carrier.http2_frame", 1U},
        {"proxy.carrier.grpc_content_type", 1U},
    }};

inline constexpr std::array<feature_definition, 23U> KTAG_PROXY_QUIC_FEATURES{{
    {"proxy.quic.packet_candidate_count", 1U},
    {"proxy.quic.parse_success_count", 1U},
    {"proxy.quic.parse_failure_count", 1U},
    {"proxy.quic.fixed_bit_count", 1U},
    {"proxy.quic.long_header_count", 1U},
    {"proxy.quic.short_header_count", 1U},
    {"proxy.quic.version_v1_count", 1U},
    {"proxy.quic.version_v2_count", 1U},
    {"proxy.quic.version_other_count", 1U},
    {"proxy.quic.version_negotiation_count", 1U},
    {"proxy.quic.initial_count", 1U},
    {"proxy.quic.zero_rtt_count", 1U},
    {"proxy.quic.handshake_count", 1U},
    {"proxy.quic.retry_count", 1U},
    {"proxy.quic.first_initial_length_log2", 1U},
    {"proxy.quic.first_dcid_length", 1U},
    {"proxy.quic.first_scid_length", 1U},
    {"proxy.quic.first_token_length_log2", 1U},
    {"proxy.quic.signed_datagram_length", 16U},
    {"proxy.quic.header_type_sequence", 16U},
    {"proxy.quic.sequence_valid_count", 1U},
    {"proxy.quic.first_byte_high_nibble_histogram", 16U},
    {"proxy.quic.payload_entropy_mean", 1U},
}};

inline constexpr provider_catalog KTAG_PROXY_SEQUENCE_CATALOG{
    KTAG_PROXY_SEQUENCE_PROVIDER_ID,
    KTAG_PROXY_PROVIDER_VERSION,
    KTAG_PROXY_SEQUENCE_FEATURES.data(),
    KTAG_PROXY_SEQUENCE_FEATURES.size(),
};

inline constexpr provider_catalog KTAG_PROXY_TCP_SIGNATURE_CATALOG{
    KTAG_PROXY_TCP_SIGNATURE_PROVIDER_ID,
    KTAG_PROXY_PROVIDER_VERSION,
    KTAG_PROXY_TCP_SIGNATURE_FEATURES.data(),
    KTAG_PROXY_TCP_SIGNATURE_FEATURES.size(),
};

inline constexpr provider_catalog KTAG_PROXY_QUIC_CATALOG{
    KTAG_PROXY_QUIC_PROVIDER_ID,
    KTAG_PROXY_PROVIDER_VERSION,
    KTAG_PROXY_QUIC_FEATURES.data(),
    KTAG_PROXY_QUIC_FEATURES.size(),
};

inline constexpr std::array<const provider_catalog *, 3U>
    KTAG_PROXY_PROVIDER_CATALOG{{
        &KTAG_PROXY_SEQUENCE_CATALOG,
        &KTAG_PROXY_TCP_SIGNATURE_CATALOG,
        &KTAG_PROXY_QUIC_CATALOG,
    }};

constexpr uint32_t proxy_feature_descriptor_count() {
  uint32_t count = 0U;
  for (const auto *provider : KTAG_PROXY_PROVIDER_CATALOG) {
    count += static_cast<uint32_t>(provider->feature_count);
  }
  return count;
}

constexpr uint32_t proxy_feature_value_count() {
  uint32_t count = 0U;
  for (const auto *provider : KTAG_PROXY_PROVIDER_CATALOG) {
    for (size_t index = 0U; index < provider->feature_count; ++index) {
      count += provider->features[index].dimension;
    }
  }
  return count;
}

constexpr bool proxy_feature_catalog_is_valid() {
  for (size_t provider_index = 0U;
       provider_index < KTAG_PROXY_PROVIDER_CATALOG.size(); ++provider_index) {
    const auto *provider = KTAG_PROXY_PROVIDER_CATALOG[provider_index];
    if (provider == nullptr || provider->provider_id == nullptr ||
        std::string_view(provider->provider_id).empty() ||
        provider->provider_version == 0U || provider->features == nullptr ||
        provider->feature_count == 0U) {
      return false;
    }
    for (size_t feature_index = 0U; feature_index < provider->feature_count;
         ++feature_index) {
      const auto &feature = provider->features[feature_index];
      if (feature.feature_id == nullptr ||
          std::string_view(feature.feature_id).empty() ||
          feature.dimension == 0U) {
        return false;
      }
      for (size_t previous_provider = 0U; previous_provider <= provider_index;
           ++previous_provider) {
        const auto *previous = KTAG_PROXY_PROVIDER_CATALOG[previous_provider];
        const size_t previous_limit = previous_provider == provider_index
                                          ? feature_index
                                          : previous->feature_count;
        for (size_t previous_feature = 0U; previous_feature < previous_limit;
             ++previous_feature) {
          if (std::string_view(feature.feature_id) ==
              previous->features[previous_feature].feature_id) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

inline constexpr uint32_t KTAG_PROXY_FEATURE_DESCRIPTOR_COUNT =
    proxy_feature_descriptor_count();
inline constexpr uint32_t KTAG_PROXY_FEATURE_VALUE_COUNT =
    proxy_feature_value_count();

static_assert(KTAG_PROXY_FEATURE_DESCRIPTOR_COUNT == 68U,
              "proxy v1 must expose exactly 68 new Feature Descriptors");
static_assert(KTAG_PROXY_FEATURE_VALUE_COUNT == 268U,
              "proxy v1 must expose exactly 268 new values");
static_assert(
    proxy_feature_catalog_is_valid(),
    "proxy v1 Feature IDs must be non-empty, unique and non-zero dimensional");

} // namespace ktag_proxy_features

#endif /* KTAG_PROXY_FEATURE_CATALOG_H */
