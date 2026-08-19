/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROXY_MATH_H
#define KTAG_PROXY_MATH_H

#include <cstddef>
#include <cstdint>

#include "ktag_feature.h"

namespace ktag_proxy_features {

uint64_t saturating_add_u64(uint64_t left, uint64_t right);

uint64_t timestamp_delta_us(const ktag_timestamp &first,
                            const ktag_timestamp &last);

float bounded_float(uint64_t value);

float signed_bounded_float(uint64_t value, ktag_direction direction);

float log2_1p_u64(uint64_t value);

float signed_log2_1p_u64(uint64_t value, ktag_direction direction);

float byte_entropy(const uint8_t *data, size_t length);

} // namespace ktag_proxy_features

#endif /* KTAG_PROXY_MATH_H */
