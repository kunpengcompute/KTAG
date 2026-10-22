/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include <gtest/gtest.h>
#include "inference_session.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {
namespace fs = std::filesystem;
// A real opset-11 ONNX graph: ReduceSum(input, axis=1), Cast(to=INT64).
// The manifest selects one value, so labels expose its scaling and row order.
constexpr unsigned char model[] = {
    8, 8, 58, 155, 1, 10, 53, 10, 5, 105, 110, 112, 117, 116, 18, 3,
    115, 117, 109, 34, 9, 82, 101, 100, 117, 99, 101, 83, 117, 109, 42, 11,
    10, 4, 97, 120, 101, 115, 64, 1, 160, 1, 7, 42, 15, 10, 8, 107,
    101, 101, 112, 100, 105, 109, 115, 24, 0, 160, 1, 2, 10, 30, 10, 3,
    115, 117, 109, 18, 6, 108, 97, 98, 101, 108, 115, 34, 4, 67, 97, 115,
    116, 42, 9, 10, 2, 116, 111, 24, 7, 160, 1, 2, 18, 17, 100, 101,
    109, 111, 45, 115, 101, 115, 115, 105, 111, 110, 45, 116, 101, 115, 116, 90,
    24, 10, 5, 105, 110, 112, 117, 116, 18, 15, 10, 13, 8, 1, 18, 9,
    10, 3, 18, 1, 78, 10, 2, 8, 1, 98, 21, 10, 6, 108, 97, 98,
    101, 108, 115, 18, 11, 10, 9, 8, 7, 18, 5, 10, 3, 18, 1, 78,
    66, 2, 16, 11,
};

class DemoInferenceSessionTest : public ::testing::Test {
protected:
    fs::path directory;
    std::unique_ptr<ktag_demo_inference_session> session;
    std::string error;
    void SetUp() override {
        static std::atomic<unsigned> sequence{0U};
        directory = fs::temp_directory_path() /
            ("ktag-demo-inference-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
        ASSERT_TRUE(fs::create_directory(directory));
        ASSERT_TRUE(fs::create_directory(directory / "package"));
        std::ofstream binary(directory / "package/model.onnx", std::ios::binary);
        binary.write(reinterpret_cast<const char *>(model), sizeof(model));
        binary.close();
        auto *schema = ktag_feature_schema_create_legacy_v1();
        ASSERT_NE(nullptr, schema);
        const auto hash = ktag_feature_schema_get_hash(schema);
        ktag_feature_schema_destroy(schema);
        std::ofstream manifest(directory / "package/manifest.json");
        manifest << "{\"model_name\":\"demo-test\",\"model_version\":\"1\",\"schema_hash\":\"0x"
                 << std::hex << hash
                 << "\",\"input_features\":[\"network.src_port\"],\"model\":\"model.onnx\","
                    "\"preprocessing\":{\"minimum_total_packets\":10,"
                    "\"packet_count_features\":[\"traffic.send.packet_count\",\"traffic.receive.packet_count\"],"
                    "\"missing_value_policy\":\"reject\",\"scaler\":\"scaler.json\"}}";
        manifest.close();
        std::ofstream scaler(directory / "package/scaler.json");
        scaler << "{\"schema_hash\":\"0x" << std::hex << hash
               << "\",\"input_features\":[\"network.src_port\"],\"mean\":[1],\"scale\":[2]}";
        scaler.close();
        write_config("pipeline:\n  enabled: false\n  model_package_path: package\n");
    }
    void write_config(const char *text) {
        std::ofstream(directory / "config.yaml") << text;
    }
    void open() {
        session = ktag_demo_inference_session::create((directory / "config.yaml").string().c_str(), &error);
        ASSERT_NE(nullptr, session) << error;
    }
    void TearDown() override {
        session.reset();
        // Only remove the unique temporary directory owned by this fixture.
        if (directory.parent_path() == fs::temp_directory_path() &&
            directory.filename().string().rfind("ktag-demo-inference-test-", 0U) == 0U) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }
    }
};

TEST_F(DemoInferenceSessionTest, FiltersScalesAndKeepsSourceRowsAligned) {
    open(); ASSERT_NE(nullptr, session);
    feature_vector rows[4]{};
    const float ports[] = {3, 9, 7, 13};
    const float packets[] = {2, 10, 4, 20};
    for (unsigned i = 0; i < 4; ++i) {
        rows[i].features[0] = static_cast<float>(100 + i);
        rows[i].features[3] = ports[i];
        rows[i].features[7] = packets[i];
    }
    std::vector<feature_vector> accepted;
    std::vector<int> labels;
    ASSERT_TRUE(session->predict({rows, 4U, 4U}, &accepted, &labels, &error)) << error;
    ASSERT_EQ(2U, accepted.size());
    EXPECT_EQ((std::vector<int>{4, 6}), labels);
    EXPECT_FLOAT_EQ(101, accepted[0].features[0]);
    EXPECT_FLOAT_EQ(103, accepted[1].features[0]);
}

TEST_F(DemoInferenceSessionTest, EmptyAndAllFilteredInputsClearPreviousResults) {
    open(); ASSERT_NE(nullptr, session);
    feature_vector row{};
    row.features[3] = 1; row.features[7] = 10;
    std::vector<feature_vector> accepted;
    std::vector<int> labels;
    ASSERT_TRUE(session->predict({&row, 1U, 1U}, &accepted, &labels, &error)) << error;
    ASSERT_EQ((std::vector<int>{0}), labels);
    ASSERT_TRUE(session->predict({}, &accepted, &labels, &error)) << error;
    EXPECT_TRUE(accepted.empty()); EXPECT_TRUE(labels.empty());
    row.features[7] = 0;
    ASSERT_TRUE(session->predict({&row, 1U, 1U}, &accepted, &labels, &error)) << error;
    EXPECT_TRUE(accepted.empty()); EXPECT_TRUE(labels.empty());
}

TEST_F(DemoInferenceSessionTest, RejectsNonFiniteModelInputWithoutPartialOutput) {
    open(); ASSERT_NE(nullptr, session);
    feature_vector row{};
    row.features[3] = std::numeric_limits<float>::infinity(); row.features[7] = 10;
    std::vector<feature_vector> accepted(1);
    std::vector<int> labels{99};
    EXPECT_FALSE(session->predict({&row, 1U, 1U}, &accepted, &labels, &error));
    EXPECT_TRUE(accepted.empty()); EXPECT_TRUE(labels.empty());
    EXPECT_NE(std::string::npos, error.find("non-finite"));
}

TEST_F(DemoInferenceSessionTest, RejectsMissingInputStorage) {
    open(); ASSERT_NE(nullptr, session);
    std::vector<feature_vector> accepted;
    std::vector<int> labels;
    EXPECT_FALSE(session->predict({nullptr, 1U, 1U}, &accepted, &labels, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(DemoInferenceSessionTest, RequiresModelPackageEvenWhenPipelineDisabled) {
    write_config("pipeline:\n  enabled: false\n");
    session = ktag_demo_inference_session::create((directory / "config.yaml").string().c_str(), &error);
    EXPECT_EQ(nullptr, session);
    EXPECT_NE(std::string::npos, error.find("model_package_path"));
    EXPECT_NE(std::string::npos, error.find("create_legacy_package.py"));
}

TEST_F(DemoInferenceSessionTest, RejectsInvalidThreadCount) {
    write_config("pipeline:\n  model_package_path: package\n  inference:\n    intra_op_threads: 0\n");
    session = ktag_demo_inference_session::create((directory / "config.yaml").string().c_str(), &error);
    EXPECT_EQ(nullptr, session);
    EXPECT_NE(std::string::npos, error.find("positive uint32"));
}

TEST_F(DemoInferenceSessionTest, RejectsWrongSchemaPackage) {
    std::ofstream(directory / "package/manifest.json") <<
        "{\"model_name\":\"wrong\",\"model_version\":\"1\",\"schema_hash\":\"0x1\","
        "\"input_features\":[\"network.src_port\"],\"model\":\"model.onnx\","
        "\"preprocessing\":{\"minimum_total_packets\":0,\"packet_count_features\":[],"
        "\"missing_value_policy\":\"reject\",\"scaler\":\"scaler.json\"}}";
    session = ktag_demo_inference_session::create((directory / "config.yaml").string().c_str(), &error);
    EXPECT_EQ(nullptr, session);
    EXPECT_NE(std::string::npos, error.find("schema"));
}
}

TEST_F(DemoInferenceSessionTest, TimedPublicApiMeasuresModelAndDistinguishesFilteredCalls) {
    auto *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, schema);
    std::unique_ptr<ktag_feature_schema, decltype(&ktag_feature_schema_destroy)> owned_schema(schema, ktag_feature_schema_destroy);
    ktag_inference_config config{}; config.struct_size = sizeof(config); config.api_version = KTAG_API_VERSION;
    const auto path = (directory / "package").string();
    config.feature_schema = schema; config.model_package_path = path.c_str();
    config.intra_op_threads = config.inter_op_threads = 1;
    std::unique_ptr<ktag_inference_context, decltype(&ktag_inference_destroy)> context(
        ktag_inference_create(&config), ktag_inference_destroy);
    ASSERT_NE(nullptr, context) << ktag_inference_get_last_error();
    std::unique_ptr<ktag_feature_batch, decltype(&ktag_feature_batch_destroy)> input(
        ktag_feature_batch_create(schema, 2), ktag_feature_batch_destroy);
    std::unique_ptr<ktag_prediction_batch, decltype(&ktag_prediction_batch_destroy)> output(
        ktag_prediction_batch_create(2), ktag_prediction_batch_destroy);
    ASSERT_NE(nullptr, input); ASSERT_NE(nullptr, output);
    ktag_feature_record_metadata metadata{}; metadata.struct_size = sizeof(metadata); metadata.api_version = KTAG_API_VERSION;
    float values[KTAG_LEGACY_V1_VALUE_COUNT]{}; values[3] = 9; values[7] = 10;
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_append(input.get(), &metadata, values, KTAG_LEGACY_V1_VALUE_COUNT));
    values[7] = 0;
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_append(input.get(), &metadata, values, KTAG_LEGACY_V1_VALUE_COUNT));
    ktag_inference_timing timing{}; timing.struct_size = sizeof(timing); timing.api_version = KTAG_API_VERSION;
    ASSERT_EQ(KTAG_SUCCESS, ktag_inference_predict_with_timing(context.get(), input.get(), output.get(), &timing));
    EXPECT_EQ(2U, timing.input_record_count); EXPECT_EQ(1U, timing.model_record_count);
    EXPECT_GT(timing.model_nanoseconds, 0U); EXPECT_EQ(1U, ktag_prediction_batch_get_count(output.get()));
    ktag_feature_batch_clear(input.get());
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_batch_append(input.get(), &metadata, values, KTAG_LEGACY_V1_VALUE_COUNT));
    ASSERT_EQ(KTAG_SUCCESS, ktag_inference_predict_with_timing(context.get(), input.get(), output.get(), &timing));
    EXPECT_EQ(1U, timing.input_record_count); EXPECT_EQ(0U, timing.model_record_count);
    EXPECT_EQ(0U, timing.model_nanoseconds); EXPECT_EQ(0U, ktag_prediction_batch_get_count(output.get()));
}
