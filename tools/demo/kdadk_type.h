/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#ifndef KDADK_TYPE_H
#define KDADK_TYPE_H

#include "kdadk_feature_extract.h"
#include "report_types.h"

// 过滤后的特征数据（保留原始特征，只过滤行）
typedef struct {
    feature_vector *vectors;  // 过滤后保留的特征向量（原始特征）
    uint32_t count;           // 过滤后的数量
    uint32_t capacity;        // 容量
    uint32_t filtered_count;  // 被过滤掉的数量
} filtered_features;

#endif
