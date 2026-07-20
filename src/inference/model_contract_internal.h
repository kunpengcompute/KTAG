/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_MODEL_CONTRACT_INTERNAL_H
#define KTAG_MODEL_CONTRACT_INTERNAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "ktag_inference.h"

enum class ktag_missing_feature_policy {
    reject,
    fill_zero
};

struct ktag_model_input_segment {
    std::string feature_id;
    uint32_t source_offset = 0U;
    uint32_t dimension = 0U;
    bool missing = false;
};

struct ktag_model_contract {
    std::string model_name;
    std::string model_version;
    std::string model_path;
    std::string scaler_path;
    std::string onnx_log_level;
    uint64_t schema_hash = 0U;
    uint64_t minimum_total_packets = 0U;
    uint32_t input_value_count = 0U;
    ktag_missing_feature_policy missing_policy =
        ktag_missing_feature_policy::reject;
    std::vector<std::string> input_feature_ids;
    std::vector<ktag_model_input_segment> input_segments;
    std::vector<uint32_t> packet_count_offsets;
    std::vector<double> scaler_mean;
    std::vector<double> scaler_scale;
};

ktag_status ktag_model_contract_load_internal(
    const char *model_package_path,
    const ktag_feature_schema *schema,
    ktag_model_contract *contract,
    std::string *error);

ktag_status ktag_model_contract_prepare_input_internal(
    const ktag_model_contract &contract,
    const ktag_feature_batch *features,
    std::vector<float> *scaled_values,
    std::vector<uint32_t> *source_record_indexes,
    std::string *error);

#endif /* KTAG_MODEL_CONTRACT_INTERNAL_H */
