/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_sequence_provider.h"

#include <array>
#include <new>

#include "proxy_feature_catalog.h"
#include "proxy_math.h"
#include "proxy_provider_common.h"

namespace ktag_proxy_features {
namespace {

constexpr uint32_t SEQUENCE_CAPACITY = 32U;
constexpr uint32_t BURST_CAPACITY = 16U;
constexpr uint8_t TCP_PROTOCOL = 6U;

enum sequence_feature_index : size_t {
  SIGNED_PAYLOAD_LENGTH = 0U,
  RELATIVE_TIME_LOG2_US,
  TCP_FLAGS,
  SEQUENCE_VALID_COUNT,
  BURST_SIGNED_PACKET_COUNT,
  BURST_SIGNED_PAYLOAD_BYTES_LOG2,
  BURST_DURATION_LOG2_US,
  BURST_VALID_COUNT,
  DIRECTION_SWITCH_COUNT,
  FORWARD_PAYLOAD_RATIO,
};

static_assert(KTAG_PROXY_SEQUENCE_FEATURES[SIGNED_PAYLOAD_LENGTH].dimension ==
              SEQUENCE_CAPACITY);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[RELATIVE_TIME_LOG2_US].dimension ==
              SEQUENCE_CAPACITY);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[TCP_FLAGS].dimension ==
              SEQUENCE_CAPACITY);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[SEQUENCE_VALID_COUNT].dimension ==
              1U);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[BURST_SIGNED_PACKET_COUNT]
                  .dimension == BURST_CAPACITY);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[BURST_SIGNED_PAYLOAD_BYTES_LOG2]
                  .dimension == BURST_CAPACITY);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[BURST_DURATION_LOG2_US].dimension ==
              BURST_CAPACITY);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[BURST_VALID_COUNT].dimension == 1U);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[DIRECTION_SWITCH_COUNT].dimension ==
              1U);
static_assert(KTAG_PROXY_SEQUENCE_FEATURES[FORWARD_PAYLOAD_RATIO].dimension ==
              1U);

struct burst_state {
  ktag_direction direction = KTAG_DIRECTION_FORWARD;
  uint64_t packet_count = 0U;
  uint64_t payload_bytes = 0U;
  ktag_timestamp first_timestamp{};
  ktag_timestamp last_timestamp{};
};

struct sequence_flow_state {
  bool has_first_packet = false;
  bool has_last_sequence_direction = false;
  bool has_last_packet_direction = false;
  bool burst_storage_complete = false;
  ktag_timestamp first_timestamp{};
  ktag_direction last_sequence_direction = KTAG_DIRECTION_FORWARD;
  ktag_direction last_packet_direction = KTAG_DIRECTION_FORWARD;
  std::array<float, SEQUENCE_CAPACITY> signed_payload_length{};
  std::array<float, SEQUENCE_CAPACITY> relative_time_log2_us{};
  std::array<float, SEQUENCE_CAPACITY> tcp_flags{};
  std::array<burst_state, BURST_CAPACITY> bursts{};
  uint32_t sequence_count = 0U;
  uint32_t burst_count = 0U;
  uint64_t direction_switch_count = 0U;
  uint64_t forward_payload_bytes = 0U;
  uint64_t reverse_payload_bytes = 0U;
};

ktag_status set_optional(ktag_feature_output *output,
                         sequence_feature_index feature, const float *values,
                         uint32_t value_count) {
  const ktag_status status = ktag_feature_output_set(
      output, KTAG_PROXY_SEQUENCE_FEATURES[feature].feature_id, values,
      value_count);
  return status == KTAG_ERROR_NOT_FOUND ? KTAG_SUCCESS : status;
}

ktag_status set_scalar(ktag_feature_output *output,
                       sequence_feature_index feature, float value) {
  return set_optional(output, feature, &value, 1U);
}

void start_burst(sequence_flow_state *state, ktag_direction direction,
                 uint32_t payload_length, const ktag_timestamp &timestamp) {
  auto &burst = state->bursts[state->burst_count++];
  burst.direction = direction;
  burst.packet_count = 1U;
  burst.payload_bytes = payload_length;
  burst.first_timestamp = timestamp;
  burst.last_timestamp = timestamp;
}

void update_bursts(sequence_flow_state *state, ktag_direction direction,
                   uint32_t payload_length, const ktag_timestamp &timestamp) {
  if (!state->has_last_packet_direction) {
    start_burst(state, direction, payload_length, timestamp);
    state->has_last_packet_direction = true;
    state->last_packet_direction = direction;
    return;
  }

  if (direction != state->last_packet_direction) {
    state->last_packet_direction = direction;
    if (state->burst_count < BURST_CAPACITY) {
      start_burst(state, direction, payload_length, timestamp);
    } else {
      state->burst_storage_complete = true;
    }
    return;
  }

  if (state->burst_storage_complete || state->burst_count == 0U) {
    return;
  }
  auto &burst = state->bursts[state->burst_count - 1U];
  burst.packet_count = saturating_add_u64(burst.packet_count, 1U);
  burst.payload_bytes = saturating_add_u64(burst.payload_bytes, payload_length);
  burst.last_timestamp = timestamp;
}

ktag_status sequence_flow_create(void *, const ktag_flow_view *,
                                 void **flow_state) {
  if (flow_state == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  auto *state = new (std::nothrow) sequence_flow_state();
  if (state == nullptr) {
    return KTAG_ERROR_OUT_OF_MEMORY;
  }
  *flow_state = state;
  return KTAG_SUCCESS;
}

ktag_status sequence_on_packet(void *, void *flow_state,
                               const ktag_packet_view *packet,
                               const ktag_flow_view *) {
  auto *state = static_cast<sequence_flow_state *>(flow_state);
  if (state == nullptr || packet == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  ktag_packet_transport_view transport{};
  transport.struct_size = sizeof(transport);
  transport.api_version = KTAG_API_VERSION;
  ktag_timestamp timestamp{};
  uint32_t payload_length = 0U;
  const uint8_t *payload =
      ktag_packet_view_get_payload(packet, &payload_length);
  if (ktag_packet_view_get_transport(packet, &transport) != KTAG_SUCCESS ||
      ktag_packet_view_get_timestamp(packet, &timestamp) != KTAG_SUCCESS ||
      (payload == nullptr && payload_length != 0U) ||
      (transport.direction != KTAG_DIRECTION_FORWARD &&
       transport.direction != KTAG_DIRECTION_REVERSE)) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  if (!state->has_first_packet) {
    state->has_first_packet = true;
    state->first_timestamp = timestamp;
  }

  if (transport.direction == KTAG_DIRECTION_FORWARD) {
    state->forward_payload_bytes =
        saturating_add_u64(state->forward_payload_bytes, payload_length);
  } else {
    state->reverse_payload_bytes =
        saturating_add_u64(state->reverse_payload_bytes, payload_length);
  }

  if (state->sequence_count < SEQUENCE_CAPACITY) {
    const uint32_t index = state->sequence_count++;
    state->signed_payload_length[index] =
        signed_bounded_float(payload_length, transport.direction);
    state->relative_time_log2_us[index] =
        log2_1p_u64(timestamp_delta_us(state->first_timestamp, timestamp));
    state->tcp_flags[index] =
        transport.transport_protocol == TCP_PROTOCOL
            ? signed_bounded_float(transport.tcp_flags, transport.direction)
            : 0.0F;
    if (state->has_last_sequence_direction &&
        transport.direction != state->last_sequence_direction) {
      state->direction_switch_count =
          saturating_add_u64(state->direction_switch_count, 1U);
    }
    state->has_last_sequence_direction = true;
    state->last_sequence_direction = transport.direction;
  }

  update_bursts(state, transport.direction, payload_length, timestamp);
  return KTAG_SUCCESS;
}

ktag_status sequence_flow_finish(void *, void *flow_state,
                                 const ktag_flow_view *,
                                 ktag_feature_output *output) {
  const auto *state = static_cast<const sequence_flow_state *>(flow_state);
  if (state == nullptr || output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  std::array<float, BURST_CAPACITY> signed_packet_count{};
  std::array<float, BURST_CAPACITY> signed_payload_bytes_log2{};
  std::array<float, BURST_CAPACITY> duration_log2_us{};
  for (uint32_t index = 0U; index < state->burst_count; ++index) {
    const auto &burst = state->bursts[index];
    signed_packet_count[index] =
        signed_bounded_float(burst.packet_count, burst.direction);
    signed_payload_bytes_log2[index] =
        signed_log2_1p_u64(burst.payload_bytes, burst.direction);
    duration_log2_us[index] = log2_1p_u64(
        timestamp_delta_us(burst.first_timestamp, burst.last_timestamp));
  }

  const uint64_t total_payload_bytes = saturating_add_u64(
      state->forward_payload_bytes, state->reverse_payload_bytes);
  const float forward_payload_ratio =
      total_payload_bytes == 0U
          ? 0.0F
          : static_cast<float>(
                static_cast<double>(state->forward_payload_bytes) /
                static_cast<double>(total_payload_bytes));

  if (set_optional(output, SIGNED_PAYLOAD_LENGTH,
                   state->signed_payload_length.data(),
                   state->signed_payload_length.size()) != KTAG_SUCCESS ||
      set_optional(output, RELATIVE_TIME_LOG2_US,
                   state->relative_time_log2_us.data(),
                   state->relative_time_log2_us.size()) != KTAG_SUCCESS ||
      set_optional(output, TCP_FLAGS, state->tcp_flags.data(),
                   state->tcp_flags.size()) != KTAG_SUCCESS ||
      set_scalar(output, SEQUENCE_VALID_COUNT,
                 bounded_float(state->sequence_count)) != KTAG_SUCCESS ||
      set_optional(output, BURST_SIGNED_PACKET_COUNT,
                   signed_packet_count.data(),
                   signed_packet_count.size()) != KTAG_SUCCESS ||
      set_optional(output, BURST_SIGNED_PAYLOAD_BYTES_LOG2,
                   signed_payload_bytes_log2.data(),
                   signed_payload_bytes_log2.size()) != KTAG_SUCCESS ||
      set_optional(output, BURST_DURATION_LOG2_US, duration_log2_us.data(),
                   duration_log2_us.size()) != KTAG_SUCCESS ||
      set_scalar(output, BURST_VALID_COUNT,
                 bounded_float(state->burst_count)) != KTAG_SUCCESS ||
      set_scalar(output, DIRECTION_SWITCH_COUNT,
                 bounded_float(state->direction_switch_count)) !=
          KTAG_SUCCESS ||
      set_scalar(output, FORWARD_PAYLOAD_RATIO, forward_payload_ratio) !=
          KTAG_SUCCESS) {
    return KTAG_ERROR_INTERNAL;
  }
  return KTAG_SUCCESS;
}

void sequence_flow_destroy(void *, void *flow_state) {
  delete static_cast<sequence_flow_state *>(flow_state);
}

} // namespace

ktag_status register_sequence_provider(ktag_feature_registry *registry) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.flow_create = sequence_flow_create;
  callbacks.on_packet = sequence_on_packet;
  callbacks.flow_finish = sequence_flow_finish;
  callbacks.flow_destroy = sequence_flow_destroy;
  return register_provider(registry, KTAG_PROXY_SEQUENCE_CATALOG, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace ktag_proxy_features
