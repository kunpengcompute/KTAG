/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_BATCH_INTERNAL_H
#define KTAG_FEATURE_BATCH_INTERNAL_H

#include <string>
#include <unordered_map>

#include "ktag_feature.h"

ktag_status ktag_feature_batch_append_with_details_internal(
    ktag_feature_batch *batch, const ktag_feature_record_details *details,
    const float *values, uint32_t value_count,
    const std::unordered_map<std::string, std::string> *annotations = nullptr);

#endif /* KTAG_FEATURE_BATCH_INTERNAL_H */
