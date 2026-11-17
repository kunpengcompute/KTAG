/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "proxy_math.h"

#include <array>
#include <cmath>
#include <limits>

namespace ktag_proxy_features {
namespace {

bool timestamp_before(const ktag_timestamp &left, const ktag_timestamp &right) {
  return left.seconds < right.seconds ||
         (left.seconds == right.seconds &&
          left.microseconds < right.microseconds);
}

} // namespace

uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
  return right > std::numeric_limits<uint64_t>::max() - left
             ? std::numeric_limits<uint64_t>::max()
             : left + right;
}

uint64_t timestamp_delta_us(const ktag_timestamp &first,
                            const ktag_timestamp &last) {
  if (timestamp_before(last, first)) {
    return 0U;
  }

  const uint64_t seconds = static_cast<uint64_t>(last.seconds) -
                           static_cast<uint64_t>(first.seconds);
  constexpr uint64_t MICROSECONDS_PER_SECOND = 1000000U;
  uint64_t result =
      seconds > std::numeric_limits<uint64_t>::max() / MICROSECONDS_PER_SECOND
          ? std::numeric_limits<uint64_t>::max()
          : seconds * MICROSECONDS_PER_SECOND;

  const int64_t microseconds = static_cast<int64_t>(last.microseconds) -
                               static_cast<int64_t>(first.microseconds);
  if (microseconds >= 0) {
    return saturating_add_u64(result, static_cast<uint64_t>(microseconds));
  }
  const uint64_t adjustment = static_cast<uint64_t>(-microseconds);
  return adjustment > result ? 0U : result - adjustment;
}

float bounded_float(uint64_t value) {
  const long double converted = static_cast<long double>(value);
  const long double maximum =
      static_cast<long double>(std::numeric_limits<float>::max());
  return static_cast<float>(converted > maximum ? maximum : converted);
}

float signed_bounded_float(uint64_t value, ktag_direction direction) {
  const float converted = bounded_float(value);
  return value != 0U && direction == KTAG_DIRECTION_REVERSE ? -converted
                                                            : converted;
}

float log2_1p_u64(uint64_t value) {
  return static_cast<float>(std::log2(1.0 + static_cast<double>(value)));
}

float signed_log2_1p_u64(uint64_t value, ktag_direction direction) {
  const float converted = log2_1p_u64(value);
  return value != 0U && direction == KTAG_DIRECTION_REVERSE ? -converted
                                                            : converted;
}

float byte_entropy(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0U) {
    return 0.0F;
  }
  std::array<uint32_t, 256U> counts{};
  for (size_t index = 0U; index < length; ++index) {
    ++counts[data[index]];
  }
  return byte_entropy_from_counts(counts, length);
}

float byte_entropy_from_counts(const std::array<uint32_t, 256U> &counts,
                               size_t length) {
  if (length == 0U) {
    return 0.0F;
  }
  double entropy = 0.0;
  for (uint32_t count : counts) {
    if (count == 0U) {
      continue;
    }
    const double probability =
        static_cast<double>(count) / static_cast<double>(length);
    entropy -= probability * std::log2(probability);
  }
  return static_cast<float>(entropy);
}

} // namespace ktag_proxy_features
