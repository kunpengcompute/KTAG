#include <pcap/pcap.h>
#include "feature_session.h"
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ktag_legacy_inference.h"
#include "ktag_feature.h"
#include "ktag_inference.h"

namespace fs = std::filesystem;

namespace {

void write_be16(std::vector<uint8_t> *data, size_t offset, uint16_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 8U);
    (*data)[offset + 1U] = static_cast<uint8_t>(value & 0xffU);
}

std::vector<uint8_t> make_udp_frame()
{
    constexpr size_t ethernet_length = 14U;
    constexpr size_t ipv4_length = 20U;
    constexpr size_t udp_length = 8U;
    std::vector<uint8_t> frame(
        ethernet_length + ipv4_length + udp_length, 0U);
    write_be16(&frame, 12U, 0x0800U);
    frame[ethernet_length] = 0x45U;
    write_be16(
        &frame,
        ethernet_length + 2U,
        static_cast<uint16_t>(ipv4_length + udp_length));
    frame[ethernet_length + 8U] = 64U;
    frame[ethernet_length + 9U] = 17U;
    const std::array<uint8_t, 4> source = {10U, 30U, 0U, 1U};
    const std::array<uint8_t, 4> destination = {10U, 30U, 0U, 2U};
    std::memcpy(
        frame.data() + ethernet_length + 12U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + ethernet_length + 16U,
        destination.data(),
        destination.size());
    const size_t transport = ethernet_length + ipv4_length;
    write_be16(&frame, transport, 40000U);
    write_be16(&frame, transport + 2U, 443U);
    write_be16(&frame, transport + 4U, udp_length);
    return frame;
}

bool load_legacy_feature_csv(
    const fs::path &path,
    std::vector<feature_vector> *features,
    std::string *error)
{
    std::ifstream input(path);
    std::string line;
    if (!input || !std::getline(input, line)) {
        *error = "cannot open the CSV or the CSV is empty";
        return false;
    }
    uint64_t line_number = 1U;
    uint64_t skipped_rows = 0U;
    uint64_t first_invalid_line = 0U;
    uint32_t first_invalid_columns = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        std::stringstream row(line);
        std::string value;
        feature_vector feature{};
        uint32_t column = 0U;
        bool empty_value = false;
        while (column < KTAG_LEGACY_V1_VALUE_COUNT &&
               std::getline(row, value, ',')) {
            if (value.empty()) {
                empty_value = true;
                break;
            }
            /* Match the legacy demo CSV reader's atof conversion semantics. */
            feature.features[column++] =
                static_cast<float>(std::atof(value.c_str()));
        }
        if (empty_value || column != KTAG_LEGACY_V1_VALUE_COUNT) {
            ++skipped_rows;
            if (first_invalid_line == 0U) {
                first_invalid_line = line_number;
                first_invalid_columns = column;
            }
            continue;
        }
        features->push_back(feature);
    }
    if (features->empty()) {
        std::ostringstream message;
        message << "CSV contains no valid rows with "
                << KTAG_LEGACY_V1_VALUE_COUNT
                << " non-empty feature columns";
        if (first_invalid_line != 0U) {
            message << "; first invalid row is line "
                    << first_invalid_line << " with "
                    << first_invalid_columns << " parsed columns";
        }
        *error = message.str();
        return false;
    }
    if (skipped_rows != 0U) {
        std::cout << "legacy CSV: skipped " << skipped_rows
                  << " incomplete rows; first was line "
                  << first_invalid_line << std::endl;
    }
    error->clear();
    return true;
}

bool create_synthetic_legacy_features(
    std::vector<feature_vector> *features)
{
    ktag_demo_feature_session *extractor = ktag_demo_feature_session::create(DLT_EN10MB);
    if (extractor == nullptr) {
        return false;
    }
    const auto frame = make_udp_frame();
    for (uint32_t index = 0U; index < 100U; ++index) {
        timeval timestamp{};
        timestamp.tv_sec = index;
        feature_vector ignored{};
        int has_feature = 0;
        if (extractor->process(frame.data(),
                static_cast<uint32_t>(frame.size()),
                &timestamp,
                &ignored,
                &has_feature) != KTAG_SUCCESS ||
            has_feature != 0) {
            delete extractor;
            return false;
        }
    }

    feature_vector result{};
    feature_vector_list list{};
    list.vectors = &result;
    list.capacity = 1U;
    const bool success =
        extractor->flush(&list) == KTAG_SUCCESS &&
        list.count == 1U;
    delete extractor;
    if (success) {
        features->push_back(result);
    }
    return success;
}

} // namespace

TEST(KTAGInferenceLegacyAdapter, ConvertedPackageMatchesDemoBridgePrediction)
{
    const char *legacy_config_path = std::getenv("KTAG_LEGACY_CONFIG");
    const char *model_package_path =
        std::getenv("KTAG_LEGACY_MODEL_PACKAGE");
    if (legacy_config_path == nullptr || model_package_path == nullptr ||
        !fs::is_regular_file(legacy_config_path) ||
        !fs::is_directory(model_package_path)) {
        SUCCEED() << "set KTAG_LEGACY_CONFIG and KTAG_LEGACY_MODEL_PACKAGE "
                     "to run the optional real-model comparison";
        return;
    }

    std::vector<feature_vector> legacy_vectors;
    const char *feature_csv = std::getenv("KTAG_LEGACY_FEATURE_CSV");
    if (feature_csv != nullptr && feature_csv[0] != '\0') {
        ASSERT_TRUE(fs::is_regular_file(feature_csv))
            << "KTAG_LEGACY_FEATURE_CSV is not a regular file";
        std::string csv_error;
        ASSERT_TRUE(load_legacy_feature_csv(
            feature_csv, &legacy_vectors, &csv_error)) << csv_error;
    } else {
        ASSERT_TRUE(create_synthetic_legacy_features(&legacy_vectors));
    }
    feature_vector_list legacy_features{};
    legacy_features.vectors = legacy_vectors.data();
    legacy_features.count = static_cast<uint32_t>(legacy_vectors.size());
    legacy_features.capacity = legacy_features.count;

    inference_config legacy_config{};
    legacy_config.config_file = legacy_config_path;
    inference_engine *legacy_engine = inference_init(&legacy_config);
    ASSERT_NE(nullptr, legacy_engine);
    std::vector<int> legacy_labels(legacy_features.count);
    inference_result legacy_result{};
    legacy_result.predictions = legacy_labels.data();
    ASSERT_EQ(
        INFERENCE_SUCCESS,
        inference_predict(
            legacy_engine, &legacy_features, &legacy_result));
    ASSERT_GT(legacy_result.count, 0U);

    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, schema);
    ktag_feature_batch *feature_batch = ktag_feature_batch_create(
        schema, legacy_features.count);
    ASSERT_NE(nullptr, feature_batch);
    for (uint32_t index = 0U; index < legacy_features.count; ++index) {
        ktag_feature_record_metadata metadata{};
        metadata.struct_size = sizeof(metadata);
        metadata.api_version = KTAG_API_VERSION;
        metadata.flow_id = static_cast<uint64_t>(index) + 1U;
        metadata.first_packet_sequence = 1U;
        metadata.last_packet_sequence = 100U;
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_batch_append(
                feature_batch,
                &metadata,
                legacy_features.vectors[index].features,
                KTAG_LEGACY_V1_VALUE_COUNT));
    }

    ktag_inference_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.model_package_path = model_package_path;
    config.feature_schema = schema;
    config.intra_op_threads = 1U;
    config.inter_op_threads = 1U;
    ktag_inference_context *context = ktag_inference_create(&config);
    ASSERT_NE(nullptr, context) << ktag_inference_get_last_error();
    ktag_prediction_batch *predictions =
        ktag_prediction_batch_create(legacy_result.count);
    ASSERT_NE(nullptr, predictions);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_inference_predict(context, feature_batch, predictions))
        << ktag_inference_get_last_error();
    ASSERT_EQ(
        legacy_result.count,
        ktag_prediction_batch_get_count(predictions));
    for (uint32_t index = 0U; index < legacy_result.count; ++index) {
        ktag_prediction_record prediction{};
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_prediction_batch_get_record(
                predictions, index, &prediction));
        EXPECT_EQ(
            static_cast<int64_t>(legacy_labels[index]),
            prediction.label);
        EXPECT_LT(prediction.source_record_index, legacy_features.count);
        EXPECT_EQ(
            static_cast<uint64_t>(prediction.source_record_index) + 1U,
            prediction.metadata.flow_id);
    }

    ktag_prediction_batch_destroy(predictions);
    ktag_inference_destroy(context);
    ktag_feature_batch_destroy(feature_batch);
    ktag_feature_schema_destroy(schema);
    inference_destroy(legacy_engine);
}
