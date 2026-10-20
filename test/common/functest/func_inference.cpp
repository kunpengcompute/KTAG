/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include <gtest/gtest.h>
#include "ktag_feature.h"
#include "ktag_inference.h"
#include <limits>
#include <memory>

namespace {
using schema_ptr = std::unique_ptr<ktag_feature_schema, decltype(&ktag_feature_schema_destroy)>;
ktag_inference_config config_for(const ktag_feature_schema *schema) {
    ktag_inference_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.feature_schema = schema;
    config.model_package_path = "unused-by-invalid-configuration-tests";
    return config;
}
void expect_invalid_config(const ktag_inference_config *config) {
    EXPECT_EQ(nullptr, ktag_inference_create(config));
    EXPECT_STREQ("invalid Inference configuration", ktag_inference_get_last_error());
}
}

TEST(KTAGInferenceApi, RejectsMissingAndIncompatibleConfiguration) {
    expect_invalid_config(nullptr);
    schema_ptr schema(ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
    ASSERT_NE(nullptr, schema);
    auto config = config_for(schema.get());
    config.struct_size = 0U;
    expect_invalid_config(&config);
    config = config_for(schema.get());
    config.api_version = KTAG_API_VERSION + 1U;
    expect_invalid_config(&config);
    config = config_for(schema.get());
    config.model_package_path = nullptr;
    expect_invalid_config(&config);
    config.model_package_path = "";
    expect_invalid_config(&config);
}

TEST(KTAGInferenceApi, RequiresFrozenFeatureSchema) {
    auto config = config_for(nullptr);
    expect_invalid_config(&config);
    schema_ptr schema(ktag_feature_schema_create("unfrozen", 1U), ktag_feature_schema_destroy);
    ASSERT_NE(nullptr, schema);
    config = config_for(schema.get());
    expect_invalid_config(&config);
}

TEST(KTAGInferenceApi, RejectsThreadCountsOutsideOnnxIntegerRange) {
    schema_ptr schema(ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
    ASSERT_NE(nullptr, schema);
    const auto overflow = static_cast<uint32_t>(std::numeric_limits<int>::max()) + 1U;
    auto config = config_for(schema.get());
    config.intra_op_threads = overflow;
    expect_invalid_config(&config);
    config = config_for(schema.get());
    config.inter_op_threads = overflow;
    expect_invalid_config(&config);
}

TEST(KTAGInferenceApi, NullContextPredictionFailsWithDiagnostic) {
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_inference_predict(nullptr, nullptr, nullptr));
    EXPECT_STRNE("", ktag_inference_get_last_error());
    ktag_inference_destroy(nullptr);
}

TEST(KTAGPredictionBatchApi, EmptyBatchRemainsEmptyAfterReserveAndClear) {
    std::unique_ptr<ktag_prediction_batch, decltype(&ktag_prediction_batch_destroy)>
        batch(ktag_prediction_batch_create(0U), ktag_prediction_batch_destroy);
    ASSERT_NE(nullptr, batch);
    EXPECT_EQ(0U, ktag_prediction_batch_get_count(batch.get()));
    EXPECT_EQ(KTAG_SUCCESS, ktag_prediction_batch_reserve(batch.get(), 16U));
    EXPECT_EQ(0U, ktag_prediction_batch_get_count(batch.get()));
    ktag_prediction_record record{};
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_prediction_batch_get_record(batch.get(), 0U, &record));
    ktag_prediction_batch_clear(batch.get());
    EXPECT_EQ(0U, ktag_prediction_batch_get_count(batch.get()));
}

TEST(KTAGPredictionBatchApi, RejectsNullStorageWithoutDereferencingIt) {
    ktag_prediction_record record{};
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_prediction_batch_reserve(nullptr, 1U));
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_prediction_batch_get_record(nullptr, 0U, &record));
    EXPECT_EQ(0U, ktag_prediction_batch_get_count(nullptr));
    ktag_prediction_batch_clear(nullptr);
    ktag_prediction_batch_destroy(nullptr);
}
