/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_quic_provider.h"

#include <array>
#include <cstdint>
#include <new>

#include "proxy_feature_catalog.h"
#include "proxy_math.h"
#include "proxy_provider_common.h"
#include "proxy_quic_parser.h"

namespace ktag_proxy_features {
namespace {

constexpr uint8_t UDP_PROTOCOL = 17U;
constexpr uint32_t DATAGRAM_CAPACITY = 64U;
constexpr uint32_t SEQUENCE_CAPACITY = 16U;
constexpr uint32_t HISTOGRAM_CAPACITY = 16U;

enum quic_feature_index : size_t {
  PACKET_CANDIDATE_COUNT = 0U,
  PARSE_SUCCESS_COUNT,
  PARSE_FAILURE_COUNT,
  FIXED_BIT_COUNT,
  LONG_HEADER_COUNT,
  SHORT_HEADER_COUNT,
  VERSION_V1_COUNT,
  VERSION_V2_COUNT,
  VERSION_OTHER_COUNT,
  VERSION_NEGOTIATION_COUNT,
  INITIAL_COUNT,
  ZERO_RTT_COUNT,
  HANDSHAKE_COUNT,
  RETRY_COUNT,
  FIRST_INITIAL_LENGTH_LOG2,
  FIRST_DCID_LENGTH,
  FIRST_SCID_LENGTH,
  FIRST_TOKEN_LENGTH_LOG2,
  SIGNED_DATAGRAM_LENGTH,
  HEADER_TYPE_SEQUENCE,
  SEQUENCE_VALID_COUNT,
  FIRST_BYTE_HIGH_NIBBLE_HISTOGRAM,
  PAYLOAD_ENTROPY_MEAN,
  QUIC_FEATURE_COUNT,
};

static_assert(QUIC_FEATURE_COUNT == KTAG_PROXY_QUIC_FEATURES.size());
static_assert(KTAG_PROXY_QUIC_FEATURES[SIGNED_DATAGRAM_LENGTH].dimension ==
              SEQUENCE_CAPACITY);
static_assert(KTAG_PROXY_QUIC_FEATURES[HEADER_TYPE_SEQUENCE].dimension ==
              SEQUENCE_CAPACITY);
static_assert(KTAG_PROXY_QUIC_FEATURES[FIRST_BYTE_HIGH_NIBBLE_HISTOGRAM]
                  .dimension == HISTOGRAM_CAPACITY);

struct quic_flow_state {
  uint32_t datagram_count = 0U;
  uint32_t sequence_count = 0U;
  uint64_t packet_candidate_count = 0U;
  uint64_t parse_success_count = 0U;
  uint64_t parse_failure_count = 0U;
  uint64_t fixed_bit_count = 0U;
  uint64_t long_header_count = 0U;
  uint64_t short_header_count = 0U;
  uint64_t version_v1_count = 0U;
  uint64_t version_v2_count = 0U;
  uint64_t version_other_count = 0U;
  uint64_t version_negotiation_count = 0U;
  uint64_t initial_count = 0U;
  uint64_t zero_rtt_count = 0U;
  uint64_t handshake_count = 0U;
  uint64_t retry_count = 0U;
  bool has_first_initial = false;
  bool has_first_connection_ids = false;
  bool has_first_token = false;
  float first_initial_length_log2 = 0.0F;
  float first_dcid_length = 0.0F;
  float first_scid_length = 0.0F;
  float first_token_length_log2 = 0.0F;
  std::array<float, SEQUENCE_CAPACITY> signed_datagram_length{};
  std::array<float, SEQUENCE_CAPACITY> header_type_sequence{};
  std::array<uint64_t, HISTOGRAM_CAPACITY> first_byte_high_nibble_histogram{};
  double payload_entropy_sum = 0.0;
};

ktag_status set_optional(ktag_feature_output *output,
                         quic_feature_index feature, const float *values,
                         uint32_t value_count) {
  const ktag_status status = ktag_feature_output_set(
      output, KTAG_PROXY_QUIC_FEATURES[feature].feature_id, values,
      value_count);
  return status == KTAG_ERROR_NOT_FOUND ? KTAG_SUCCESS : status;
}

ktag_status set_scalar(ktag_feature_output *output, quic_feature_index feature,
                       float value) {
  return set_optional(output, feature, &value, 1U);
}

void increment(uint64_t *value) { *value = saturating_add_u64(*value, 1U); }

void update_version_counts(quic_flow_state *state,
                           const quic_parse_result &parsed) {
  if (!parsed.version_available) {
    return;
  }
  if (parsed.version == KTAG_QUIC_VERSION_1) {
    increment(&state->version_v1_count);
  } else if (parsed.version == KTAG_QUIC_VERSION_2) {
    increment(&state->version_v2_count);
  } else if (parsed.version != 0U) {
    increment(&state->version_other_count);
  }
}

void update_header_type_counts(quic_flow_state *state, quic_header_type type) {
  switch (type) {
  case quic_header_type::version_negotiation:
    increment(&state->version_negotiation_count);
    break;
  case quic_header_type::initial:
    increment(&state->initial_count);
    break;
  case quic_header_type::zero_rtt:
    increment(&state->zero_rtt_count);
    break;
  case quic_header_type::handshake:
    increment(&state->handshake_count);
    break;
  case quic_header_type::retry:
    increment(&state->retry_count);
    break;
  default:
    break;
  }
}

void update_quic_features(quic_flow_state *state,
                          const quic_parse_result &parsed,
                          uint32_t payload_length) {
  if (parsed.status == quic_parse_status::not_candidate) {
    return;
  }
  increment(&state->packet_candidate_count);
  if (parsed.status == quic_parse_status::parsed) {
    increment(&state->parse_success_count);
  } else {
    increment(&state->parse_failure_count);
  }
  if (parsed.fixed_bit) {
    increment(&state->fixed_bit_count);
  }
  if (parsed.long_header) {
    increment(&state->long_header_count);
  }
  if (parsed.short_header) {
    increment(&state->short_header_count);
  }
  update_version_counts(state, parsed);
  update_header_type_counts(state, parsed.header_type);

  if (parsed.header_type == quic_header_type::initial &&
      !state->has_first_initial) {
    state->has_first_initial = true;
    state->first_initial_length_log2 = log2_1p_u64(payload_length);
  }
  if (parsed.connection_ids_available && !state->has_first_connection_ids) {
    state->has_first_connection_ids = true;
    state->first_dcid_length = bounded_float(parsed.dcid_length);
    state->first_scid_length = bounded_float(parsed.scid_length);
  }
  if (parsed.token_length_available && !state->has_first_token) {
    state->has_first_token = true;
    state->first_token_length_log2 = log2_1p_u64(parsed.token_length);
  }
}

ktag_status quic_flow_create(void *, const ktag_flow_view *,
                             void **flow_state) {
  if (flow_state == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  auto *state = new (std::nothrow) quic_flow_state();
  if (state == nullptr) {
    return KTAG_ERROR_OUT_OF_MEMORY;
  }
  *flow_state = state;
  return KTAG_SUCCESS;
}

ktag_status quic_on_packet(void *, void *flow_state,
                           const ktag_packet_view *packet,
                           const ktag_flow_view *) {
  auto *state = static_cast<quic_flow_state *>(flow_state);
  if (state == nullptr || packet == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  ktag_packet_transport_view transport{};
  transport.struct_size = sizeof(transport);
  transport.api_version = KTAG_API_VERSION;
  uint32_t payload_length = 0U;
  const uint8_t *payload =
      ktag_packet_view_get_payload(packet, &payload_length);
  if (ktag_packet_view_get_transport(packet, &transport) != KTAG_SUCCESS ||
      (payload == nullptr && payload_length != 0U) ||
      (transport.direction != KTAG_DIRECTION_FORWARD &&
       transport.direction != KTAG_DIRECTION_REVERSE)) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (transport.transport_protocol != UDP_PROTOCOL ||
      state->datagram_count >= DATAGRAM_CAPACITY) {
    return KTAG_SUCCESS;
  }

  ++state->datagram_count;
  const quic_parse_result parsed =
      parse_quic_udp_payload(payload, payload_length);
  if (state->sequence_count < SEQUENCE_CAPACITY) {
    const uint32_t index = state->sequence_count++;
    state->signed_datagram_length[index] =
        signed_bounded_float(payload_length, transport.direction);
    state->header_type_sequence[index] =
        bounded_float(static_cast<uint8_t>(parsed.header_type));
  }
  if (payload_length != 0U) {
    const uint8_t high_nibble = payload[0U] >> 4U;
    increment(&state->first_byte_high_nibble_histogram[high_nibble]);
    state->payload_entropy_sum +=
        static_cast<double>(byte_entropy(payload, payload_length));
  }
  update_quic_features(state, parsed, payload_length);
  return KTAG_SUCCESS;
}

ktag_status quic_flow_finish(void *, void *flow_state, const ktag_flow_view *,
                             ktag_feature_output *output) {
  const auto *state = static_cast<const quic_flow_state *>(flow_state);
  if (state == nullptr || output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  std::array<float, HISTOGRAM_CAPACITY> histogram{};
  for (size_t index = 0U; index < histogram.size(); ++index) {
    histogram[index] =
        bounded_float(state->first_byte_high_nibble_histogram[index]);
  }
  const float entropy_mean =
      state->datagram_count == 0U
          ? 0.0F
          : static_cast<float>(state->payload_entropy_sum /
                               static_cast<double>(state->datagram_count));

  if (set_scalar(output, PACKET_CANDIDATE_COUNT,
                 bounded_float(state->packet_candidate_count)) !=
          KTAG_SUCCESS ||
      set_scalar(output, PARSE_SUCCESS_COUNT,
                 bounded_float(state->parse_success_count)) != KTAG_SUCCESS ||
      set_scalar(output, PARSE_FAILURE_COUNT,
                 bounded_float(state->parse_failure_count)) != KTAG_SUCCESS ||
      set_scalar(output, FIXED_BIT_COUNT,
                 bounded_float(state->fixed_bit_count)) != KTAG_SUCCESS ||
      set_scalar(output, LONG_HEADER_COUNT,
                 bounded_float(state->long_header_count)) != KTAG_SUCCESS ||
      set_scalar(output, SHORT_HEADER_COUNT,
                 bounded_float(state->short_header_count)) != KTAG_SUCCESS ||
      set_scalar(output, VERSION_V1_COUNT,
                 bounded_float(state->version_v1_count)) != KTAG_SUCCESS ||
      set_scalar(output, VERSION_V2_COUNT,
                 bounded_float(state->version_v2_count)) != KTAG_SUCCESS ||
      set_scalar(output, VERSION_OTHER_COUNT,
                 bounded_float(state->version_other_count)) != KTAG_SUCCESS ||
      set_scalar(output, VERSION_NEGOTIATION_COUNT,
                 bounded_float(state->version_negotiation_count)) !=
          KTAG_SUCCESS ||
      set_scalar(output, INITIAL_COUNT, bounded_float(state->initial_count)) !=
          KTAG_SUCCESS ||
      set_scalar(output, ZERO_RTT_COUNT,
                 bounded_float(state->zero_rtt_count)) != KTAG_SUCCESS ||
      set_scalar(output, HANDSHAKE_COUNT,
                 bounded_float(state->handshake_count)) != KTAG_SUCCESS ||
      set_scalar(output, RETRY_COUNT, bounded_float(state->retry_count)) !=
          KTAG_SUCCESS ||
      set_scalar(output, FIRST_INITIAL_LENGTH_LOG2,
                 state->first_initial_length_log2) != KTAG_SUCCESS ||
      set_scalar(output, FIRST_DCID_LENGTH, state->first_dcid_length) !=
          KTAG_SUCCESS ||
      set_scalar(output, FIRST_SCID_LENGTH, state->first_scid_length) !=
          KTAG_SUCCESS ||
      set_scalar(output, FIRST_TOKEN_LENGTH_LOG2,
                 state->first_token_length_log2) != KTAG_SUCCESS ||
      set_optional(
          output, SIGNED_DATAGRAM_LENGTH, state->signed_datagram_length.data(),
          static_cast<uint32_t>(state->signed_datagram_length.size())) !=
          KTAG_SUCCESS ||
      set_optional(output, HEADER_TYPE_SEQUENCE,
                   state->header_type_sequence.data(),
                   static_cast<uint32_t>(state->header_type_sequence.size())) !=
          KTAG_SUCCESS ||
      set_scalar(output, SEQUENCE_VALID_COUNT,
                 bounded_float(state->sequence_count)) != KTAG_SUCCESS ||
      set_optional(output, FIRST_BYTE_HIGH_NIBBLE_HISTOGRAM, histogram.data(),
                   static_cast<uint32_t>(histogram.size())) != KTAG_SUCCESS ||
      set_scalar(output, PAYLOAD_ENTROPY_MEAN, entropy_mean) != KTAG_SUCCESS) {
    return KTAG_ERROR_INTERNAL;
  }
  return KTAG_SUCCESS;
}

void quic_flow_destroy(void *, void *flow_state) {
  delete static_cast<quic_flow_state *>(flow_state);
}

} // namespace

ktag_status register_quic_provider(ktag_feature_registry *registry) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.flow_create = quic_flow_create;
  callbacks.on_packet = quic_on_packet;
  callbacks.flow_finish = quic_flow_finish;
  callbacks.flow_destroy = quic_flow_destroy;
  return register_provider(registry, KTAG_PROXY_QUIC_CATALOG, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace ktag_proxy_features
