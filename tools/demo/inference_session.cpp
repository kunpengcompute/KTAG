/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include "inference_session.h"

#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace {
uint32_t thread_count(const YAML::Node &node, const char *key) {
    if (!node || !node[key]) return 1U;
    const auto count = node[key].as<int64_t>();
    if (count <= 0 || count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string("pipeline.inference.") + key +
                                 " must be a positive uint32");
    }
    return static_cast<uint32_t>(count);
}

bool fail(std::string *error, const std::string &message) {
    if (error) *error = message;
    return false;
}
}

std::unique_ptr<ktag_demo_inference_session> ktag_demo_inference_session::create(
    const char *config_path, std::string *error) {
    if (error) error->clear();
    try {
        if (!config_path || !*config_path) {
            throw std::runtime_error("inference config path is required");
        }
        const auto config_file = std::filesystem::absolute(config_path);
        const auto yaml = YAML::LoadFile(config_file.string());
        const auto pipeline = yaml["pipeline"];
        if (!pipeline || !pipeline.IsMap() || !pipeline["model_package_path"] ||
            pipeline["model_package_path"].as<std::string>().empty()) {
            throw std::runtime_error(
                "pipeline.model_package_path is required, including in legacy mode; "
                "convert historical ONNX/scaler files with tools/model_package/create_legacy_package.py");
        }
        std::filesystem::path package(pipeline["model_package_path"].as<std::string>());
        if (package.is_relative()) package = config_file.parent_path() / package;
        const auto package_path = package.lexically_normal().string();
        std::unique_ptr<ktag_feature_schema, decltype(&ktag_feature_schema_destroy)>
            schema(ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
        std::unique_ptr<ktag_demo_inference_session> session(new ktag_demo_inference_session());
        if (!schema) throw std::runtime_error("cannot create frozen feature schema");
        session->features_ = ktag_feature_batch_create(schema.get(), 1U);
        session->predictions_ = ktag_prediction_batch_create(1U);
        if (!session->features_ || !session->predictions_) {
            throw std::runtime_error("cannot allocate inference batches");
        }
        ktag_inference_config config{};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.feature_schema = schema.get();
        config.model_package_path = package_path.c_str();
        config.intra_op_threads = thread_count(pipeline["inference"], "intra_op_threads");
        config.inter_op_threads = thread_count(pipeline["inference"], "inter_op_threads");
        session->context_ = ktag_inference_create(&config);
        if (!session->context_) throw std::runtime_error(ktag_inference_get_last_error());
        return session;
    } catch (const std::exception &exception) {
        fail(error, exception.what());
        return nullptr;
    }
}

ktag_demo_inference_session::~ktag_demo_inference_session() {
    ktag_inference_destroy(context_);
    ktag_prediction_batch_destroy(predictions_);
    ktag_feature_batch_destroy(features_);
}

bool ktag_demo_inference_session::predict(
    const feature_vector_list &features, std::vector<feature_vector> *accepted,
    std::vector<int> *labels, std::string *error, double *compute_seconds) {
    if (error) error->clear();
    if (accepted) accepted->clear();
    if (labels) labels->clear();
    if (!accepted || !labels || (features.count && !features.vectors)) {
        return fail(error, "invalid Demo inference input");
    }
    ktag_feature_batch_clear(features_);
    ktag_prediction_batch_clear(predictions_);
    try {
        for (uint32_t i = 0; i < features.count; ++i) {
            ktag_feature_record_metadata metadata{};
            metadata.struct_size = sizeof(metadata);
            metadata.api_version = KTAG_API_VERSION;
            // Historical CSV has no reliable lossless flow metadata. This ID
            // is local to this batch; output association uses source_record_index.
            metadata.flow_id = static_cast<uint64_t>(i) + 1U;
            if (ktag_feature_batch_append(features_, &metadata,
                    features.vectors[i].features, KTAG_LEGACY_V1_VALUE_COUNT) != KTAG_SUCCESS) {
                return fail(error, "cannot append Demo feature record");
            }
        }
        const auto compute_start = compute_seconds != nullptr
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const auto status = ktag_inference_predict(context_, features_, predictions_);
        if (compute_seconds != nullptr) {
            *compute_seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - compute_start).count();
        }
        if (status != KTAG_SUCCESS) {
            return fail(error, ktag_inference_get_last_error());
        }
        const auto count = ktag_prediction_batch_get_count(predictions_);
        std::vector<feature_vector> selected;
        std::vector<int> predicted;
        selected.reserve(count);
        predicted.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ktag_prediction_record record{};
            if (ktag_prediction_batch_get_record(predictions_, i, &record) != KTAG_SUCCESS ||
                record.source_record_index >= features.count) {
                return fail(error, "invalid prediction source record index");
            }
            if (record.label < std::numeric_limits<int>::min() ||
                record.label > std::numeric_limits<int>::max()) {
                return fail(error, "model label exceeds the historical Demo output range");
            }
            selected.push_back(features.vectors[record.source_record_index]);
            predicted.push_back(static_cast<int>(record.label));
        }
        accepted->swap(selected);
        labels->swap(predicted);
        return true;
    } catch (const std::exception &exception) {
        return fail(error, exception.what());
    }
}
