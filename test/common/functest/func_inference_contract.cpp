/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include <unistd.h>

#include "ktag_feature.h"
#include "ktag_inference.h"

namespace fs = std::filesystem;

namespace {

std::string hash_text(uint64_t hash)
{
    std::ostringstream output;
    output << "0x" << std::hex << hash;
    return output.str();
}

bool add_feature(
    ktag_feature_schema *schema,
    const char *feature_id,
    uint32_t dimension)
{
    ktag_feature_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    descriptor.feature_id = feature_id;
    descriptor.display_name = feature_id;
    descriptor.provider_id = "test.inference.contract";
    descriptor.provider_version = 1U;
    descriptor.value_type = KTAG_FEATURE_FLOAT32;
    descriptor.dimension = dimension;
    return ktag_feature_schema_add(schema, &descriptor) == KTAG_SUCCESS;
}

class InferenceContractTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        schema = ktag_feature_schema_create("inference_contract_test", 7U);
        ASSERT_NE(nullptr, schema);
        ASSERT_TRUE(add_feature(schema, "custom.extra", 1U));
        ASSERT_TRUE(add_feature(
            schema, "traffic.receive.packet_count", 1U));
        ASSERT_TRUE(add_feature(schema, "custom.vector", 2U));
        ASSERT_TRUE(add_feature(
            schema, "traffic.send.packet_count", 1U));
        ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));

        package = fs::temp_directory_path() /
            ("ktag_inference_contract_" + std::to_string(getpid()));
        fs::remove_all(package);
        ASSERT_TRUE(fs::create_directories(package));
        std::ofstream model(package / "model.onnx", std::ios::binary);
        model << "contract validation does not open the ONNX model";
    }

    void TearDown() override
    {
        ktag_feature_schema_destroy(schema);
        schema = nullptr;
        std::error_code ignored;
        fs::remove_all(package, ignored);
    }

    void write_manifest(
        uint64_t manifest_hash,
        const std::string &input_features,
        const char *missing_policy = "reject")
    {
        std::ofstream manifest(package / "manifest.json");
        manifest << "{\n"
                 << "  \"model_name\": \"contract-test\",\n"
                 << "  \"model_version\": \"1.0.0\",\n"
                 << "  \"schema_hash\": \""
                 << hash_text(manifest_hash) << "\",\n"
                 << "  \"input_features\": " << input_features << ",\n"
                 << "  \"preprocessing\": {\n"
                 << "    \"minimum_total_packets\": 3,\n"
                 << "    \"packet_count_features\": ["
                 << "\"traffic.send.packet_count\", "
                 << "\"traffic.receive.packet_count\"],\n"
                 << "    \"missing_value_policy\": \""
                 << missing_policy << "\",\n"
                 << "    \"scaler\": \"scaler.json\"\n"
                 << "  },\n"
                 << "  \"model\": \"model.onnx\"\n"
                 << "}\n";
    }

    void write_scaler(
        uint64_t scaler_hash,
        const std::string &input_features,
        const std::string &mean,
        const std::string &scale)
    {
        std::ofstream scaler(package / "scaler.json");
        scaler << "{\n"
               << "  \"schema_hash\": \""
               << hash_text(scaler_hash) << "\",\n"
               << "  \"input_features\": " << input_features << ",\n"
               << "  \"mean\": " << mean << ",\n"
               << "  \"scale\": " << scale << "\n"
               << "}\n";
    }

    ktag_model_package_validation validation()
    {
        ktag_model_package_validation result{};
        result.struct_size = sizeof(result);
        result.api_version = KTAG_API_VERSION;
        return result;
    }

    ktag_feature_schema *schema = nullptr;
    fs::path package;
};

} // namespace

TEST_F(InferenceContractTest, ResolvesFeatureIdsDimensionsAndIgnoresExtras)
{
    const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
    write_manifest(
        schema_hash,
        "[\"custom.vector\", \"traffic.send.packet_count\"]");
    write_scaler(
        schema_hash,
        "[\"custom.vector\", \"traffic.send.packet_count\"]",
        "[1.0, 2.0, 3.0]",
        "[1.0, 2.0, 4.0]");

    auto result = validation();
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_inference_validate_model_package(
            package.string().c_str(), schema, &result))
        << ktag_inference_get_last_error();
    EXPECT_EQ(schema_hash, result.schema_hash);
    EXPECT_EQ(2U, result.input_feature_count);
    EXPECT_EQ(3U, result.input_value_count);
    EXPECT_EQ(3U, result.minimum_total_packets);
}

TEST_F(InferenceContractTest, RejectsManifestSchemaMismatch)
{
    const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
    write_manifest(
        schema_hash + 1U,
        "[\"custom.vector\", \"traffic.send.packet_count\"]");
    write_scaler(
        schema_hash + 1U,
        "[\"custom.vector\", \"traffic.send.packet_count\"]",
        "[0.0, 0.0, 0.0]",
        "[1.0, 1.0, 1.0]");

    auto result = validation();
    EXPECT_EQ(
        KTAG_ERROR_SCHEMA_MISMATCH,
        ktag_inference_validate_model_package(
            package.string().c_str(), schema, &result));
    EXPECT_STRNE("", ktag_inference_get_last_error());
}

TEST_F(InferenceContractTest, RejectsMissingRequiredFeature)
{
    const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
    write_manifest(schema_hash, "[\"customer.missing\"]");
    write_scaler(
        schema_hash,
        "[\"customer.missing\"]",
        "[0.0]",
        "[1.0]");

    auto result = validation();
    EXPECT_EQ(
        KTAG_ERROR_NOT_FOUND,
        ktag_inference_validate_model_package(
            package.string().c_str(), schema, &result));
}

TEST_F(InferenceContractTest, ExplicitZeroPolicyRequiresAndUsesDimension)
{
    const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
    write_manifest(
        schema_hash,
        "[{\"feature_id\": \"customer.missing\", \"dimension\": 2}]",
        "zero");
    write_scaler(
        schema_hash,
        "[\"customer.missing\"]",
        "[0.0, 0.0]",
        "[1.0, 1.0]");

    auto result = validation();
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_inference_validate_model_package(
            package.string().c_str(), schema, &result))
        << ktag_inference_get_last_error();
    EXPECT_EQ(1U, result.input_feature_count);
    EXPECT_EQ(2U, result.input_value_count);
}

TEST_F(InferenceContractTest, ZeroPolicyRejectsMissingFeatureWithoutDimension)
{
    const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
    write_manifest(schema_hash, "[\"customer.missing\"]", "zero");
    write_scaler(
        schema_hash,
        "[\"customer.missing\"]",
        "[0.0]",
        "[1.0]");

    auto result = validation();
    EXPECT_EQ(
        KTAG_ERROR_NOT_FOUND,
        ktag_inference_validate_model_package(
            package.string().c_str(), schema, &result));
}

TEST_F(InferenceContractTest, RejectsScalerFeatureOrderMismatch)
{
    const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
    write_manifest(
        schema_hash,
        "[\"custom.vector\", \"traffic.send.packet_count\"]");
    write_scaler(
        schema_hash,
        "[\"traffic.send.packet_count\", \"custom.vector\"]",
        "[0.0, 0.0, 0.0]",
        "[1.0, 1.0, 1.0]");

    auto result = validation();
    EXPECT_EQ(
        KTAG_ERROR_SCHEMA_MISMATCH,
        ktag_inference_validate_model_package(
            package.string().c_str(), schema, &result));
}

TEST_F(InferenceContractTest, RejectsScalerSchemaMismatch)
{
    const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
    write_manifest(
        schema_hash,
        "[\"custom.vector\", \"traffic.send.packet_count\"]");
    write_scaler(
        schema_hash + 1U,
        "[\"custom.vector\", \"traffic.send.packet_count\"]",
        "[0.0, 0.0, 0.0]",
        "[1.0, 1.0, 1.0]");

    auto result = validation();
    EXPECT_EQ(
        KTAG_ERROR_SCHEMA_MISMATCH,
        ktag_inference_validate_model_package(
            package.string().c_str(), schema, &result));
}

TEST(KTAGPredictionBatch, OwnsRecordsAndValidatesAccess)
{
    EXPECT_FLOAT_EQ(-1.0F, KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE);
    EXPECT_NE(0.0F, KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE);
    ktag_prediction_batch *batch = ktag_prediction_batch_create(2U);
    ASSERT_NE(nullptr, batch);
    EXPECT_EQ(0U, ktag_prediction_batch_get_count(batch));
    EXPECT_EQ(KTAG_SUCCESS, ktag_prediction_batch_reserve(batch, 8U));
    ktag_prediction_record record{};
    EXPECT_EQ(
        KTAG_ERROR_INVALID_ARGUMENT,
        ktag_prediction_batch_get_record(batch, 0U, &record));
    ktag_prediction_batch_clear(batch);
    ktag_prediction_batch_destroy(batch);
}
