/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include <gtest/gtest.h>
#include "latency_benchmark.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
namespace {
namespace fs = std::filesystem;
constexpr unsigned char latency_test_model[] = {
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
void le32(std::ostream &out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out.put(static_cast<char>(value >> (8 * i)));
}
void be16(std::vector<unsigned char> &data, size_t offset, uint16_t value) {
  data[offset] = static_cast<unsigned char>(value >> 8); data[offset + 1] = static_cast<unsigned char>(value);
}
void write_pcap(const fs::path &path) {
  std::ofstream out(path, std::ios::binary);
  le32(out, 0xa1b2c3d4); le32(out, 0x00040002); le32(out, 0); le32(out, 0); le32(out, 65535); le32(out, 1);
  const uint16_t ports[] = {1001,1001,1001,1001,1001,1001,1002,1003,1003,0};
  for (unsigned i = 0; i < 10; ++i) {
    std::vector<unsigned char> frame(43, 0);
    be16(frame, 12, ports[i] ? 0x0800 : 0x0806);
    frame[14] = 0x45; be16(frame, 16, 29); frame[22] = 64; frame[23] = 17;
    frame[26] = frame[30] = 10; frame[29] = 1; frame[33] = 2;
    be16(frame, 34, ports[i]); be16(frame, 36, 443); be16(frame, 38, 9);
    le32(out, 1); le32(out, i * 10); le32(out, frame.size()); le32(out, frame.size());
    out.write(reinterpret_cast<const char *>(frame.data()), frame.size());
  }
}
std::vector<std::vector<std::string>> read_csv(const fs::path &path) {
  std::ifstream input(path); std::vector<std::vector<std::string>> rows; std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> row; std::string cell; std::istringstream in(line);
    while (std::getline(in, cell, ',')) row.push_back(cell);
    if (!line.empty() && line.back() == ',') row.push_back("");
    rows.push_back(std::move(row));
  }
  return rows;
}
uint64_t metadata_value(const fs::path &prefix, const std::string &key) {
  for (const auto &row : read_csv(prefix.string() + ".run.csv"))
    if (row.size() == 2 && row[0] == key) return std::stoull(row[1]);
  throw std::runtime_error("missing metadata: " + key);
}
ktag_status create_counter(const ktag_feature_provider_config *config, void **state) {
  *state = config->user_data; return KTAG_SUCCESS;
}
ktag_status count_packets(void *state, void *, const ktag_flow_view *flow, ktag_feature_output *output) {
  float value = static_cast<float>(ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_FORWARD) +
      ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_REVERSE));
  if (state && *static_cast<bool *>(state) && ktag_flow_view_get_flow_id(flow) == 1) value = 0;
  return ktag_feature_output_set(output, "traffic.send.packet_count", &value, 1);
}
class LatencyPipelineTest : public ::testing::Test {
protected:
  fs::path directory;
  ktag_feature_schema *schema = nullptr;
  ktag_feature_registry *registry = nullptr;
  volatile sig_atomic_t stop = 0;
  bool zero_first_flow = false;
  void SetUp() override {
    static std::atomic<unsigned> sequence{0};
    directory = fs::temp_directory_path() / ("ktag-latency-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
    ASSERT_TRUE(fs::create_directories(directory / "package"));
    schema = ktag_feature_schema_create("latency_test", 1); registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, schema); ASSERT_NE(nullptr, registry);
    ktag_feature_descriptor descriptor{}; descriptor.struct_size = sizeof(descriptor); descriptor.api_version = KTAG_API_VERSION;
    descriptor.feature_id = "traffic.send.packet_count"; descriptor.display_name = "packets";
    descriptor.provider_id = "test.latency"; descriptor.provider_version = 1;
    descriptor.value_type = KTAG_FEATURE_FLOAT32; descriptor.dimension = 1;
    ktag_feature_provider_registration provider{}; provider.struct_size = sizeof(provider); provider.api_version = KTAG_API_VERSION;
    provider.provider_id = descriptor.provider_id; provider.provider_version = 1;
    provider.features = &descriptor; provider.feature_count = 1;
    provider.callbacks.struct_size = sizeof(provider.callbacks); provider.callbacks.api_version = KTAG_API_VERSION;
    provider.callbacks.flow_finish = count_packets;
    provider.callbacks.provider_create = create_counter; provider.user_data = &zero_first_flow;
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_registry_register(registry, &provider, KTAG_FEATURE_REGISTER_DEFAULT));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema, &descriptor));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    std::ofstream binary(directory / "package/model.onnx", std::ios::binary);
    binary.write(reinterpret_cast<const char *>(latency_test_model), sizeof(latency_test_model)); binary.close();
    const auto hash = ktag_feature_schema_get_hash(schema);
    std::ofstream(directory / "package/scaler.json") << "{\"schema_hash\":\"0x" << std::hex << hash
        << "\",\"input_features\":[\"traffic.send.packet_count\"],\"mean\":[0],\"scale\":[1]}";
    manifest(2); write_pcap(directory / "input.pcap");
  }
  void manifest(unsigned minimum) {
    std::ofstream(directory / "package/manifest.json") << "{\"model_name\":\"latency\",\"model_version\":\"1\",\"schema_hash\":\"0x"
        << std::hex << ktag_feature_schema_get_hash(schema)
        << "\",\"input_features\":[\"traffic.send.packet_count\"],\"model\":\"model.onnx\","
        << "\"preprocessing\":{\"minimum_total_packets\":" << std::dec << minimum
        << ",\"packet_count_features\":[\"traffic.send.packet_count\"],\"missing_value_policy\":\"reject\",\"scaler\":\"scaler.json\"}}";
  }
  int run(bool parallel, const std::string &mode, uint32_t size, const std::string &name, bool twice = false) {
    ktag_demo_pipeline_config config;
    config.mode = parallel ? ktag_demo_pipeline_mode::full_parallel : ktag_demo_pipeline_mode::single_thread;
    config.feature_threads = parallel ? 2 : 1; config.inference_threads = parallel ? 2 : 1;
    config.packet_queue_capacity = config.feature_queue_capacity = 1;
    config.model_package_path = (directory / "package").string();
    config.latency.mode = mode; config.latency.batch_size = size; config.latency.output_prefix = (directory / name).string();
    config.latency.packet_details = mode == "e2e"; config.latency.warmup = 2; config.latency.iterations = 5;
    config.inference_batch_timeout_us = 100000;
    auto make_feature = [&](ktag_domain_store *store) {
      ktag_feature_config fc{}; fc.struct_size = sizeof(fc); fc.api_version = KTAG_API_VERSION;
      fc.schema = schema; fc.registry = registry; fc.shared_domain_store = store; fc.udp_packet_limit = 3;
      return ktag_feature_create(&fc);
    };
    auto make_inference = [&] {
      ktag_inference_config ic{}; ic.struct_size = sizeof(ic); ic.api_version = KTAG_API_VERSION;
      ic.feature_schema = schema; ic.model_package_path = config.model_package_path.c_str();
      ic.intra_op_threads = ic.inter_op_threads = 1; return ktag_inference_create(&ic);
    };
    const auto path = (directory / "input.pcap").string(); const char *inputs[] = {path.c_str(), path.c_str()};
    return ktag_demo_run_latency_benchmark(config, schema, inputs, twice ? 2 : 1, make_feature, make_inference, &stop);
  }
  void verify_e2e(const std::string &name, unsigned source_count, unsigned limit) {
    const auto prefix = directory / name;
    EXPECT_EQ(10U * source_count, metadata_value(prefix, "captured_packets"));
    EXPECT_EQ(9U * source_count, metadata_value(prefix, "associated_packets"));
    EXPECT_EQ(3U * source_count, metadata_value(prefix, "model_output_flows"));
    EXPECT_EQ(source_count, metadata_value(prefix, "filtered_input_flows"));
    EXPECT_EQ(8U * source_count, metadata_value(prefix, "predicted_packet_observations"));
    EXPECT_EQ(0U, metadata_value(prefix, "flows_missing_packet_history"));
    const auto flows = read_csv(prefix.string() + ".flows.csv"); ASSERT_EQ(1U + 4U * source_count, flows.size());
    for (const auto &row : flows) EXPECT_EQ(19U, row.size());
    for (size_t i = 1; i < flows.size(); ++i) {
      const auto &row = flows[i]; ASSERT_EQ(19U, row.size());
      if (row[5] == "predicted") {
        EXPECT_EQ(std::stoull(row[8]), static_cast<uint64_t>(std::stoll(row[6])));
        EXPECT_EQ(std::stoull(row[14]) - std::stoull(row[11]), std::stoull(row[15]));
        EXPECT_EQ(std::stoull(row[14]) - std::stoull(row[12]), std::stoull(row[16]));
        EXPECT_EQ(std::stoull(row[14]) - std::stoull(row[13]), std::stoull(row[17]));
      } else {
        EXPECT_EQ("filtered", row[5]);
        for (size_t col = 14; col <= 17; ++col) EXPECT_TRUE(row[col].empty());
      }
    }
    const auto packets = read_csv(prefix.string() + ".packets.csv"); ASSERT_EQ(1U + 8U * source_count, packets.size());
    for (size_t i = 1; i < packets.size(); ++i) {
      ASSERT_EQ(7U, packets[i].size());
      EXPECT_EQ(std::stoull(packets[i][5]) - std::stoull(packets[i][4]), std::stoull(packets[i][6]));
    }
    std::vector<uint64_t> delays;
    for (size_t i = 1; i < packets.size(); ++i) delays.push_back(std::stoull(packets[i][6]));
    std::sort(delays.begin(), delays.end());
    long double total = 0; for (uint64_t value : delays) total += value;
    bool found_packet_metric = false;
    for (const auto &row : read_csv(prefix.string() + ".summary.csv")) {
      if (!row.empty() && row[0] == "packet_to_prediction") {
        ASSERT_EQ(8U, row.size()); found_packet_metric = true;
        EXPECT_EQ(delays.size(), std::stoull(row[2]));
        EXPECT_NEAR(static_cast<double>(total / delays.size()), std::stod(row[3]) * 1000.0, 1.0);
        EXPECT_NEAR(delays[static_cast<size_t>(std::ceil(.99 * delays.size())) - 1], std::stod(row[6]) * 1000.0, 1.0);
        EXPECT_NEAR(delays.back(), std::stod(row[7]) * 1000.0, 1.0);
      }
    }
    EXPECT_TRUE(found_packet_metric);
    for (const auto &row : read_csv(prefix.string() + ".sources.csv")) EXPECT_EQ(13U, row.size());
    const auto batches = read_csv(prefix.string() + ".batches.csv");
    for (size_t i = 1; i < batches.size(); ++i) EXPECT_LE(std::stoul(batches[i][3]), limit);
  }
  void TearDown() override {
    ktag_feature_registry_destroy(registry); ktag_feature_schema_destroy(schema);
    if (directory.parent_path() == fs::temp_directory_path() && directory.filename().string().find("ktag-latency-test-") == 0) {
      std::error_code ignored; fs::remove_all(directory, ignored);
    }
  }
};
TEST_F(LatencyPipelineTest, StrictSingleRecordCallsAndSourceIsolation) {
  ASSERT_EQ(0, run(false, "e2e", 1, "single", true)); verify_e2e("single", 2, 1);
  ASSERT_EQ(0, run(true, "e2e", 1, "parallel", true)); verify_e2e("parallel", 2, 1);
}
TEST_F(LatencyPipelineTest, FullAndPartialBatchesKeepPacketAssociations) {
  ASSERT_EQ(0, run(false, "e2e", 3, "single-batch")); verify_e2e("single-batch", 1, 3);
  ASSERT_EQ(0, run(true, "e2e", 3, "parallel-batch")); verify_e2e("parallel-batch", 1, 3);
}
TEST_F(LatencyPipelineTest, PureInferenceExcludesWarmupAndUsesActualWorkerCount) {
  for (bool parallel : {false, true}) {
    const std::string name = parallel ? "pure-parallel" : "pure-single";
    ASSERT_EQ(0, run(parallel, "inference", 3, name));
    const auto prefix = directory / name;
    EXPECT_EQ(parallel ? 30U : 15U, metadata_value(prefix, "model_output_flows"));
    EXPECT_EQ(parallel ? 11U : 6U, read_csv(prefix.string() + ".batches.csv").size());
    EXPECT_FALSE(fs::exists(prefix.string() + ".flows.csv"));
    for (const auto &row : read_csv(prefix.string() + ".sources.csv")) EXPECT_EQ(13U, row.size());
  }
}
TEST_F(LatencyPipelineTest, AFilteredCallDoesNotConsumeTheFirstModelCallMarker) {
  zero_first_flow = true;
  ASSERT_EQ(0, run(false, "e2e", 1, "first-filtered"));
  const auto rows = read_csv((directory / "first-filtered.batches.csv").string());
  ASSERT_GT(rows.size(), 2U);
  EXPECT_EQ("0", rows[1][4]); EXPECT_EQ("0", rows[1][9]);
  unsigned measured_first = 0;
  for (size_t i = 1; i < rows.size(); ++i) {
    if (rows[i][9] == "1") { ++measured_first; EXPECT_NE("0", rows[i][4]); }
  }
  EXPECT_EQ(1U, measured_first);
}
TEST_F(LatencyPipelineTest, RefusesExistingReportsAndDoesNotOverwriteThem) {
  std::ofstream(directory / "existing.summary.csv") << "preserve me";
  EXPECT_EQ(-1, run(false, "e2e", 1, "existing"));
  std::ifstream input(directory / "existing.summary.csv"); std::string value; std::getline(input, value);
  EXPECT_EQ("preserve me", value);
}
TEST_F(LatencyPipelineTest, RejectsAllFilteredDataRatherThanReportingZeroModelLatency) {
  manifest(4); EXPECT_EQ(-1, run(true, "e2e", 3, "filtered"));
  EXPECT_FALSE(fs::exists(directory / "filtered.summary.csv"));
  EXPECT_EQ(-1, run(true, "inference", 3, "filtered-pure"));
}
TEST_F(LatencyPipelineTest, CaptureFailureDrainsAndJoinsWorkersWithoutSuccessfulReport) {
  fs::resize_file(directory / "input.pcap", fs::file_size(directory / "input.pcap") - 1);
  EXPECT_EQ(-1, run(true, "e2e", 3, "truncated"));
  EXPECT_FALSE(fs::exists(directory / "truncated.summary.csv"));
}
} // namespace
