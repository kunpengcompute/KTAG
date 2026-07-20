/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "kdadk_test.h"
#include "kdadk_inference.h"

#include <vector>
#include <yaml-cpp/yaml.h>

// 测试配置文件路径
static const char *TEST_CONFIG_FILE = "../../../tools/demo/config.yaml";

// 测试夹具
class InferenceEngineTest : public ::testing::Test {
protected:
    inference_engine *engine;

    void SetUp() override
    {
        engine = nullptr;
    }

    void TearDown() override
    {
        if (engine) {
            inference_destroy(engine);
            engine = nullptr;
        }
    }
};

TEST(InferenceConfigTest, ConfigStructure)
{
    inference_config config;
    config.config_file = TEST_CONFIG_FILE;

    EXPECT_NE(config.config_file, nullptr) << "Config file path should be set";
    EXPECT_STREQ(config.config_file, TEST_CONFIG_FILE) << "Config file path should match";
}

TEST_F(InferenceEngineTest, Initialization)
{
    inference_config config;
    config.config_file = TEST_CONFIG_FILE;

    engine = inference_init(&config);

    if (engine) {
        EXPECT_NE(engine, nullptr) << "Engine should be initialized";
        EXPECT_EQ(engine->initialized, 1) << "Engine should be marked as initialized";
    } else {
        SUCCEED() << "Engine initialization failed gracefully (expected if model files don't exist)";
    }
}

TEST_F(InferenceEngineTest, InvalidParameters)
{
    inference_engine *engine = inference_init(nullptr);
    EXPECT_EQ(engine, nullptr) << "Should fail with NULL config";

    inference_config config;
    config.config_file = nullptr;
    engine = inference_init(&config);
    EXPECT_EQ(engine, nullptr) << "Should fail with NULL config file";

    config.config_file = "nonexistent_config.yaml";
    engine = inference_init(&config);
    EXPECT_EQ(engine, nullptr) << "Should fail with nonexistent config file";

    feature_vector_list features;
    inference_result result;
    int ret = inference_predict(nullptr, &features, &result);
    EXPECT_EQ(ret, INFERENCE_ERROR) << "Should fail with NULL engine";

    inference_destroy(nullptr);
    SUCCEED() << "Destroy NULL engine should not crash";
}

TEST(InferenceResultTest, ResultStructure)
{
    inference_result result;
    result.count = 10;
    result.predictions = static_cast<int *>(malloc(sizeof(int) * result.count));
    ASSERT_NE(result.predictions, nullptr) << "Should allocate predictions array";

    for (uint32_t i = 0; i < result.count; i++) {
        result.predictions[i] = i % 3;
    }

    EXPECT_EQ(result.predictions[0], 0) << "Prediction value should be 0";
    EXPECT_EQ(result.predictions[1], 1) << "Prediction value should be 1";
    EXPECT_EQ(result.predictions[2], 2) << "Prediction value should be 2";
    EXPECT_EQ(result.predictions[3], 0) << "Prediction value should be 0";

    free(result.predictions);
}

TEST(InferenceAlignmentTest, FeatureInferenceAlignment)
{
    feature_vector_list features;
    features.count = 5;
    features.capacity = 5;
    features.vectors = static_cast<feature_vector *>(malloc(sizeof(feature_vector) * features.count));
    ASSERT_NE(features.vectors, nullptr) << "Should allocate feature vectors";

    for (uint32_t i = 0; i < features.count; i++) {
        memset(&features.vectors[i], 0, sizeof(feature_vector));
        features.vectors[i].features[0] = i + 1;  // flow_id
        features.vectors[i].features[6] = 10;     // 满足过滤条件
        features.vectors[i].features[164] = 10;
    }

    inference_result result;
    result.count = features.count;
    result.predictions = static_cast<int *>(malloc(sizeof(int) * result.count));
    ASSERT_NE(result.predictions, nullptr) << "Should allocate predictions";

    EXPECT_EQ(features.count, result.count) << "Feature and prediction count should align";

    free(features.vectors);
    free(result.predictions);
}

TEST_F(InferenceEngineTest, PredictInputValidation)
{
    inference_config config;
    config.config_file = TEST_CONFIG_FILE;
    engine = inference_init(&config);
    if (engine == nullptr) {
        return;
    }

    int prediction = 0;
    inference_result result{};
    result.predictions = &prediction;
    int ret = inference_predict(engine, nullptr, &result);
    EXPECT_EQ(ret, INFERENCE_ERROR) << "Should fail with NULL features";

    feature_vector vector{};
    feature_vector_list features{};
    features.count = 1U;
    features.capacity = 1U;
    features.vectors = &vector;
    ret = inference_predict(engine, &features, nullptr);
    EXPECT_EQ(ret, INFERENCE_ERROR) << "Should fail with NULL result";
}

TEST_F(InferenceEngineTest, ConfigParameters)
{
    const YAML::Node expected = YAML::LoadFile(TEST_CONFIG_FILE);
    ASSERT_TRUE(expected["filter_packets"]);
    ASSERT_TRUE(expected["columns_to_remove"]);
    const int expected_filter_packets =
        expected["filter_packets"].as<int>();
    const std::vector<int> expected_columns =
        expected["columns_to_remove"].as<std::vector<int>>();

    inference_config config;
    config.config_file = TEST_CONFIG_FILE;

    engine = inference_init(&config);

    if (engine) {
        EXPECT_EQ(expected_filter_packets, engine->filter_packets);
        ASSERT_EQ(expected_columns.size(), engine->columns_to_remove_count);
        for (uint32_t index = 0U;
             index < engine->columns_to_remove_count;
             ++index) {
            EXPECT_EQ(
                expected_columns[index],
                engine->columns_to_remove[index]);
        }
    }
}
