/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
/* Data-only bridge for historical Demo file formats.
 * Feature extraction uses ktag_feature.h and plugin Providers exclusively. */
#ifndef KTAG_LEGACY_TYPES_H
#define KTAG_LEGACY_TYPES_H
#include <stdint.h>
#define FEATURES_NUM 355U
#define RAWBOW_MAX_LEN 4096U
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { float features[FEATURES_NUM]; } feature_vector;
typedef struct { feature_vector *vectors; uint32_t count, capacity; } feature_vector_list;
typedef struct { char rawbow[RAWBOW_MAX_LEN]; uint32_t len; } rawbow;
typedef struct { rawbow *rawbows; uint32_t count, capacity; } rawbow_list;
#ifdef __cplusplus
}
#endif
#endif
