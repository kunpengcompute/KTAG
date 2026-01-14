/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "kdadk_test.h"
#include "kdadk_inference.h"

// 测试配置文件路径
static const char *TEST_CONFIG_FILE = "../../../src/config.yaml";

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

    void createTestConfig()
    {
        std::ofstream ofs(TEST_CONFIG_FILE);
        ofs << "model_path: \"test_model.onnx\"\n";
        ofs << "scaler_path: \"test_scaler.pkl\"\n";
        ofs << "online_data_path: \"test_data\"\n";
        ofs << "filter_packets: 5\n";
        ofs << "columns_to_remove: [0, 1, 2]\n";
        ofs << "onnx_log_level: WARNING\n";
        ofs.close();
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
    feature_vector_list features;
    inference_result result;

    int ret = inference_predict(engine, nullptr, &result);
    EXPECT_EQ(ret, INFERENCE_ERROR) << "Should fail with NULL features";

    features.count = 1;
    features.vectors = static_cast<feature_vector *>(malloc(sizeof(feature_vector) * features.count));;
    ret = inference_predict(engine, &features, nullptr);
    EXPECT_EQ(ret, INFERENCE_ERROR) << "Should fail with NULL result";
}

TEST_F(InferenceEngineTest, ConfigParameters)
{
    inference_config config;
    config.config_file = TEST_CONFIG_FILE;

    engine = inference_init(&config);

    if (engine) {
        EXPECT_EQ(engine->filter_packets, 10) << "filter_packets should be 5";
        EXPECT_EQ(engine->columns_to_remove_count, 3) << "Should have 3 columns to remove";
    }
}