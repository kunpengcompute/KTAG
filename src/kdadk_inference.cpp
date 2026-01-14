/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "kdadk_inference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <stdexcept>

using namespace std;

struct standard_scaler {
    StandardScaler scaler;
};

struct ort_session {
    Ort::Session *session;
    Ort::Env *env;

    ~ort_session()
    {
        delete session;
        delete env;
    }
};

inference_engine *inference_init(const inference_config *config)
{
    if (!config || !config->config_file) {
        fprintf(stderr, "Error: Invalid config or config_file is NULL\n");
        return NULL;
    }

    inference_engine *engine = static_cast<inference_engine *>(malloc(sizeof(inference_engine)));
    if (!engine) {
        fprintf(stderr, "Error: Failed to allocate memory for inference_engine\n");
        return NULL;
    }

    memset(engine, 0, sizeof(inference_engine));

    try {
        // 1. 加载配置文件
        printf("Loading config file: %s\n", config->config_file);
        Config cfg = load_config(config->config_file);

        printf("model_path: %s\n", cfg.model_path.c_str());
        printf("scaler_path: %s\n", cfg.scaler_path.c_str());

        // 保存配置参数
        engine->model_path = strdup(cfg.model_path.c_str());
        engine->scaler_path = strdup(cfg.scaler_path.c_str());
        engine->filter_packets = cfg.filter_packets;
        engine->onnx_log_level = strdup(cfg.onnx_log_level.c_str());
        printf("filter_packets: %d\n", engine->filter_packets);
        engine->columns_to_remove_count = cfg.columns_to_remove.size();
        if (engine->columns_to_remove_count > 0) {
            engine->columns_to_remove = static_cast<int *>(malloc(sizeof(int) * engine->columns_to_remove_count));
            if (!engine->columns_to_remove) {
                throw runtime_error("Failed to allocate memory for columns_to_remove");
            }
            for (size_t i = 0; i < cfg.columns_to_remove.size(); i++) {
                engine->columns_to_remove[i] = cfg.columns_to_remove[i];
            }

            printf("columns_to_remove: [");
            for (uint32_t i = 0; i < engine->columns_to_remove_count; i++) {
                printf("%d%s", engine->columns_to_remove[i], i < engine->columns_to_remove_count - 1 ? ", " : "");
            }
            printf("]\n");
        }
        // 2. 加载ONNX模型
        printf("Loading ONNX model: %s\n", engine->model_path);
        engine->session = new ort_session();
        engine->session->env = new Ort::Env(get_onnx_log_level(engine->onnx_log_level), "Online_Inference");

        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(4);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

        engine->session->session = new Ort::Session(*engine->session->env, engine->model_path, session_options);

        // 3. 加载标准化器
        printf("Loading scaler: %s\n", engine->scaler_path);
        engine->scaler = new standard_scaler();
        if (!engine->scaler->scaler.load(engine->scaler_path)) {
            throw runtime_error("Failed to load scaler from: " + string(engine->scaler_path));
        }

        // 4. 保存配置
        engine->config.config_file = strdup(config->config_file);
        engine->initialized = 1;

        printf("Inference engine initialized successfully\n");
        return engine;

    } catch (const exception &e) {
        fprintf(stderr, "Error initializing inference engine: %s\n", e.what());

        // 清理已分配的资源
        if (engine->scaler)
            delete engine->scaler;
        if (engine->session)
            delete engine->session;
        free(engine->model_path);
        free(engine->scaler_path);
        free(engine);

        return NULL;
    }
}

int inference_predict(inference_engine *engine, const feature_vector_list *features, inference_result *result)
{
    if (!engine || !engine->initialized) {
        fprintf(stderr, "Error: Inference engine not initialized\n");
        return INFERENCE_ERROR;
    }

    if (!features || !features->vectors || features->count == 0) {
        fprintf(stderr, "Error: Invalid features\n");
        return INFERENCE_ERROR;
    }

    if (!result || !result->predictions) {
        fprintf(stderr, "Error: Invalid result or predictions array is NULL\n");
        return INFERENCE_ERROR;
    }

    try {
        vector<int> columns_to_remove;
        if (engine->columns_to_remove && engine->columns_to_remove_count > 0) {
            for (uint32_t i = 0; i < engine->columns_to_remove_count; i++) {
                columns_to_remove.push_back(engine->columns_to_remove[i]);
            }
        }

        int filter_packets = engine->filter_packets;

        vector<vector<float>> all_features;
        int filtered_rows_count = 0;  // 统计过滤掉的行数

        for (uint32_t j = 0; j < features->count; j++) {
            const feature_vector *fv = &features->vectors[j];

            if (fv->features[7] + fv->features[165] >= filter_packets) {
                vector<float> filtered_row;

                for (int k = 0; k < FEATURES_NUM - 1; k++) {
                    if (find(columns_to_remove.begin(), columns_to_remove.end(), k) == columns_to_remove.end()) {
                        filtered_row.push_back(fv->features[k]);
                    }
                }

                all_features.push_back(filtered_row);
            } else {
                filtered_rows_count++;  // 如果过滤条件不满足，增加过滤掉的行数统计
            }
        }

        if (all_features.empty()) {
            fprintf(stderr, "Warning: No data after filtering (filtered %d samples)\n", filtered_rows_count);
            result->count = 0;
            return INFERENCE_SUCCESS;
        }
        vector<vector<float>> scaled_features = engine->scaler->scaler.transform(all_features);

        vector<int> predictions = predict_with_onnx(*engine->session->session, scaled_features);

        result->count = predictions.size();

        for (size_t i = 0; i < predictions.size(); i++) {
            result->predictions[i] = predictions[i];
        }

        return INFERENCE_SUCCESS;

    } catch (const exception &e) {
        fprintf(stderr, "Error during inference: %s\n", e.what());
        return INFERENCE_ERROR;
    }
}

void inference_destroy(inference_engine *engine)
{
    if (!engine) {
        return;
    }

    if (engine->scaler) {
        delete engine->scaler;
    }

    if (engine->session) {
        delete engine->session;
    }

    free(engine->model_path);
    free(engine->scaler_path);
    free(engine->onnx_log_level);
    free(engine->columns_to_remove);
    free((void *)engine->config.config_file);

    free(engine);

    printf("Inference engine destroyed\n");
}