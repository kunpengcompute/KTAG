/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#ifndef KTAG_LEGACY_INFERENCE_H
#define KTAG_LEGACY_INFERENCE_H

#include <stdint.h>
#include "ktag_legacy_types.h"
#include "ktag_export.h"
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deprecated compatibility surface for config.yaml plus fixed feature_vector.
 * It remains available until the project owner explicitly authorizes removal.
 */
#define KTAG_LEGACY_INFERENCE_COMPAT_AVAILABLE 1

typedef struct standard_scaler standard_scaler;
typedef struct ort_session ort_session;

typedef struct {
    const char *config_file; /* 配置文件路径 */
} inference_config;

typedef struct {
    int *predictions; /* 预测标签数组 */
    uint32_t count;   /* 结果数量 */
} inference_result;

typedef struct {
    standard_scaler *scaler; /* 标准化器 */
    ort_session *session;    /* ONNX Runtime会话 */
    inference_config config; /* 配置信息 */
    int initialized;         /* 初始化标志 */
    char *model_path;
    char *scaler_path;
    int filter_packets;
    char *onnx_log_level;
    int *columns_to_remove;
    uint32_t columns_to_remove_count;
} inference_engine;

#define INFERENCE_SUCCESS 0
#define INFERENCE_ERROR -1

/*
 * LegacyModelAdapter API retained for one compatibility version. New code
 * should use ktag_inference.h and a Schema-bound model package.
 */
/* ========== 推理引擎接口 ========== */

/**
 * @brief 创建并初始化推理引擎
 * @param config 推理配置（包含配置文件路径）
 * @return 推理引擎句柄，失败返回NULL
 */
KTAG_API KTAG_DEPRECATED("Use ktag_inference_create")
inference_engine *inference_init(const inference_config *config);

/**
 * @brief 销毁推理引擎
 * @param engine 推理引擎句柄
 */
KTAG_API KTAG_DEPRECATED("Use ktag_inference_destroy")
void inference_destroy(inference_engine *engine);

/**
 * @brief 批量推理
 * @param engine 推理引擎句柄
 * @param features 特征向量列表
 * @param result 输出推理结果（调用者需要预先分配predictions数组）
 * @return INFERENCE_SUCCESS表示成功，INFERENCE_ERROR表示失败
 */
KTAG_API KTAG_DEPRECATED("Use ktag_inference_predict")
int inference_predict(inference_engine *engine, const feature_vector_list *features, inference_result *result);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_LEGACY_INFERENCE_H */
