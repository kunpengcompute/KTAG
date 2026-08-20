/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "traffic_statistics_provider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "traffic_feature_catalog.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

enum class sample_kind { header, payload, interval };

struct statistics_result {
  double minimum = 0.0;
  double maximum = 0.0;
  double mean = 0.0;
  double variance = 0.0;
  std::array<double, KTAG_LEGACY_HISTOGRAM_DIMENSION> relative{};
  std::array<double, KTAG_LEGACY_HISTOGRAM_DIMENSION> absolute{};
  std::array<double, KTAG_LEGACY_HISTOGRAM_DIMENSION> normalized{};
};

using flow_finish_callback = ktag_status (*)(
    void *, void *, const ktag_flow_view *, ktag_feature_output *);

/*
 * The seven descriptors in every statistics slot preserve the historical
 * order: minimum, maximum, mean, sample variance, then the relative,
 * absolute and normalized 16-bin histograms.  Keeping this layout local to a
 * replaceable slot lets a customer remove or replace one traffic family
 * without changing the remaining providers.
 */
ktag_status write_statistics(
    ktag_feature_output *output,
    const std::array<feature_definition, 7U> &features,
    const statistics_result &statistics) {
  std::array<float, KTAG_LEGACY_HISTOGRAM_DIMENSION> histogram{};
  const auto write_histogram =
      [output, &features, &histogram](size_t feature_index,
                                     const double *source) {
        for (size_t index = 0U; index < histogram.size(); ++index) {
          histogram[index] = static_cast<float>(source[index]);
        }
        return set_optional(output, features[feature_index], histogram.data(),
                            static_cast<uint32_t>(histogram.size()));
      };

  if (set_scalar(output, features[0U], statistics.minimum) != KTAG_SUCCESS ||
      set_scalar(output, features[1U], statistics.maximum) != KTAG_SUCCESS ||
      set_scalar(output, features[2U], statistics.mean) != KTAG_SUCCESS ||
      set_scalar(output, features[3U], statistics.variance) != KTAG_SUCCESS ||
      write_histogram(4U, statistics.relative.data()) != KTAG_SUCCESS ||
      write_histogram(5U, statistics.absolute.data()) != KTAG_SUCCESS ||
      write_histogram(6U, statistics.normalized.data()) != KTAG_SUCCESS) {
    return KTAG_ERROR_INTERNAL;
  }
  return KTAG_SUCCESS;
}

void fill_histograms(const uint32_t *samples,
                     size_t sample_count,
                     bool logarithmic,
                     statistics_result *result) {
  constexpr double EPSILON = 1e-5;
  const double range = result->maximum - result->minimum;
  const double absolute_width =
      result->maximum / KTAG_LEGACY_HISTOGRAM_DIMENSION;
  for (size_t index = 0U; index < sample_count; ++index) {
    const uint32_t raw_sample = samples[index];
    const double sample = logarithmic ? std::log2(raw_sample + 1.0)
                                      : static_cast<double>(raw_sample);
    size_t relative_bucket = 0U;
    if (range >= EPSILON) {
      const auto bucket = static_cast<int>(std::floor(
          (sample - result->minimum) / range *
          KTAG_LEGACY_HISTOGRAM_DIMENSION));
      relative_bucket = static_cast<size_t>(std::clamp(
          bucket, 0, static_cast<int>(KTAG_LEGACY_HISTOGRAM_DIMENSION - 1U)));
    }
    ++result->relative[relative_bucket];

    size_t absolute_bucket = 0U;
    if (absolute_width >= EPSILON) {
      const auto bucket =
          static_cast<int>(std::floor(sample / absolute_width));
      absolute_bucket = static_cast<size_t>(std::clamp(
          bucket, 0, static_cast<int>(KTAG_LEGACY_HISTOGRAM_DIMENSION - 1U)));
    }
    ++result->absolute[absolute_bucket];

    size_t normalized_bucket = 0U;
    if (result->variance >= EPSILON) {
      const auto bucket = static_cast<int>(std::floor(
          (sample - result->mean) / result->variance *
          KTAG_LEGACY_HISTOGRAM_DIMENSION));
      normalized_bucket = static_cast<size_t>(std::clamp(
          bucket, 0, static_cast<int>(KTAG_LEGACY_HISTOGRAM_DIMENSION - 1U)));
    }
    ++result->normalized[normalized_bucket];
  }
}

statistics_result calculate_statistics(const uint32_t *samples,
                                       size_t sample_count,
                                       bool logarithmic) {
  statistics_result result{};
  if (samples == nullptr || sample_count == 0U) {
    return result;
  }
  result.minimum = samples[0U];
  result.maximum = samples[0U];
  double sum = 0.0;
  double sum_of_squares = 0.0;
  for (size_t index = 0U; index < sample_count; ++index) {
    const uint32_t sample = samples[index];
    result.minimum = std::min(result.minimum, static_cast<double>(sample));
    result.maximum = std::max(result.maximum, static_cast<double>(sample));
    const double converted = sample;
    sum += converted;
    sum_of_squares += converted * converted;
  }
  result.mean = sum / sample_count;
  if (sample_count > 1U) {
    result.variance =
        (sum_of_squares - sum * sum / sample_count) /
        (sample_count - 1U);
  }
  if (logarithmic) {
    result.minimum = std::log2(result.minimum + 1.0);
    result.maximum = std::log2(result.maximum + 1.0);
    result.mean = std::log2(result.mean + 1.0);
    result.variance = std::log2(result.variance + 1.0);
  }
  fill_histograms(samples, sample_count, logarithmic, &result);
  return result;
}

statistics_result calculate_slot(const ktag_flow_view *flow,
                                 ktag_direction direction,
                                 sample_kind kind) {
  const uint32_t packet_count =
      ktag_flow_view_get_packet_count(flow, direction);
  if (kind == sample_kind::interval && packet_count <= 1U) {
    return {};
  }
  if (kind != sample_kind::interval) {
    const uint32_t *header_lengths = nullptr;
    const uint32_t *payload_lengths = nullptr;
    uint32_t sample_count = 0U;
    if (ktag_flow_view_get_length_samples(
            flow, direction, &header_lengths, &payload_lengths,
            &sample_count) != KTAG_SUCCESS) {
      return {};
    }
    return calculate_statistics(
        kind == sample_kind::header ? header_lengths : payload_lengths,
        sample_count, false);
  }

  std::vector<uint32_t> samples;
  samples.reserve(packet_count - 1U);
  ktag_flow_packet_sample previous{};
  for (uint32_t index = 0U; index < packet_count; ++index) {
    ktag_flow_packet_sample current{};
    if (ktag_flow_view_get_packet_sample(flow, direction, index, &current) !=
        KTAG_SUCCESS) {
      return {};
    }
    if (index > 0U) {
      uint32_t difference = 0U;
      if (current.timestamp.microseconds <= 999999 &&
          previous.timestamp.microseconds <= 999999) {
        const int64_t current_us =
            current.timestamp.seconds * 1000000LL +
            current.timestamp.microseconds;
        const int64_t previous_us =
            previous.timestamp.seconds * 1000000LL +
            previous.timestamp.microseconds;
        if (current_us >= previous_us) {
          difference = static_cast<uint32_t>(current_us - previous_us);
        }
      }
      samples.push_back(difference);
    }
    previous = current;
  }
  return calculate_statistics(samples.data(), samples.size(), true);
}

ktag_status counters_finish(
    const ktag_flow_view *flow,
    ktag_direction direction,
    const std::array<feature_definition, 2U> &features,
    ktag_feature_output *output) {
  if (flow == nullptr || output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (set_scalar(output, features[0U],
                 ktag_flow_view_get_total_bytes(flow, direction)) !=
          KTAG_SUCCESS ||
      set_scalar(output, features[1U],
                 ktag_flow_view_get_packet_count(flow, direction)) !=
          KTAG_SUCCESS) {
    return KTAG_ERROR_INTERNAL;
  }
  return KTAG_SUCCESS;
}

ktag_status statistics_finish(
    const ktag_flow_view *flow,
    ktag_direction direction,
    sample_kind kind,
    const std::array<feature_definition, 7U> &features,
    ktag_feature_output *output) {
  if (flow == nullptr || output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  return write_statistics(output, features,
                          calculate_slot(flow, direction, kind));
}

ktag_status send_counters_finish(void *, void *, const ktag_flow_view *flow,
                                 ktag_feature_output *output) {
  return counters_finish(flow, KTAG_DIRECTION_FORWARD,
                         KTAG_SEND_COUNTERS_FEATURES, output);
}

ktag_status send_header_finish(void *, void *, const ktag_flow_view *flow,
                               ktag_feature_output *output) {
  return statistics_finish(flow, KTAG_DIRECTION_FORWARD, sample_kind::header,
                           KTAG_SEND_HEADER_FEATURES, output);
}

ktag_status send_payload_finish(void *, void *, const ktag_flow_view *flow,
                                ktag_feature_output *output) {
  return statistics_finish(flow, KTAG_DIRECTION_FORWARD, sample_kind::payload,
                           KTAG_SEND_PAYLOAD_FEATURES, output);
}

ktag_status send_interval_finish(void *, void *, const ktag_flow_view *flow,
                                 ktag_feature_output *output) {
  return statistics_finish(flow, KTAG_DIRECTION_FORWARD,
                           sample_kind::interval,
                           KTAG_SEND_INTERVAL_FEATURES, output);
}

ktag_status receive_counters_finish(void *, void *, const ktag_flow_view *flow,
                                    ktag_feature_output *output) {
  return counters_finish(flow, KTAG_DIRECTION_REVERSE,
                         KTAG_RECEIVE_COUNTERS_FEATURES, output);
}

ktag_status receive_header_finish(void *, void *, const ktag_flow_view *flow,
                                  ktag_feature_output *output) {
  return statistics_finish(flow, KTAG_DIRECTION_REVERSE, sample_kind::header,
                           KTAG_RECEIVE_HEADER_FEATURES, output);
}

ktag_status receive_payload_finish(void *, void *, const ktag_flow_view *flow,
                                   ktag_feature_output *output) {
  return statistics_finish(flow, KTAG_DIRECTION_REVERSE, sample_kind::payload,
                           KTAG_RECEIVE_PAYLOAD_FEATURES, output);
}

ktag_status receive_interval_finish(void *, void *, const ktag_flow_view *flow,
                                    ktag_feature_output *output) {
  return statistics_finish(flow, KTAG_DIRECTION_REVERSE,
                           sample_kind::interval,
                           KTAG_RECEIVE_INTERVAL_FEATURES, output);
}

ktag_status register_finish_provider(
    ktag_feature_registry *registry,
    const provider_catalog &catalog,
    flow_finish_callback finish) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.flow_finish = finish;
  return register_provider(registry, catalog, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace

ktag_status register_send_counters_provider(ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_SEND_COUNTERS_CATALOG,
                                  send_counters_finish);
}

ktag_status register_send_header_provider(ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_SEND_HEADER_CATALOG,
                                  send_header_finish);
}

ktag_status register_send_payload_provider(ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_SEND_PAYLOAD_CATALOG,
                                  send_payload_finish);
}

ktag_status register_send_interval_provider(ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_SEND_INTERVAL_CATALOG,
                                  send_interval_finish);
}

ktag_status register_receive_counters_provider(
    ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_RECEIVE_COUNTERS_CATALOG,
                                  receive_counters_finish);
}

ktag_status register_receive_header_provider(ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_RECEIVE_HEADER_CATALOG,
                                  receive_header_finish);
}

ktag_status register_receive_payload_provider(
    ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_RECEIVE_PAYLOAD_CATALOG,
                                  receive_payload_finish);
}

ktag_status register_receive_interval_provider(
    ktag_feature_registry *registry) {
  return register_finish_provider(registry, KTAG_RECEIVE_INTERVAL_CATALOG,
                                  receive_interval_finish);
}

} // namespace ktag_traffic_features
