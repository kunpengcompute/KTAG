/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "traffic_statistics_provider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <vector>

#include "traffic_feature_catalog.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

enum class slot_kind { counters, header, payload, interval };

struct slot_config {
  const provider_catalog *catalog;
  ktag_direction direction;
  slot_kind kind;
};

struct slot_flow_state {
  uint64_t total_bytes = 0U;
  uint32_t packet_count = 0U;
  std::vector<uint32_t> samples;
  ktag_timestamp previous{};
  bool has_previous = false;
};

struct statistics_result {
  double minimum = 0.0;
  double maximum = 0.0;
  double mean = 0.0;
  double variance = 0.0;
  std::array<double, KTAG_LEGACY_HISTOGRAM_DIMENSION> relative{};
  std::array<double, KTAG_LEGACY_HISTOGRAM_DIMENSION> absolute{};
  std::array<double, KTAG_LEGACY_HISTOGRAM_DIMENSION> normalized{};
};

const slot_config SEND_COUNTERS{&KTAG_SEND_COUNTERS_CATALOG,
                                KTAG_DIRECTION_FORWARD, slot_kind::counters};
const slot_config SEND_HEADER{&KTAG_SEND_HEADER_CATALOG,
                              KTAG_DIRECTION_FORWARD, slot_kind::header};
const slot_config SEND_PAYLOAD{&KTAG_SEND_PAYLOAD_CATALOG,
                               KTAG_DIRECTION_FORWARD, slot_kind::payload};
const slot_config SEND_INTERVAL{&KTAG_SEND_INTERVAL_CATALOG,
                                KTAG_DIRECTION_FORWARD, slot_kind::interval};
const slot_config RECEIVE_COUNTERS{&KTAG_RECEIVE_COUNTERS_CATALOG,
                                   KTAG_DIRECTION_REVERSE,
                                   slot_kind::counters};
const slot_config RECEIVE_HEADER{&KTAG_RECEIVE_HEADER_CATALOG,
                                 KTAG_DIRECTION_REVERSE, slot_kind::header};
const slot_config RECEIVE_PAYLOAD{&KTAG_RECEIVE_PAYLOAD_CATALOG,
                                  KTAG_DIRECTION_REVERSE, slot_kind::payload};
const slot_config RECEIVE_INTERVAL{&KTAG_RECEIVE_INTERVAL_CATALOG,
                                   KTAG_DIRECTION_REVERSE,
                                   slot_kind::interval};

ktag_status provider_create(const ktag_feature_provider_config *config,
                            void **provider_state) {
  if (config == nullptr || provider_state == nullptr ||
      config->user_data == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *provider_state = config->user_data;
  return KTAG_SUCCESS;
}

ktag_status flow_create(void *, const ktag_flow_view *, void **flow_state) {
  if (flow_state == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *flow_state = new (std::nothrow) slot_flow_state();
  return *flow_state == nullptr ? KTAG_ERROR_OUT_OF_MEMORY : KTAG_SUCCESS;
}

ktag_status on_packet(void *provider_state, void *flow_state,
                      const ktag_packet_view *packet,
                      const ktag_flow_view *) {
  const auto *config = static_cast<const slot_config *>(provider_state);
  auto *state = static_cast<slot_flow_state *>(flow_state);
  if (config == nullptr || state == nullptr || packet == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  ktag_packet_transport_view transport{};
  transport.struct_size = sizeof(transport);
  transport.api_version = KTAG_API_VERSION;
  uint32_t packet_length = 0U;
  uint32_t header_length = 0U;
  uint32_t payload_length = 0U;
  if (ktag_packet_view_get_transport(packet, &transport) != KTAG_SUCCESS ||
      ktag_packet_view_get_capture_lengths(packet, &packet_length,
                                           &header_length,
                                           &payload_length) != KTAG_SUCCESS) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (transport.direction != config->direction) {
    return KTAG_SUCCESS;
  }
  if (config->kind == slot_kind::counters) {
    state->total_bytes += packet_length;
    ++state->packet_count;
    return KTAG_SUCCESS;
  }
  if (config->kind == slot_kind::header) {
    state->samples.push_back(header_length);
    return KTAG_SUCCESS;
  }
  if (config->kind == slot_kind::payload) {
    state->samples.push_back(payload_length);
    return KTAG_SUCCESS;
  }

  ktag_timestamp current{};
  if (ktag_packet_view_get_timestamp(packet, &current) != KTAG_SUCCESS) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (state->has_previous) {
    uint32_t difference = 0U;
    if (current.microseconds <= 999999 &&
        state->previous.microseconds <= 999999) {
      const int64_t current_us =
          current.seconds * 1000000LL + current.microseconds;
      const int64_t previous_us =
          state->previous.seconds * 1000000LL + state->previous.microseconds;
      if (current_us >= previous_us) {
        difference = static_cast<uint32_t>(current_us - previous_us);
      }
    }
    state->samples.push_back(difference);
  }
  state->previous = current;
  state->has_previous = true;
  return KTAG_SUCCESS;
}

void fill_histograms(const std::vector<uint32_t> &samples, bool logarithmic,
                     statistics_result *result) {
  constexpr double EPSILON = 1e-5;
  const double range = result->maximum - result->minimum;
  const double absolute_width =
      result->maximum / KTAG_LEGACY_HISTOGRAM_DIMENSION;
  for (const uint32_t raw : samples) {
    const double sample = logarithmic ? std::log2(raw + 1.0) : raw;
    size_t relative = 0U;
    if (range >= EPSILON) {
      relative = static_cast<size_t>(std::clamp(
          static_cast<int>(std::floor((sample - result->minimum) / range *
                                      KTAG_LEGACY_HISTOGRAM_DIMENSION)),
          0, static_cast<int>(KTAG_LEGACY_HISTOGRAM_DIMENSION - 1U)));
    }
    ++result->relative[relative];
    size_t absolute = 0U;
    if (absolute_width >= EPSILON) {
      absolute = static_cast<size_t>(std::clamp(
          static_cast<int>(std::floor(sample / absolute_width)), 0,
          static_cast<int>(KTAG_LEGACY_HISTOGRAM_DIMENSION - 1U)));
    }
    ++result->absolute[absolute];
    size_t normalized = 0U;
    if (result->variance >= EPSILON) {
      normalized = static_cast<size_t>(std::clamp(
          static_cast<int>(std::floor((sample - result->mean) /
                                      result->variance *
                                      KTAG_LEGACY_HISTOGRAM_DIMENSION)),
          0, static_cast<int>(KTAG_LEGACY_HISTOGRAM_DIMENSION - 1U)));
    }
    ++result->normalized[normalized];
  }
}

statistics_result calculate(const std::vector<uint32_t> &samples,
                            bool logarithmic) {
  statistics_result result{};
  if (samples.empty()) {
    return result;
  }
  result.minimum = samples.front();
  result.maximum = samples.front();
  double sum = 0.0;
  double squares = 0.0;
  for (const uint32_t sample : samples) {
    result.minimum = std::min(result.minimum, static_cast<double>(sample));
    result.maximum = std::max(result.maximum, static_cast<double>(sample));
    sum += sample;
    squares += static_cast<double>(sample) * sample;
  }
  result.mean = sum / samples.size();
  if (samples.size() > 1U) {
    result.variance =
        (squares - sum * sum / samples.size()) / (samples.size() - 1U);
  }
  if (logarithmic) {
    result.minimum = std::log2(result.minimum + 1.0);
    result.maximum = std::log2(result.maximum + 1.0);
    result.mean = std::log2(result.mean + 1.0);
    result.variance = std::log2(result.variance + 1.0);
  }
  fill_histograms(samples, logarithmic, &result);
  return result;
}

ktag_status write_statistics(const provider_catalog &catalog,
                             const statistics_result &result,
                             ktag_feature_output *output) {
  if (catalog.feature_count != 7U) {
    return KTAG_ERROR_INVALID_STATE;
  }
  if (set_scalar(output, catalog.features[0U], result.minimum) != KTAG_SUCCESS ||
      set_scalar(output, catalog.features[1U], result.maximum) != KTAG_SUCCESS ||
      set_scalar(output, catalog.features[2U], result.mean) != KTAG_SUCCESS ||
      set_scalar(output, catalog.features[3U], result.variance) != KTAG_SUCCESS) {
    return KTAG_ERROR_INTERNAL;
  }
  const std::array<const std::array<double, KTAG_LEGACY_HISTOGRAM_DIMENSION> *,
                   3U>
      sources{{&result.relative, &result.absolute, &result.normalized}};
  for (size_t family = 0U; family < sources.size(); ++family) {
    std::array<float, KTAG_LEGACY_HISTOGRAM_DIMENSION> values{};
    std::transform(sources[family]->cbegin(), sources[family]->cend(),
                   values.begin(), [](double value) {
                     return static_cast<float>(value);
                   });
    if (set_optional(output, catalog.features[4U + family], values.data(),
                     static_cast<uint32_t>(values.size())) != KTAG_SUCCESS) {
      return KTAG_ERROR_INTERNAL;
    }
  }
  return KTAG_SUCCESS;
}

ktag_status flow_finish(void *provider_state, void *flow_state,
                        const ktag_flow_view *, ktag_feature_output *output) {
  const auto *config = static_cast<const slot_config *>(provider_state);
  const auto *state = static_cast<const slot_flow_state *>(flow_state);
  if (config == nullptr || config->catalog == nullptr || state == nullptr ||
      output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (config->kind == slot_kind::counters) {
    return set_scalar(output, config->catalog->features[0U],
                      state->total_bytes) == KTAG_SUCCESS &&
                   set_scalar(output, config->catalog->features[1U],
                              state->packet_count) == KTAG_SUCCESS
               ? KTAG_SUCCESS
               : KTAG_ERROR_INTERNAL;
  }
  return write_statistics(*config->catalog,
                          calculate(state->samples,
                                    config->kind == slot_kind::interval),
                          output);
}

void flow_destroy(void *, void *flow_state) {
  delete static_cast<slot_flow_state *>(flow_state);
}

ktag_status register_slot(ktag_feature_registry *registry,
                          const slot_config &config) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.provider_create = provider_create;
  callbacks.flow_create = flow_create;
  callbacks.on_packet = on_packet;
  callbacks.flow_finish = flow_finish;
  callbacks.flow_destroy = flow_destroy;
  return register_provider(registry, *config.catalog, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW,
                           const_cast<slot_config *>(&config));
}

} // namespace

ktag_status register_send_counters_provider(ktag_feature_registry *registry) {
  return register_slot(registry, SEND_COUNTERS);
}
ktag_status register_send_header_provider(ktag_feature_registry *registry) {
  return register_slot(registry, SEND_HEADER);
}
ktag_status register_send_payload_provider(ktag_feature_registry *registry) {
  return register_slot(registry, SEND_PAYLOAD);
}
ktag_status register_send_interval_provider(ktag_feature_registry *registry) {
  return register_slot(registry, SEND_INTERVAL);
}
ktag_status register_receive_counters_provider(ktag_feature_registry *registry) {
  return register_slot(registry, RECEIVE_COUNTERS);
}
ktag_status register_receive_header_provider(ktag_feature_registry *registry) {
  return register_slot(registry, RECEIVE_HEADER);
}
ktag_status register_receive_payload_provider(ktag_feature_registry *registry) {
  return register_slot(registry, RECEIVE_PAYLOAD);
}
ktag_status register_receive_interval_provider(ktag_feature_registry *registry) {
  return register_slot(registry, RECEIVE_INTERVAL);
}

} // namespace ktag_traffic_features
