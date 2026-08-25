#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include "bounded_queue.h"
#include "feature_plugin_loader.h"
#include "ktag_traffic_features.h"
#include "pipeline_config.h"
#include "pipeline_runner.h"
#include "proxy_feature_catalog.h"
#include "schema_feature_writer.h"
#include "ub_feature_wire.h"

namespace {

namespace fs = std::filesystem;

const char *const EXAMPLE_PLUGIN =
    "../../../output/plugins/libktag_example_feature_plugin.so";
const char *const PROXY_PLUGIN =
    "../../../output/plugins/libktag_proxy_features.so";
const char *const TRAFFIC_PLUGIN =
    "../../../output/plugins/libktag_traffic_features.so";
const char *const OFFICIAL_PROXY_CONFIG =
    "../../../tools/demo/proxy_config.yaml";
const char *const BAD_ABI_PLUGIN =
    "../../../output/plugins/libktag_test_feature_plugin_bad_abi.so";
const char *const CREATE_FAIL_PLUGIN =
    "../../../output/plugins/libktag_test_feature_plugin_create_fail.so";
const char *const NO_ENTRY_PLUGIN =
    "../../../output/plugins/libktag_test_feature_plugin_no_entry.so";

std::string write_config(const std::string &body)
{
    const std::string path =
        "/tmp/ktag_pipeline_" + std::to_string(getpid()) + ".yaml";
    std::ofstream output(path);
    output << body;
    output.close();
    return path;
}

bool add_proxy_catalog(
    ktag_feature_schema *schema,
    const ktag_feature_registry *registry)
{
    for (const auto *provider :
         ktag_proxy_features::KTAG_PROXY_PROVIDER_CATALOG) {
        for (size_t index = 0U;
             index < provider->feature_count;
             ++index) {
            if (ktag_feature_schema_add_registered(
                    schema,
                    registry,
                    provider->features[index].feature_id) !=
                KTAG_SUCCESS) {
                return false;
            }
        }
    }
    return true;
}

void add_proxy_feature_ids(ktag_demo_pipeline_config *config)
{
    for (const auto *provider :
         ktag_proxy_features::KTAG_PROXY_PROVIDER_CATALOG) {
        for (size_t index = 0U;
             index < provider->feature_count;
             ++index) {
            config->add_features.emplace_back(
                provider->features[index].feature_id);
        }
    }
}

ktag_feature_plugin_descriptor query_proxy_plugin()
{
    ktag_feature_plugin_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    EXPECT_EQ(
        KTAG_SUCCESS,
        ktag_feature_plugin_query(
            KTAG_FEATURE_PLUGIN_ABI_VERSION, &descriptor));
    return descriptor;
}

void write_be16(
    std::vector<uint8_t> *data,
    size_t offset,
    uint16_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 8U);
    (*data)[offset + 1U] = static_cast<uint8_t>(value & 0xffU);
}

std::vector<uint8_t> make_proxy_test_udp_frame()
{
    constexpr size_t ethernet_length = 14U;
    constexpr size_t ipv4_length = 20U;
    constexpr size_t udp_length = 8U;
    std::vector<uint8_t> frame(
        ethernet_length + ipv4_length + udp_length + 1U, 0U);
    write_be16(&frame, 12U, 0x0800U);
    frame[ethernet_length] = 0x45U;
    write_be16(
        &frame,
        ethernet_length + 2U,
        static_cast<uint16_t>(ipv4_length + udp_length + 1U));
    frame[ethernet_length + 8U] = 64U;
    frame[ethernet_length + 9U] = 17U;
    const std::array<uint8_t, 4U> source{{10U, 0U, 0U, 1U}};
    const std::array<uint8_t, 4U> destination{{10U, 0U, 0U, 2U}};
    std::memcpy(
        frame.data() + ethernet_length + 12U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + ethernet_length + 16U,
        destination.data(),
        destination.size());
    const size_t transport = ethernet_length + ipv4_length;
    write_be16(&frame, transport, 50000U);
    write_be16(&frame, transport + 2U, 443U);
    write_be16(&frame, transport + 4U, udp_length + 1U);
    frame.back() = 0x40U;
    return frame;
}

std::array<uint8_t, 16U> ipv6_address(uint8_t last)
{
    std::array<uint8_t, 16U> address{};
    address[0U] = 0x20U;
    address[1U] = 0x01U;
    address[2U] = 0x0dU;
    address[3U] = 0xb8U;
    address[15U] = last;
    return address;
}

std::vector<uint8_t> make_ipv6_frame(
    uint8_t next_header,
    const std::vector<uint8_t> &payload)
{
    constexpr size_t ethernet_length = 14U;
    constexpr size_t ipv6_length = 40U;
    std::vector<uint8_t> frame(
        ethernet_length + ipv6_length + payload.size(), 0U);
    write_be16(&frame, 12U, 0x86ddU);
    const size_t network = ethernet_length;
    frame[network] = 0x60U;
    write_be16(
        &frame,
        network + 4U,
        static_cast<uint16_t>(payload.size()));
    frame[network + 6U] = next_header;
    frame[network + 7U] = 64U;
    const auto source = ipv6_address(0x10U);
    const auto destination = ipv6_address(0x20U);
    std::memcpy(
        frame.data() + network + 8U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + network + 24U,
        destination.data(),
        destination.size());
    std::copy(
        payload.begin(),
        payload.end(),
        frame.begin() + ethernet_length + ipv6_length);
    return frame;
}

std::vector<uint8_t> make_ipv6_udp_frame()
{
    std::vector<uint8_t> udp(9U, 0U);
    write_be16(&udp, 0U, 50000U);
    write_be16(&udp, 2U, 443U);
    write_be16(&udp, 4U, static_cast<uint16_t>(udp.size()));
    udp.back() = 0x40U;
    return make_ipv6_frame(17U, udp);
}

std::vector<uint8_t> make_ipv6_non_initial_fragment()
{
    std::vector<uint8_t> fragment(16U, 0U);
    fragment[0U] = 17U;
    write_be16(&fragment, 2U, 8U);
    return make_ipv6_frame(44U, fragment);
}

void append_le16(std::vector<uint8_t> *data, uint16_t value)
{
    data->push_back(static_cast<uint8_t>(value & 0xffU));
    data->push_back(static_cast<uint8_t>(value >> 8U));
}

void append_le32(std::vector<uint8_t> *data, uint32_t value)
{
    data->push_back(static_cast<uint8_t>(value & 0xffU));
    data->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    data->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    data->push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
}

std::string write_test_pcap_frames(
    const std::vector<std::vector<uint8_t>> &frames,
    const std::string &suffix)
{
    std::vector<uint8_t> bytes;
    append_le32(&bytes, 0xa1b2c3d4U);
    append_le16(&bytes, 2U);
    append_le16(&bytes, 4U);
    append_le32(&bytes, 0U);
    append_le32(&bytes, 0U);
    append_le32(&bytes, 65535U);
    append_le32(&bytes, 1U);
    for (size_t index = 0U; index < frames.size(); ++index) {
        const auto &frame = frames[index];
        append_le32(&bytes, static_cast<uint32_t>(index + 1U));
        append_le32(&bytes, 0U);
        append_le32(&bytes, static_cast<uint32_t>(frame.size()));
        append_le32(&bytes, static_cast<uint32_t>(frame.size()));
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }

    const std::string path =
        "/tmp/ktag_schema_pipeline_" + std::to_string(getpid()) + "_" +
        suffix + ".pcap";
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    return path;
}

std::string write_test_pcap()
{
    return write_test_pcap_frames(
        {make_proxy_test_udp_frame()}, "ipv4");
}

std::string write_truncated_test_pcap()
{
    const std::string path = write_test_pcap_frames(
        {make_proxy_test_udp_frame()}, "truncated");
    std::vector<uint8_t> truncated_record;
    append_le32(&truncated_record, 2U);
    append_le32(&truncated_record, 0U);
    append_le32(&truncated_record, 64U);
    append_le32(&truncated_record, 64U);
    truncated_record.insert(
        truncated_record.end(), {0x01U, 0x02U, 0x03U});
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.write(
        reinterpret_cast<const char *>(truncated_record.data()),
        static_cast<std::streamsize>(truncated_record.size()));
    output.close();
    return path;
}

std::string read_text_file(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

ktag_demo_pipeline_prediction make_prediction(
    bool ipv6,
    int64_t label,
    float confidence,
    uint64_t flow_id)
{
    ktag_demo_pipeline_prediction prediction;
    auto &details = prediction.details;
    details.struct_size = sizeof(details);
    details.api_version = KTAG_API_VERSION;
    auto &metadata = details.metadata;
    metadata.struct_size = sizeof(metadata);
    metadata.api_version = KTAG_API_VERSION;
    metadata.flow_id = flow_id;
    metadata.first_packet_sequence = flow_id * 10U;
    metadata.last_packet_sequence = flow_id * 10U + 3U;
    details.source_port = 50000U;
    details.destination_port = 443U;
    metadata.flow_key.port_a = 443U;
    metadata.flow_key.port_b = 50000U;
    metadata.flow_key.protocol = 17U;

    if (ipv6) {
        details.source_address.version = 6U;
        details.destination_address.version = 6U;
        EXPECT_EQ(
            1,
            inet_pton(
                AF_INET6,
                "2001:db8::2",
                details.source_address.bytes));
        EXPECT_EQ(
            1,
            inet_pton(
                AF_INET6,
                "2001:db8::1",
                details.destination_address.bytes));
        metadata.flow_key.address_a = details.destination_address;
        metadata.flow_key.address_b = details.source_address;
    } else {
        details.source_address.version = 4U;
        details.destination_address.version = 4U;
        EXPECT_EQ(
            1,
            inet_pton(
                AF_INET,
                "198.51.100.10",
                details.source_address.bytes));
        EXPECT_EQ(
            1,
            inet_pton(
                AF_INET,
                "203.0.113.20",
                details.destination_address.bytes));
        metadata.flow_key.address_a = details.source_address;
        metadata.flow_key.address_b = details.destination_address;
    }

    prediction.record.struct_size = sizeof(prediction.record);
    prediction.record.api_version = KTAG_API_VERSION;
    prediction.record.metadata.struct_size =
        sizeof(prediction.record.metadata);
    prediction.record.metadata.api_version = KTAG_API_VERSION;
    prediction.record.metadata.flow_id = metadata.flow_id;
    prediction.record.metadata.flow_key = metadata.flow_key;
    prediction.record.metadata.first_packet_sequence =
        metadata.first_packet_sequence;
    prediction.record.metadata.last_packet_sequence =
        metadata.last_packet_sequence;
    prediction.record.label = label;
    prediction.record.confidence = confidence;
    return prediction;
}

bool write_contract_package(
    const fs::path &package,
    const ktag_feature_schema *schema,
    const std::unordered_set<std::string> &excluded)
{
    std::error_code ignored;
    fs::remove_all(package, ignored);
    ignored.clear();
    if (!fs::create_directories(package, ignored) || ignored) {
        return false;
    }
    struct input_feature {
        std::string id;
        uint32_t dimension;
    };
    std::vector<input_feature> features;
    uint32_t value_count = 0U;
    const uint32_t count = ktag_feature_schema_get_feature_count(schema);
    for (uint32_t index = 0U; index < count; ++index) {
        ktag_feature_descriptor descriptor{};
        uint32_t value_offset = 0U;
        if (ktag_feature_schema_get_descriptor(
                schema, index, &descriptor, &value_offset) != KTAG_SUCCESS) {
            return false;
        }
        if (excluded.count(descriptor.feature_id) != 0U) {
            continue;
        }
        features.push_back({descriptor.feature_id, descriptor.dimension});
        value_count += descriptor.dimension;
    }

    std::ostringstream hash;
    hash << "0x" << std::hex << ktag_feature_schema_get_hash(schema);
    std::ofstream model(package / "model.onnx", std::ios::binary);
    model << "contract preflight does not open this model";
    std::ofstream manifest(package / "manifest.json");
    manifest << "{\n  \"model_name\": \"pipeline-contract-test\",\n"
             << "  \"model_version\": \"1\",\n"
             << "  \"schema_hash\": \"" << hash.str() << "\",\n"
             << "  \"input_features\": [";
    for (size_t index = 0U; index < features.size(); ++index) {
        manifest << (index == 0U ? "" : ",") << "\n    {\"feature_id\": \""
                 << features[index].id << "\", \"dimension\": "
                 << features[index].dimension << '}';
    }
    manifest << "\n  ],\n  \"preprocessing\": {\n"
             << "    \"minimum_total_packets\": 0,\n"
             << "    \"missing_value_policy\": \"reject\",\n"
             << "    \"scaler\": \"scaler.json\"\n"
             << "  },\n  \"model\": \"model.onnx\"\n}\n";

    std::ofstream scaler(package / "scaler.json");
    scaler << "{\n  \"schema_hash\": \"" << hash.str() << "\",\n"
           << "  \"input_features\": [";
    for (size_t index = 0U; index < features.size(); ++index) {
        scaler << (index == 0U ? "" : ", ") << '"'
               << features[index].id << '"';
    }
    scaler << "],\n  \"mean\": [";
    for (uint32_t index = 0U; index < value_count; ++index) {
        scaler << (index == 0U ? "" : ",") << '0';
    }
    scaler << "],\n  \"scale\": [";
    for (uint32_t index = 0U; index < value_count; ++index) {
        scaler << (index == 0U ? "" : ",") << '1';
    }
    scaler << "]\n}\n";
    return static_cast<bool>(model) && static_cast<bool>(manifest) &&
        static_cast<bool>(scaler);
}

ktag_feature_schema *make_proxy_v1_schema()
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *legacy = ktag_feature_schema_create_legacy_v1();
    ktag_feature_schema *schema =
        ktag_feature_schema_create("ktag_proxy_v1", 1U);
    const auto plugin = query_proxy_plugin();
    if (registry == nullptr || legacy == nullptr || schema == nullptr ||
        ktag_traffic_features_register(registry) !=
            KTAG_SUCCESS ||
        plugin.register_providers == nullptr ||
        plugin.register_providers(nullptr, registry) != KTAG_SUCCESS) {
        ktag_feature_schema_destroy(schema);
        ktag_feature_schema_destroy(legacy);
        ktag_feature_registry_destroy(registry);
        return nullptr;
    }
    const std::unordered_set<std::string> excluded{
        "legacy.flow_id", "network.src_ip", "network.dst_ip"};
    bool success = true;
    const uint32_t count = ktag_feature_schema_get_feature_count(legacy);
    for (uint32_t index = 0U; index < count && success; ++index) {
        ktag_feature_descriptor descriptor{};
        uint32_t value_offset = 0U;
        success = ktag_feature_schema_get_descriptor(
                      legacy, index, &descriptor, &value_offset) ==
                KTAG_SUCCESS &&
            (excluded.count(descriptor.feature_id) != 0U ||
             ktag_feature_schema_add_registered(
                 schema, registry, descriptor.feature_id) == KTAG_SUCCESS);
    }
    success = success && add_proxy_catalog(schema, registry) &&
        ktag_feature_schema_freeze(schema) == KTAG_SUCCESS;
    ktag_feature_schema_destroy(legacy);
    ktag_feature_registry_destroy(registry);
    if (!success) {
        ktag_feature_schema_destroy(schema);
        return nullptr;
    }
    return schema;
}

TEST(KTAGDemoPipelineQueue, AppliesAllCapacityPolicies)
{
    ktag_demo_bounded_queue<int> drop_oldest(2U);
    EXPECT_EQ(
        ktag_demo_queue_push_result::pushed,
        drop_oldest.push(1, ktag_demo_backpressure_policy::drop_oldest));
    EXPECT_EQ(
        ktag_demo_queue_push_result::pushed,
        drop_oldest.push(2, ktag_demo_backpressure_policy::drop_oldest));
    EXPECT_EQ(
        ktag_demo_queue_push_result::pushed_after_dropping_oldest,
        drop_oldest.push(3, ktag_demo_backpressure_policy::drop_oldest));
    int value = 0;
    ASSERT_TRUE(drop_oldest.pop(&value));
    EXPECT_EQ(2, value);
    ASSERT_TRUE(drop_oldest.pop(&value));
    EXPECT_EQ(3, value);
    EXPECT_EQ(1U, drop_oldest.statistics().dropped_oldest);

    ktag_demo_bounded_queue<int> drop_newest(1U);
    EXPECT_EQ(
        ktag_demo_queue_push_result::pushed,
        drop_newest.push(4, ktag_demo_backpressure_policy::drop_newest));
    EXPECT_EQ(
        ktag_demo_queue_push_result::dropped_newest,
        drop_newest.push(5, ktag_demo_backpressure_policy::drop_newest));
    EXPECT_EQ(1U, drop_newest.statistics().dropped_newest);

    ktag_demo_bounded_queue<int> fail(1U);
    EXPECT_EQ(
        ktag_demo_queue_push_result::pushed,
        fail.push(6, ktag_demo_backpressure_policy::fail));
    EXPECT_EQ(
        ktag_demo_queue_push_result::failed,
        fail.push(7, ktag_demo_backpressure_policy::fail));
    EXPECT_EQ(1U, fail.statistics().failures);
}

TEST(KTAGDemoPipelineQueue, BlockPolicyWakesWhenConsumerMakesRoom)
{
    ktag_demo_bounded_queue<int> queue(1U);
    ASSERT_EQ(
        ktag_demo_queue_push_result::pushed,
        queue.push(1, ktag_demo_backpressure_policy::block));
    ktag_demo_queue_push_result producer_result =
        ktag_demo_queue_push_result::failed;
    std::atomic<bool> producer_started{false};
    std::thread producer([&queue, &producer_result, &producer_started]() {
        producer_started.store(true, std::memory_order_release);
        producer_result = queue.push(
            2, ktag_demo_backpressure_policy::block);
    });
    while (!producer_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    uint32_t attempts = 0U;
    while (queue.statistics().block_count == 0U && attempts < 100000U) {
        std::this_thread::yield();
        ++attempts;
    }
    EXPECT_EQ(1U, queue.statistics().block_count);
    int value = 0;
    ASSERT_TRUE(queue.pop(&value));
    producer.join();
    EXPECT_EQ(1, value);
    EXPECT_EQ(ktag_demo_queue_push_result::pushed, producer_result);
    EXPECT_EQ(1U, queue.statistics().block_count);
}

TEST(KTAGDemoPipelineQueue, CloseDrainsExistingItemsAndRejectsNewPushes)
{
    ktag_demo_bounded_queue<int> queue(2U);
    ASSERT_EQ(
        ktag_demo_queue_push_result::pushed,
        queue.push(10, ktag_demo_backpressure_policy::block));
    ASSERT_EQ(
        ktag_demo_queue_push_result::pushed,
        queue.push(11, ktag_demo_backpressure_policy::block));
    queue.close();
    EXPECT_EQ(
        ktag_demo_queue_push_result::closed,
        queue.push(12, ktag_demo_backpressure_policy::block));
    int value = 0;
    ASSERT_TRUE(queue.pop(&value));
    EXPECT_EQ(10, value);
    ASSERT_TRUE(queue.pop(&value));
    EXPECT_EQ(11, value);
    EXPECT_FALSE(queue.pop(&value));
    const auto statistics = queue.statistics();
    EXPECT_TRUE(statistics.closed);
    EXPECT_EQ(0U, statistics.current_depth);
}

TEST(KTAGDemoPipelineConfig, DisabledConfigPreservesLegacyPath)
{
    const std::string path = write_config("model_path_onnx: old.onnx\n");
    ktag_demo_pipeline_config config;
    std::string error;
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::disabled,
        ktag_demo_load_pipeline_config(path.c_str(), &config, &error));
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, NormalizesFeatureParallelThreadCounts)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: true\n"
        "  mode: feature_parallel\n"
        "  model_package_path: model_package\n"
        "  feature:\n"
        "    threads: 4\n"
        "  inference:\n"
        "    threads: 9\n"
        "  backpressure:\n"
        "    policy: block\n");
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config(path.c_str(), &config, &error))
        << error;
    EXPECT_EQ(4U, config.feature_threads);
    EXPECT_EQ(1U, config.inference_threads);
    EXPECT_EQ(
        ktag_demo_feature_lifecycle_mode::legacy_compat,
        config.lifecycle_mode);
    EXPECT_FALSE(config.model_package_path.empty());
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, ParsesModernFeatureLifecycleExplicitly)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: true\n"
        "  mode: single_thread\n"
        "  model_package_path: model_package\n"
        "  feature:\n"
        "    lifecycle_mode: modern\n");
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config(path.c_str(), &config, &error))
        << error;
    EXPECT_EQ(
        ktag_demo_feature_lifecycle_mode::modern,
        config.lifecycle_mode);
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, NullOptionalFieldsUseDefaults)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: true\n"
        "  mode: single_thread\n"
        "  model_package_path: model_package\n"
        "  feature:\n"
        "    lifecycle_mode:\n"
        "  backpressure:\n"
        "    policy:\n"
        "  schema:\n"
        "    feature_order:\n"
        "    remove_features:\n"
        "    add_features:\n");
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config(path.c_str(), &config, &error))
        << error;
    EXPECT_EQ(
        ktag_demo_feature_lifecycle_mode::legacy_compat,
        config.lifecycle_mode);
    EXPECT_EQ(
        ktag_demo_backpressure_policy::block,
        config.backpressure);
    EXPECT_TRUE(config.feature_order.empty());
    EXPECT_TRUE(config.remove_features.empty());
    EXPECT_TRUE(config.add_features.empty());
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, CommandLineOverridesForceDisabledPipeline)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: staged\n"
        "  model_package_path: model_package\n"
        "  feature:\n"
        "    threads: 7\n"
        "  inference:\n"
        "    threads: 3\n");
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.mode_set = true;
    overrides.mode = ktag_demo_pipeline_mode::full_parallel;
    overrides.feature_threads = 5U;
    overrides.inference_threads = 2U;
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error))
        << error;
    EXPECT_EQ(ktag_demo_pipeline_mode::full_parallel, config.mode);
    EXPECT_EQ(5U, config.feature_threads);
    EXPECT_EQ(2U, config.inference_threads);
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, RejectsThreadsIncompatibleWithSelectedMode)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: true\n"
        "  mode: feature_parallel\n"
        "  model_package_path: model_package\n");
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.mode_set = true;
    overrides.mode = ktag_demo_pipeline_mode::staged;
    overrides.feature_threads = 4U;
    ktag_demo_pipeline_config config;
    std::string error;
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_FALSE(error.empty());
    std::remove(path.c_str());
}

TEST(KTAGDemoFeaturePlugin, RejectsInvalidPluginsAndUnloadsValidPlugin)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ktag_demo_feature_plugin_loader loader;
    std::string error;
    EXPECT_FALSE(loader.load(BAD_ABI_PLUGIN, registry, &error));
    EXPECT_FALSE(loader.load(CREATE_FAIL_PLUGIN, registry, &error));
    EXPECT_FALSE(loader.load(NO_ENTRY_PLUGIN, registry, &error));
    ASSERT_TRUE(loader.load(EXAMPLE_PLUGIN, registry, &error)) << error;
    EXPECT_EQ(1U, loader.size());
    EXPECT_EQ("customer.example.plugin", loader.plugin_id(0U));
    EXPECT_FALSE(loader.load(EXAMPLE_PLUGIN, registry, &error));

    /* Registry owns callback copies and must die before dlclose. */
    ktag_feature_registry_destroy(registry);
    loader.unload_all();
    EXPECT_EQ(0U, loader.size());
}

TEST(KTAGDemoPipelineConfig, FeatureOnlyDoesNotRequireModelOrInference)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: full_parallel\n"
        "  model_package_path: [ignored, in, feature, mode]\n"
        "  feature:\n"
        "    threads: 3\n"
        "    lifecycle_mode: modern\n"
        "  inference: ignored\n"
        "  schema:\n"
        "    name: ktag_legacy_v1\n"
        "    version: 1\n");
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.feature_only = true;
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error))
        << error;
    EXPECT_TRUE(config.feature_only);
    EXPECT_TRUE(config.model_package_path.empty());
    EXPECT_EQ(3U, config.feature_threads);
    EXPECT_EQ(3U, config.inference_threads);
    EXPECT_EQ(
        ktag_demo_feature_lifecycle_mode::modern,
        config.lifecycle_mode);

    ktag_demo_pipeline_config inference_config;
    ktag_demo_pipeline_config_overrides inference_overrides;
    inference_overrides.force_enable = true;
    error.clear();
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(),
            &inference_overrides,
            &inference_config,
            &error));
    EXPECT_FALSE(error.empty());
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, ParsesSharedTopologyForDistributedFrontend)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: feature_parallel\n"
        "  feature:\n"
        "    threads: 3\n"
        "  backpressure:\n"
        "    policy: block\n"
        "  distributed:\n"
        "    ub_ring:\n"
        "      enabled: false\n"
        "      provider_nodes: [fe-01, fe-02, fe-03]\n"
        "      user_nodes: [be-01]\n"
        "      region: ktag-test-region\n"
        "      shm_prefix: ktag-test-feature\n"
        "      provider_shm_size: 134217728\n"
        "      queues_per_provider: 1\n"
        "      queue_depth: 1024\n"
        "      data_element_size: 4096\n"
        "      credits_per_provider: 256\n"
        "      poll_us: 50\n"
        "      ground_truth_labels:\n"
        "        fe-01: [0, 3]\n"
        "        fe-02: [1, 4]\n"
        "        fe-03: [2]\n"
        "      eal_arguments: [kdadk_demo, --no-huge, -l, \"0\"]\n"
        "  schema:\n"
        "    name: ktag_legacy_v1\n"
        "    version: 1\n");
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.distributed_role_set = true;
    overrides.distributed_role = ktag_demo_distributed_role::frontend;
    overrides.node_id = "fe-02";
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error))
        << error;
    EXPECT_TRUE(config.feature_only);
    EXPECT_TRUE(config.model_package_path.empty());
    EXPECT_TRUE(config.ub.configured);
    EXPECT_TRUE(config.ub.enabled);
    EXPECT_EQ(ktag_demo_distributed_role::frontend, config.ub.role);
    EXPECT_EQ("fe-02", config.ub.node_id);
    ASSERT_EQ(3U, config.ub.provider_nodes.size());
    ASSERT_EQ(1U, config.ub.user_nodes.size());
    EXPECT_EQ("be-01", config.ub.user_nodes.front());
    EXPECT_EQ(134217728U, config.ub.provider_shm_size);
    EXPECT_EQ(256U, config.ub.credits_per_provider);
    ASSERT_EQ(3U, config.ub.ground_truth_labels.size());
    EXPECT_EQ((std::vector<uint32_t>{1U, 4U}),
              config.ub.ground_truth_labels.at("fe-02"));
    ASSERT_EQ(4U, config.ub.eal_arguments.size());
    EXPECT_EQ("0", config.ub.eal_arguments.back());
    EXPECT_NE(0U, config.ub.ruleset_hash);
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, ParsesDistributedBackendWithModel)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: feature_parallel\n"
        "  model_package_path: model_package\n"
        "  inference:\n"
        "    threads: 1\n"
        "    batch_timeout_us: 2500\n"
        "  backpressure:\n"
        "    policy: block\n"
        "  distributed:\n"
        "    ub_ring:\n"
        "      provider_nodes: [fe-01, fe-02, fe-03]\n"
        "      user_nodes: [be-01]\n"
        "      queue_depth: 1024\n"
        "      data_element_size: 4096\n"
        "      credits_per_provider: 256\n");
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.distributed_role_set = true;
    overrides.distributed_role = ktag_demo_distributed_role::backend;
    overrides.node_id = "be-01";
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error))
        << error;
    EXPECT_FALSE(config.feature_only);
    EXPECT_FALSE(config.model_package_path.empty());
    EXPECT_EQ(ktag_demo_distributed_role::backend, config.ub.role);
    EXPECT_EQ("be-01", config.ub.node_id);
    EXPECT_EQ(2500U, config.inference_batch_timeout_us);
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, RejectsDistributedRoleOrBackpressureMismatch)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: feature_parallel\n"
        "  feature:\n"
        "    threads: 3\n"
        "  backpressure:\n"
        "    policy: drop_newest\n"
        "  distributed:\n"
        "    ub_ring:\n"
        "      provider_nodes: [fe-01, fe-02, fe-03]\n"
        "      user_nodes: [be-01]\n"
        "      queue_depth: 1024\n"
        "      data_element_size: 4096\n"
        "      credits_per_provider: 256\n");
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.distributed_role_set = true;
    overrides.distributed_role = ktag_demo_distributed_role::frontend;
    overrides.node_id = "be-01";
    ktag_demo_pipeline_config config;
    std::string error;
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("selected distributed role"));

    overrides.node_id = "fe-01";
    error.clear();
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("requires pipeline.backpressure"));
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, RestrictsFirstDistributedFrontendTopology)
{
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.distributed_role_set = true;
    overrides.distributed_role = ktag_demo_distributed_role::frontend;
    overrides.node_id = "fe-01";
    ktag_demo_pipeline_config config;
    std::string error;

    std::string path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: full_parallel\n"
        "  backpressure: {policy: block}\n"
        "  distributed:\n"
        "    ub_ring:\n"
        "      provider_nodes: [fe-01]\n"
        "      user_nodes: [be-01]\n"
        "      queues_per_provider: 1\n"
        "      queue_depth: 1024\n"
        "      data_element_size: 4096\n"
        "      credits_per_provider: 256\n");
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("feature_parallel"));

    path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: feature_parallel\n"
        "  backpressure: {policy: block}\n"
        "  distributed:\n"
        "    ub_ring:\n"
        "      provider_nodes: [fe-01]\n"
        "      user_nodes: [be-01]\n"
        "      queues_per_provider: 2\n"
        "      queue_depth: 1024\n"
        "      data_element_size: 4096\n"
        "      credits_per_provider: 256\n");
    error.clear();
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("one UB queue"));
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, RejectsIncompleteGroundTruthTopology)
{
    const std::string path = write_config(
        "pipeline:\n"
        "  enabled: false\n"
        "  mode: feature_parallel\n"
        "  backpressure: {policy: block}\n"
        "  distributed:\n"
        "    ub_ring:\n"
        "      provider_nodes: [fe-01, fe-02]\n"
        "      user_nodes: [be-01]\n"
        "      queues_per_provider: 1\n"
        "      queue_depth: 1024\n"
        "      data_element_size: 4096\n"
        "      credits_per_provider: 256\n"
        "      ground_truth_labels:\n"
        "        fe-01: [0]\n");
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.distributed_role_set = true;
    overrides.distributed_role = ktag_demo_distributed_role::frontend;
    overrides.node_id = "fe-01";
    ktag_demo_pipeline_config config;
    std::string error;
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("every Provider"));
    std::remove(path.c_str());
}

TEST(KTAGDemoPipelineConfig, RejectsMutationOfFrozenOfficialSchemaNames)
{
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.feature_only = true;
    ktag_demo_pipeline_config config;
    std::string error;

    std::string path = write_config(
        "pipeline:\n"
        "  enabled: true\n"
        "  plugins: [/tmp/libktag_proxy_features.so]\n"
        "  schema:\n"
        "    name: ktag_proxy_v1\n"
        "    version: 1\n"
        "    remove_features:\n"
        "      - legacy.flow_id\n"
        "      - network.src_ip\n"
        "      - network.dst_ip\n"
        "    add_features: [proxy.incomplete]\n");
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("68"));

    path = write_config(
        "pipeline:\n"
        "  enabled: true\n"
        "  plugins: [/tmp/libktag_proxy_features.so]\n"
        "  schema:\n"
        "    name: proxy_endpoint_ablation_v1\n"
        "    version: 1\n"
        "    remove_features:\n"
        "      - legacy.flow_id\n"
        "      - network.src_ip\n"
        "      - network.dst_ip\n"
        "    add_features: [proxy.incomplete]\n");
    error.clear();
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("frozen ablation"));

    path = write_config(
        "pipeline:\n"
        "  enabled: true\n"
        "  schema:\n"
        "    name: ktag_legacy_v1\n"
        "    version: 1\n"
        "    add_features: [customer.feature]\n");
    error.clear();
    EXPECT_EQ(
        ktag_demo_pipeline_config_result::error,
        ktag_demo_load_pipeline_config_with_overrides(
            path.c_str(), &overrides, &config, &error));
    EXPECT_NE(std::string::npos, error.find("frozen"));
    std::remove(path.c_str());
}

TEST(KTAGDemoOnlineInferenceContract, PreflightsApplicationAndProxySeparately)
{
    const fs::path root = fs::temp_directory_path() /
        ("ktag_online_contract_" + std::to_string(getpid()));
    ktag_feature_schema *application =
        ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, application);
    ASSERT_EQ(
        0xdd48882b53bebfa5ULL,
        ktag_feature_schema_get_hash(application));
    const fs::path application_package = root / "application";
    ASSERT_TRUE(write_contract_package(
        application_package,
        application,
        {"legacy.flow_id", "network.src_ip", "network.dst_ip"}));

    ktag_demo_pipeline_config application_config;
    application_config.enabled = true;
    application_config.mode = ktag_demo_pipeline_mode::single_thread;
    application_config.feature_threads = 1U;
    application_config.inference_threads = 1U;
    application_config.model_package_path = application_package.string();
    application_config.plugin_paths = {TRAFFIC_PLUGIN};
    const std::string application_output =
        (root / "application.csv").string();
    EXPECT_EQ(
        0,
        ktag_demo_run_pipeline(
            application_config,
            "functest-application",
            nullptr,
            0,
            nullptr,
            application_output.c_str(),
            ktag_demo_pipeline_output_format::csv));

    const fs::path invalid_application_package = root / "application_invalid";
    ASSERT_TRUE(write_contract_package(
        invalid_application_package,
        application,
        {
            "legacy.flow_id",
            "network.src_ip",
            "network.dst_ip",
            "network.protocol",
        }));
    application_config.model_package_path =
        invalid_application_package.string();
    EXPECT_NE(
        0,
        ktag_demo_run_pipeline(
            application_config,
            "functest-application-invalid",
            nullptr,
            0,
            nullptr,
            application_output.c_str(),
            ktag_demo_pipeline_output_format::csv));
    ktag_feature_schema_destroy(application);

    ktag_feature_schema *proxy = make_proxy_v1_schema();
    ASSERT_NE(nullptr, proxy);
    ASSERT_EQ(118U, ktag_feature_schema_get_feature_count(proxy));
    ASSERT_EQ(619U, ktag_feature_schema_get_value_count(proxy));
    ASSERT_EQ(
        0xca1eae5086b41ee5ULL,
        ktag_feature_schema_get_hash(proxy));
    const fs::path proxy_package = root / "proxy";
    ASSERT_TRUE(write_contract_package(proxy_package, proxy, {}));

    ktag_demo_pipeline_config proxy_config;
    proxy_config.enabled = true;
    proxy_config.mode = ktag_demo_pipeline_mode::single_thread;
    proxy_config.feature_threads = 1U;
    proxy_config.inference_threads = 1U;
    proxy_config.model_package_path = proxy_package.string();
    proxy_config.schema_name = "ktag_proxy_v1";
    proxy_config.schema_version = 1U;
    proxy_config.plugin_paths = {TRAFFIC_PLUGIN, PROXY_PLUGIN};
    proxy_config.remove_features = {
        "legacy.flow_id", "network.src_ip", "network.dst_ip"};
    add_proxy_feature_ids(&proxy_config);
    const std::string proxy_output = (root / "proxy.csv").string();
    EXPECT_EQ(
        0,
        ktag_demo_run_pipeline(
            proxy_config,
            "functest-proxy",
            nullptr,
            0,
            nullptr,
            proxy_output.c_str(),
            ktag_demo_pipeline_output_format::csv));
    ktag_feature_schema_destroy(proxy);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

TEST(KTAGDemoSchemaFeatureWriter, ExpandsSchemaAndQuotesIpv6Metadata)
{
    ktag_feature_schema *schema =
        ktag_feature_schema_create("writer_test_v1", 1U);
    ASSERT_NE(nullptr, schema);
    ktag_feature_descriptor scalar{};
    scalar.struct_size = sizeof(scalar);
    scalar.api_version = KTAG_API_VERSION;
    scalar.feature_id = "test.scalar";
    scalar.display_name = "Scalar";
    scalar.provider_id = "test.writer";
    scalar.provider_version = 1U;
    scalar.value_type = KTAG_FEATURE_FLOAT32;
    scalar.dimension = 1U;
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema, &scalar));
    ktag_feature_descriptor vector = scalar;
    vector.feature_id = "test.vector";
    vector.display_name = "Vector";
    vector.dimension = 2U;
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_add(schema, &vector));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));

    ktag_demo_schema_feature_record record;
    record.details.struct_size = sizeof(record.details);
    record.details.api_version = KTAG_API_VERSION;
    auto &metadata = record.details.metadata;
    metadata.struct_size = sizeof(metadata);
    metadata.api_version = KTAG_API_VERSION;
    metadata.flow_id = 7U;
    metadata.first_packet_sequence = 11U;
    metadata.last_packet_sequence = 19U;
    metadata.flow_key.address_a.version = 6U;
    metadata.flow_key.address_b.version = 6U;
    ASSERT_EQ(
        1,
        inet_pton(
            AF_INET6,
            "2001:db8::1",
            metadata.flow_key.address_a.bytes));
    ASSERT_EQ(
        1,
        inet_pton(
            AF_INET6,
            "2001:db8::2",
            metadata.flow_key.address_b.bytes));
    metadata.flow_key.port_a = 443U;
    metadata.flow_key.port_b = 50000U;
    metadata.flow_key.protocol = 6U;
    record.details.source_address = metadata.flow_key.address_b;
    record.details.destination_address = metadata.flow_key.address_a;
    record.details.source_port = 50000U;
    record.details.destination_port = 443U;
    record.details.ipv6_extension_packet_count = 2U;
    record.details.ip_fragment_first_packet_count = 1U;
    record.values = {1.25F, 2.0F, 3.0F};

    const std::string output_path =
        "/tmp/ktag_schema_writer_" + std::to_string(getpid()) + ".csv";
    std::string error;
    ASSERT_TRUE(ktag_demo_write_schema_feature_csv(
        output_path.c_str(), schema, {record}, &error)) << error;
    std::ifstream input(output_path);
    ASSERT_TRUE(input.good());
    std::string header;
    std::string row;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, header)));
    ASSERT_TRUE(static_cast<bool>(std::getline(input, row)));
    EXPECT_EQ(
        "meta.flow_id,meta.ip_version,meta.src_ip,meta.dst_ip,"
        "meta.src_port,meta.dst_port,meta.protocol,meta.flow_key,"
        "meta.ipv6_extension_packet_count,"
        "meta.ip_fragment_first_packet_count,"
        "meta.first_packet_sequence,meta.last_packet_sequence,"
        "test.scalar,test.vector[0],test.vector[1]",
        header);
    EXPECT_NE(std::string::npos, row.find("\"2001:db8::2\""));
    EXPECT_NE(
        std::string::npos,
        row.find(
            "\"[2001:db8::1]:443-[2001:db8::2]:50000/6\""));
    EXPECT_NE(std::string::npos, row.find(",2,1,11,19,1.25,2,3"));

    std::remove(output_path.c_str());
    ktag_feature_schema_destroy(schema);
}

TEST(KTAGDemoUbFeatureWire, RoundTripsFeatureRecordAndContract)
{
    ktag_demo_schema_feature_record input;
    input.source_index = 7U;
    input.details.struct_size = sizeof(input.details);
    input.details.api_version = KTAG_API_VERSION;
    auto &metadata = input.details.metadata;
    metadata.struct_size = sizeof(metadata);
    metadata.api_version = KTAG_API_VERSION;
    metadata.flow_id = 0x1122334455667788ULL;
    metadata.flow_key.struct_size = sizeof(metadata.flow_key);
    metadata.flow_key.api_version = KTAG_API_VERSION;
    metadata.flow_key.address_a.version = 6U;
    metadata.flow_key.address_b.version = 4U;
    ASSERT_EQ(1, inet_pton(AF_INET6, "2001:db8::10",
                           metadata.flow_key.address_a.bytes));
    ASSERT_EQ(1, inet_pton(AF_INET, "192.0.2.20",
                           metadata.flow_key.address_b.bytes));
    metadata.flow_key.port_a = 443U;
    metadata.flow_key.port_b = 51000U;
    metadata.flow_key.protocol = 6U;
    metadata.first_packet_sequence = 19U;
    metadata.last_packet_sequence = 91U;
    input.details.source_address = metadata.flow_key.address_b;
    input.details.destination_address = metadata.flow_key.address_a;
    input.details.source_port = metadata.flow_key.port_b;
    input.details.destination_port = metadata.flow_key.port_a;
    input.details.ipv6_extension_packet_count = 3U;
    input.details.ip_fragment_first_packet_count = 2U;
    input.values = {1.25F, -2.5F, 0.0F, 99.75F};

    ktag_demo_ub_frame_contract contract;
    contract.frontend_id_hash = 0xa1U;
    contract.epoch = 5U;
    contract.sequence = 17U;
    contract.schema_hash = 0xdd48882b53bebfa5ULL;
    contract.ruleset_hash = 0xbbU;

    std::vector<uint8_t> buffer(
        ktag_demo_ub_feature_frame_size(input.values.size()));
    size_t encoded_size = 0U;
    std::string error;
    ASSERT_TRUE(ktag_demo_ub_encode_feature_frame(
        buffer.data(), buffer.size(), contract, input, true, &encoded_size,
        &error)) << error;
    EXPECT_EQ(buffer.size(), encoded_size);

    ktag_demo_ub_frame_info info;
    ktag_demo_schema_feature_record output;
    ASSERT_TRUE(ktag_demo_ub_decode_feature_frame(
        buffer.data(), buffer.size(), &info, &output, &error)) << error;
    EXPECT_EQ(ktag_demo_ub_message_type::feature, info.message_type);
    EXPECT_EQ(KTAG_DEMO_UB_FRAME_FLAG_GROUND_TRUTH_VALID, info.flags);
    EXPECT_EQ(contract.frontend_id_hash, info.contract.frontend_id_hash);
    EXPECT_EQ(contract.epoch, info.contract.epoch);
    EXPECT_EQ(contract.sequence, info.contract.sequence);
    EXPECT_EQ(contract.schema_hash, info.contract.schema_hash);
    EXPECT_EQ(contract.ruleset_hash, info.contract.ruleset_hash);
    EXPECT_EQ(input.source_index, output.source_index);
    EXPECT_EQ(metadata.flow_id, output.details.metadata.flow_id);
    EXPECT_EQ(metadata.flow_key.port_a,
              output.details.metadata.flow_key.port_a);
    EXPECT_EQ(metadata.flow_key.port_b,
              output.details.metadata.flow_key.port_b);
    EXPECT_EQ(metadata.flow_key.protocol,
              output.details.metadata.flow_key.protocol);
    EXPECT_EQ(0, std::memcmp(metadata.flow_key.address_a.bytes,
                             output.details.metadata.flow_key.address_a.bytes,
                             KTAG_IPV6_ADDRESS_SIZE));
    EXPECT_EQ(input.details.source_port, output.details.source_port);
    EXPECT_EQ(input.details.destination_port, output.details.destination_port);
    EXPECT_EQ(input.details.ipv6_extension_packet_count,
              output.details.ipv6_extension_packet_count);
    EXPECT_EQ(input.values, output.values);
}

TEST(KTAGDemoUbFeatureWire, ValidatesControlAndRejectsCorruption)
{
    ktag_demo_ub_frame_contract contract;
    contract.frontend_id_hash = 3U;
    contract.epoch = 9U;
    contract.schema_hash = 11U;
    contract.ruleset_hash = 13U;
    std::array<uint8_t, KTAG_DEMO_UB_FRAME_HEADER_SIZE> control{};
    size_t encoded_size = 0U;
    std::string error;
    ASSERT_TRUE(ktag_demo_ub_encode_control_frame(
        control.data(), control.size(), ktag_demo_ub_message_type::credit,
        contract, &encoded_size, &error)) << error;
    ktag_demo_ub_frame_info info;
    ASSERT_TRUE(ktag_demo_ub_inspect_frame(
        control.data(), control.size(), &info, &error)) << error;
    EXPECT_EQ(ktag_demo_ub_message_type::credit, info.message_type);
    EXPECT_EQ(contract.schema_hash, info.contract.schema_hash);

    ktag_demo_schema_feature_record record{};
    record.values = {1.0F, 2.0F};
    std::vector<uint8_t> feature(
        ktag_demo_ub_feature_frame_size(record.values.size()));
    ASSERT_TRUE(ktag_demo_ub_encode_feature_frame(
        feature.data(), feature.size(), contract, record, false,
        &encoded_size, &error)) << error;
    ASSERT_TRUE(ktag_demo_ub_inspect_frame(
        feature.data(), feature.size(), &info, &error)) << error;
    EXPECT_EQ(0U, info.flags);
    feature.back() ^= 0x1U;
    EXPECT_FALSE(ktag_demo_ub_inspect_frame(
        feature.data(), feature.size(), &info, &error));
    EXPECT_NE(std::string::npos, error.find("CRC"));
    EXPECT_FALSE(ktag_demo_ub_encode_feature_frame(
        feature.data(), KTAG_DEMO_UB_FRAME_HEADER_SIZE, contract, record,
        false, &encoded_size, &error));
}

TEST(KTAGDemoPredictionWriter, WritesDualStackMetadataAndConfidenceContract)
{
    std::vector<ktag_demo_pipeline_prediction> predictions{
        make_prediction(false, 5, 0.0F, 7U),
        make_prediction(
            true,
            9,
            KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE,
            8U),
    };
    const std::string prefix =
        "/tmp/ktag_prediction_writer_" + std::to_string(getpid());
    const std::string csv_path = prefix + ".csv";
    const std::string json_path = prefix + ".json";
    std::string error;
    ASSERT_TRUE(ktag_demo_write_pipeline_predictions(
        csv_path.c_str(),
        ktag_demo_pipeline_output_format::csv,
        predictions,
        &error)) << error;
    ASSERT_TRUE(ktag_demo_write_pipeline_predictions(
        json_path.c_str(),
        ktag_demo_pipeline_output_format::json,
        predictions,
        &error)) << error;

    const std::string csv = read_text_file(csv_path);
    const std::string json = read_text_file(json_path);
    EXPECT_EQ(
        0U,
        csv.find(
            "source_index,flow_id,ip_version,src_ip,dst_ip,src_port,"
            "dst_port,protocol,flow_key,first_packet_sequence,"
            "last_packet_sequence,label,confidence\n"));
    EXPECT_NE(std::string::npos, csv.find("\"198.51.100.10\""));
    EXPECT_NE(std::string::npos, csv.find("\"2001:db8::2\""));
    EXPECT_NE(
        std::string::npos,
        csv.find(
            "\"[2001:db8::1]:443-[2001:db8::2]:50000/17\""));
    EXPECT_NE(std::string::npos, csv.find(",5,0\n"));
    EXPECT_NE(std::string::npos, csv.find(",9,-1\n"));

    EXPECT_NE(std::string::npos, json.find("\"ip_version\":4"));
    EXPECT_NE(std::string::npos, json.find("\"ip_version\":6"));
    EXPECT_EQ(std::string::npos, json.find("frontend_id_hash"));
    EXPECT_NE(std::string::npos, json.find("\"label\":5,\"confidence\":0"));
    EXPECT_NE(std::string::npos, json.find("\"label\":9,\"confidence\":-1"));
    EXPECT_EQ(std::string::npos, json.find("unknown"));
    EXPECT_EQ(std::string::npos, json.find("label_name"));

    auto invalid = predictions;
    invalid[0U].record.confidence = -0.5F;
    EXPECT_FALSE(ktag_demo_write_pipeline_predictions(
        (prefix + "_invalid.csv").c_str(),
        ktag_demo_pipeline_output_format::csv,
        invalid,
        &error));
    EXPECT_NE(std::string::npos, error.find("confidence"));

    std::remove(csv_path.c_str());
    std::remove(json_path.c_str());
    std::remove((prefix + "_invalid.csv").c_str());
}

TEST(KTAGDemoSchemaFeaturePipeline, FlushesSameApplicationCsvInAllModes)
{
    struct mode_case {
        ktag_demo_pipeline_mode mode;
        uint32_t feature_threads;
        uint32_t output_threads;
    };
    const std::array<mode_case, 4U> modes{{
        {ktag_demo_pipeline_mode::single_thread, 1U, 1U},
        {ktag_demo_pipeline_mode::staged, 1U, 1U},
        {ktag_demo_pipeline_mode::feature_parallel, 3U, 1U},
        {ktag_demo_pipeline_mode::full_parallel, 3U, 3U},
    }};
    const std::string pcap_path = write_test_pcap();
    const char *pcap_files[] = {pcap_path.c_str()};
    std::string expected_csv;

    for (const auto &test_case : modes) {
        ktag_demo_pipeline_config config;
        config.enabled = true;
        config.feature_only = true;
        config.mode = test_case.mode;
        config.feature_threads = test_case.feature_threads;
        config.inference_threads = test_case.output_threads;
        config.lifecycle_mode = ktag_demo_feature_lifecycle_mode::modern;
        config.plugin_paths = {TRAFFIC_PLUGIN};

        const std::string output_path =
            "/tmp/ktag_schema_pipeline_" + std::to_string(getpid()) + "_" +
            ktag_demo_pipeline_mode_name(test_case.mode) + ".csv";
        ASSERT_EQ(
            0,
            ktag_demo_run_pipeline(
                config,
                "functest",
                pcap_files,
                1,
                nullptr,
                output_path.c_str(),
                ktag_demo_pipeline_output_format::csv))
            << ktag_demo_pipeline_mode_name(test_case.mode);

        const std::string csv = read_text_file(output_path);
        ASSERT_FALSE(csv.empty());
        const size_t first_newline = csv.find('\n');
        ASSERT_NE(std::string::npos, first_newline);
        const std::string header = csv.substr(0U, first_newline);
        EXPECT_EQ(
            366U,
            static_cast<uint32_t>(
                std::count(header.begin(), header.end(), ',')) + 1U);
        EXPECT_EQ(2, std::count(csv.begin(), csv.end(), '\n'));
        if (expected_csv.empty()) {
            expected_csv = csv;
        } else {
            EXPECT_EQ(expected_csv, csv)
                << ktag_demo_pipeline_mode_name(test_case.mode);
        }
        std::remove(output_path.c_str());
    }
    std::remove(pcap_path.c_str());
}

TEST(KTAGDemoSchemaFeaturePipeline, FlushesTruncatedPcapInAllModes)
{
    struct mode_case {
        ktag_demo_pipeline_mode mode;
        uint32_t feature_threads;
        uint32_t output_threads;
    };
    const std::array<mode_case, 4U> modes{{
        {ktag_demo_pipeline_mode::single_thread, 1U, 1U},
        {ktag_demo_pipeline_mode::staged, 1U, 1U},
        {ktag_demo_pipeline_mode::feature_parallel, 3U, 1U},
        {ktag_demo_pipeline_mode::full_parallel, 3U, 3U},
    }};
    const std::string pcap_path = write_truncated_test_pcap();
    const char *pcap_files[] = {pcap_path.c_str()};

    for (const auto &test_case : modes) {
        ktag_demo_pipeline_config config;
        config.enabled = true;
        config.feature_only = true;
        config.mode = test_case.mode;
        config.feature_threads = test_case.feature_threads;
        config.inference_threads = test_case.output_threads;
        config.lifecycle_mode = ktag_demo_feature_lifecycle_mode::modern;
        config.plugin_paths = {TRAFFIC_PLUGIN};
        const std::string output_path =
            "/tmp/ktag_schema_truncated_" + std::to_string(getpid()) + "_" +
            ktag_demo_pipeline_mode_name(test_case.mode) + ".csv";

        testing::internal::CaptureStderr();
        const int result = ktag_demo_run_pipeline(
            config,
            "functest",
            pcap_files,
            1,
            nullptr,
            output_path.c_str(),
            ktag_demo_pipeline_output_format::csv);
        const std::string warning = testing::internal::GetCapturedStderr();

        ASSERT_EQ(0, result) << ktag_demo_pipeline_mode_name(test_case.mode);
        EXPECT_NE(std::string::npos, warning.find("WARNING:"));
        EXPECT_EQ(warning.find("WARNING:"), warning.rfind("WARNING:"));
        EXPECT_NE(std::string::npos, warning.find("truncated"));
        const std::string csv = read_text_file(output_path);
        EXPECT_EQ(2, std::count(csv.begin(), csv.end(), '\n'));
        std::remove(output_path.c_str());
    }
    std::remove(pcap_path.c_str());
}

TEST(KTAGDemoDualStackPipeline, SkipsUnsupportedIpv6AndContinues)
{
    const std::string pcap_path = write_test_pcap_frames(
        {
            make_ipv6_non_initial_fragment(),
            make_ipv6_frame(50U, std::vector<uint8_t>(8U, 0U)),
            make_ipv6_frame(253U, std::vector<uint8_t>(8U, 0U)),
            make_ipv6_udp_frame(),
        },
        "unsupported_then_valid_ipv6");
    const char *pcap_files[] = {pcap_path.c_str()};
    struct mode_case {
        ktag_demo_pipeline_mode mode;
        uint32_t feature_threads;
        uint32_t output_threads;
    };
    const std::array<mode_case, 2U> modes{{
        {ktag_demo_pipeline_mode::single_thread, 1U, 1U},
        {ktag_demo_pipeline_mode::full_parallel, 2U, 2U},
    }};

    for (const auto &test_case : modes) {
        ktag_demo_pipeline_config config;
        config.enabled = true;
        config.feature_only = true;
        config.mode = test_case.mode;
        config.feature_threads = test_case.feature_threads;
        config.inference_threads = test_case.output_threads;
        config.lifecycle_mode = ktag_demo_feature_lifecycle_mode::modern;
        config.plugin_paths = {TRAFFIC_PLUGIN};
        const std::string output_path =
            "/tmp/ktag_dual_stack_skip_" + std::to_string(getpid()) + "_" +
            ktag_demo_pipeline_mode_name(test_case.mode) + ".csv";

        testing::internal::CaptureStdout();
        const int result = ktag_demo_run_pipeline(
            config,
            "functest",
            pcap_files,
            1,
            nullptr,
            output_path.c_str(),
            ktag_demo_pipeline_output_format::csv);
        const std::string console = testing::internal::GetCapturedStdout();
        ASSERT_EQ(0, result) << ktag_demo_pipeline_mode_name(test_case.mode);

        const std::string csv = read_text_file(output_path);
        EXPECT_EQ(2, std::count(csv.begin(), csv.end(), '\n'));
        EXPECT_NE(std::string::npos, csv.find(",6,\"2001:db8::10\","));
        const std::string marker = "不支持数据包:";
        const size_t marker_position = console.find(marker);
        ASSERT_NE(std::string::npos, marker_position);
        const size_t value_position = console.find_first_not_of(
            " \t", marker_position + marker.size());
        ASSERT_NE(std::string::npos, value_position);
        EXPECT_EQ('3', console[value_position]);
        std::remove(output_path.c_str());
    }
    std::remove(pcap_path.c_str());
}

TEST(KTAGDemoSchemaFeaturePipeline, RejectsMissingPluginAndSchemaFeature)
{
    ktag_demo_pipeline_config config;
    config.enabled = true;
    config.feature_only = true;
    config.mode = ktag_demo_pipeline_mode::single_thread;
    config.feature_threads = 1U;
    config.inference_threads = 1U;
    config.plugin_paths = {"/tmp/ktag_plugin_does_not_exist.so"};
    EXPECT_NE(
        0,
        ktag_demo_run_pipeline(
            config,
            "functest",
            nullptr,
            0,
            nullptr,
            "/tmp/ktag_unused.csv",
            ktag_demo_pipeline_output_format::csv));

    config.plugin_paths.clear();
    config.feature_order = {"missing.feature"};
    EXPECT_NE(
        0,
        ktag_demo_run_pipeline(
            config,
            "functest",
            nullptr,
            0,
            nullptr,
            "/tmp/ktag_unused.csv",
            ktag_demo_pipeline_output_format::csv));
}

TEST(KTAGDemoSchemaFeaturePipeline, RejectsReorderedProxyV1Contract)
{
    ktag_demo_pipeline_config config;
    config.enabled = true;
    config.feature_only = true;
    config.mode = ktag_demo_pipeline_mode::single_thread;
    config.feature_threads = 1U;
    config.inference_threads = 1U;
    config.schema_name = "ktag_proxy_v1";
    config.schema_version = 1U;
    config.plugin_paths = {TRAFFIC_PLUGIN, PROXY_PLUGIN};
    config.remove_features = {
        "legacy.flow_id",
        "network.src_ip",
        "network.dst_ip",
    };
    for (const auto *provider :
         ktag_proxy_features::KTAG_PROXY_PROVIDER_CATALOG) {
        for (size_t index = 0U; index < provider->feature_count; ++index) {
            config.add_features.emplace_back(
                provider->features[index].feature_id);
        }
    }
    ASSERT_EQ(68U, config.add_features.size());
    std::swap(config.add_features[66U], config.add_features[67U]);
    EXPECT_NE(
        0,
        ktag_demo_run_pipeline(
            config,
            "functest",
            nullptr,
            0,
            nullptr,
            "/tmp/ktag_unused.csv",
            ktag_demo_pipeline_output_format::csv));
}

TEST(KTAGDemoSchemaFeaturePipeline, Writes619ValueProxyCsv)
{
    ktag_demo_pipeline_config_overrides overrides;
    overrides.force_enable = true;
    overrides.feature_only = true;
    overrides.mode_set = true;
    overrides.mode = ktag_demo_pipeline_mode::single_thread;
    ktag_demo_pipeline_config config;
    std::string error;
    ASSERT_EQ(
        ktag_demo_pipeline_config_result::enabled,
        ktag_demo_load_pipeline_config_with_overrides(
            OFFICIAL_PROXY_CONFIG, &overrides, &config, &error))
        << error;
    EXPECT_TRUE(config.feature_only);
    EXPECT_EQ("ktag_proxy_v1", config.schema_name);
    EXPECT_EQ(1U, config.schema_version);
    EXPECT_EQ(2U, config.plugin_paths.size());
    EXPECT_EQ(3U, config.remove_features.size());
    EXPECT_EQ(68U, config.add_features.size());

    const std::string pcap_path = write_test_pcap();
    const std::string output_path =
        "/tmp/ktag_proxy_schema_" + std::to_string(getpid()) + ".csv";
    const char *pcap_files[] = {pcap_path.c_str()};
    ASSERT_EQ(
        0,
        ktag_demo_run_pipeline(
            config,
            OFFICIAL_PROXY_CONFIG,
            pcap_files,
            1,
            nullptr,
            output_path.c_str(),
            ktag_demo_pipeline_output_format::csv));

    const std::string csv = read_text_file(output_path);
    ASSERT_FALSE(csv.empty());
    const size_t first_newline = csv.find('\n');
    ASSERT_NE(std::string::npos, first_newline);
    const std::string header = csv.substr(0U, first_newline);
    EXPECT_EQ(
        631U,
        static_cast<uint32_t>(
            std::count(header.begin(), header.end(), ',')) + 1U);
    EXPECT_EQ(2, std::count(csv.begin(), csv.end(), '\n'));

    std::remove(output_path.c_str());
    std::remove(pcap_path.c_str());
}

TEST(KTAGDemoSchemaFeaturePipeline, WritesFrozenProxyAblationCsvs)
{
    struct ablation_case {
        const char *schema_name;
        std::vector<std::string> remove_features;
        uint32_t csv_columns;
    };
    const std::array<ablation_case, 2U> cases{{
        {
            "proxy_endpoint_ablation_v1",
            {"legacy.flow_id"},
            633U,
        },
        {
            "proxy_no_endpoint_ablation_v1",
            {
                "legacy.flow_id",
                "network.src_ip",
                "network.dst_ip",
                "network.src_port",
                "network.dst_port",
            },
            629U,
        },
    }};
    const std::string pcap_path = write_test_pcap();
    const char *pcap_files[] = {pcap_path.c_str()};

    for (const auto &test_case : cases) {
        ktag_demo_pipeline_config config;
        config.enabled = true;
        config.feature_only = true;
        config.mode = ktag_demo_pipeline_mode::single_thread;
        config.feature_threads = 1U;
        config.inference_threads = 1U;
        config.schema_name = test_case.schema_name;
        config.schema_version = 1U;
        config.plugin_paths = {TRAFFIC_PLUGIN, PROXY_PLUGIN};
        config.remove_features = test_case.remove_features;
        add_proxy_feature_ids(&config);
        ASSERT_EQ(68U, config.add_features.size());

        const std::string output_path =
            "/tmp/ktag_proxy_ablation_" + std::string(test_case.schema_name) +
            "_" + std::to_string(getpid()) + ".csv";
        ASSERT_EQ(
            0,
            ktag_demo_run_pipeline(
                config,
                "functest",
                pcap_files,
                1,
                nullptr,
                output_path.c_str(),
                ktag_demo_pipeline_output_format::csv));

        const std::string csv = read_text_file(output_path);
        const size_t first_newline = csv.find('\n');
        ASSERT_NE(std::string::npos, first_newline);
        const std::string header = csv.substr(0U, first_newline);
        EXPECT_EQ(
            test_case.csv_columns,
            static_cast<uint32_t>(
                std::count(header.begin(), header.end(), ',')) + 1U);
        if (std::string(test_case.schema_name) ==
            "proxy_endpoint_ablation_v1") {
            EXPECT_NE(std::string::npos, header.find("network.src_ip"));
            EXPECT_NE(std::string::npos, header.find("network.dst_port"));
        } else {
            EXPECT_EQ(std::string::npos, header.find("network.src_ip"));
            EXPECT_EQ(std::string::npos, header.find("network.dst_port"));
        }
        std::remove(output_path.c_str());
    }
    std::remove(pcap_path.c_str());
}

TEST(KTAGProxyFeaturePlugin, ExposesFrozenCatalogAndAbiDescriptor)
{
    EXPECT_EQ(
        3U,
        ktag_proxy_features::KTAG_PROXY_PROVIDER_CATALOG.size());
    EXPECT_EQ(
        68U,
        ktag_proxy_features::KTAG_PROXY_FEATURE_DESCRIPTOR_COUNT);
    EXPECT_EQ(
        268U,
        ktag_proxy_features::KTAG_PROXY_FEATURE_VALUE_COUNT);

    std::unordered_set<std::string> feature_ids;
    for (const auto *provider :
         ktag_proxy_features::KTAG_PROXY_PROVIDER_CATALOG) {
        ASSERT_NE(nullptr, provider);
        EXPECT_EQ(1U, provider->provider_version);
        for (size_t index = 0U;
             index < provider->feature_count;
             ++index) {
            const auto &feature = provider->features[index];
            ASSERT_NE(nullptr, feature.feature_id);
            EXPECT_GT(feature.dimension, 0U);
            EXPECT_TRUE(feature_ids.emplace(feature.feature_id).second)
                << feature.feature_id;
        }
    }
    EXPECT_EQ(68U, feature_ids.size());

    ktag_feature_plugin_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    EXPECT_EQ(
        KTAG_ERROR_NOT_SUPPORTED,
        ktag_feature_plugin_query(
            KTAG_FEATURE_PLUGIN_ABI_VERSION + 1U, &descriptor));

    descriptor = query_proxy_plugin();
    EXPECT_STREQ("ktag.proxy.features", descriptor.plugin_id);
    EXPECT_STREQ("1.0.0", descriptor.plugin_version);
    EXPECT_EQ(
        KTAG_FEATURE_PLUGIN_ABI_VERSION,
        descriptor.plugin_abi_version);
    EXPECT_EQ(0U, descriptor.reserved);
    ASSERT_NE(nullptr, descriptor.register_providers);

    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(
        KTAG_SUCCESS,
        descriptor.register_providers(nullptr, registry));
    EXPECT_EQ(3U, ktag_feature_registry_get_provider_count(registry));

    /* Leave the middle Provider registered to force rollback after the first
     * Provider of the next transaction has been added successfully. */
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_registry_unregister(
            registry,
            ktag_proxy_features::KTAG_PROXY_SEQUENCE_PROVIDER_ID));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_registry_unregister(
            registry,
            ktag_proxy_features::KTAG_PROXY_QUIC_PROVIDER_ID));
    ASSERT_EQ(
        KTAG_ERROR_ALREADY_EXISTS,
        descriptor.register_providers(nullptr, registry));
    EXPECT_EQ(1U, ktag_feature_registry_get_provider_count(registry));

    ktag_feature_schema *rollback_schema =
        ktag_feature_schema_create("proxy_rollback_check", 1U);
    ASSERT_NE(nullptr, rollback_schema);
    EXPECT_EQ(
        KTAG_ERROR_NOT_FOUND,
        ktag_feature_schema_add_registered(
            rollback_schema,
            registry,
            ktag_proxy_features::KTAG_PROXY_SEQUENCE_FEATURES[0U]
                .feature_id));
    EXPECT_EQ(
        KTAG_SUCCESS,
        ktag_feature_schema_add_registered(
            rollback_schema,
            registry,
            ktag_proxy_features::KTAG_PROXY_TCP_SIGNATURE_FEATURES[0U]
                .feature_id));
    ktag_feature_schema_destroy(rollback_schema);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGProxyFeaturePlugin, LoadsOnceAndUnloadsAfterRegistry)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ktag_demo_feature_plugin_loader loader;
    std::string error;
    ASSERT_TRUE(loader.load(PROXY_PLUGIN, registry, &error)) << error;
    EXPECT_EQ(1U, loader.size());
    EXPECT_EQ("ktag.proxy.features", loader.plugin_id(0U));
    EXPECT_EQ(3U, ktag_feature_registry_get_provider_count(registry));
    EXPECT_FALSE(loader.load(PROXY_PLUGIN, registry, &error));

    ktag_feature_registry_destroy(registry);
    loader.unload_all();
    EXPECT_EQ(0U, loader.size());
}

TEST(KTAGTrafficFeaturePlugin, LoadsAndRegistersElevenProviders)
{
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ktag_demo_feature_plugin_loader loader;
    std::string error;
    ASSERT_TRUE(loader.load(TRAFFIC_PLUGIN, registry, &error)) << error;
    EXPECT_EQ(1U, loader.size());
    EXPECT_EQ("ktag.traffic.features", loader.plugin_id(0U));
    EXPECT_EQ(11U, ktag_feature_registry_get_provider_count(registry));

    ktag_feature_registry_destroy(registry);
    loader.unload_all();
    EXPECT_EQ(0U, loader.size());
}

TEST(KTAGProxyFeaturePlugin, BuildsLegacyProxyAndAvailableSchemas)
{
    const auto descriptor = query_proxy_plugin();
    ASSERT_NE(nullptr, descriptor.register_providers);
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_traffic_features_register(registry));
    ASSERT_EQ(
        KTAG_SUCCESS,
        descriptor.register_providers(nullptr, registry));
    // The traffic plugin contributes eleven independently replaceable slots;
    // the proxy plugin still contributes its original three Providers.
    EXPECT_EQ(14U, ktag_feature_registry_get_provider_count(registry));

    ktag_feature_schema *legacy =
        ktag_feature_schema_create_legacy_v1();
    ASSERT_NE(nullptr, legacy);
    EXPECT_EQ(
        53U,
        ktag_feature_schema_get_feature_count(legacy));
    EXPECT_EQ(
        354U,
        ktag_feature_schema_get_value_count(legacy));
    EXPECT_EQ(
        0xdd48882b53bebfa5ULL,
        ktag_feature_schema_get_hash(legacy));

    ktag_feature_schema *proxy =
        ktag_feature_schema_create("ktag_proxy_v1", 1U);
    ASSERT_NE(nullptr, proxy);
    const std::unordered_set<std::string> excluded{
        "legacy.flow_id",
        "network.src_ip",
        "network.dst_ip",
    };
    for (uint32_t index = 0U;
         index < ktag_feature_schema_get_feature_count(legacy);
         ++index) {
        ktag_feature_descriptor feature{};
        uint32_t offset = 0U;
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_schema_get_descriptor(
                legacy, index, &feature, &offset));
        if (excluded.find(feature.feature_id) == excluded.end()) {
            ASSERT_EQ(
                KTAG_SUCCESS,
                ktag_feature_schema_add_registered(
                    proxy, registry, feature.feature_id));
        }
    }
    ASSERT_TRUE(add_proxy_catalog(proxy, registry));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(proxy));
    EXPECT_EQ(
        118U,
        ktag_feature_schema_get_feature_count(proxy));
    EXPECT_EQ(
        619U,
        ktag_feature_schema_get_value_count(proxy));
    EXPECT_EQ(
        0xca1eae5086b41ee5ULL,
        ktag_feature_schema_get_hash(proxy));
    for (const auto &feature_id : excluded) {
        ktag_feature_descriptor feature{};
        uint32_t offset = 0U;
        EXPECT_EQ(
            KTAG_ERROR_NOT_FOUND,
            ktag_feature_schema_find(
                proxy, feature_id.c_str(), &feature, &offset));
    }
    const char *required[] = {
        "network.src_port",
        "network.dst_port",
        "network.protocol",
        "traffic.send.byte_count",
        "traffic.receive.byte_count",
        "rawbow.crc_histogram",
    };
    for (const char *feature_id : required) {
        ktag_feature_descriptor feature{};
        uint32_t offset = 0U;
        EXPECT_EQ(
            KTAG_SUCCESS,
            ktag_feature_schema_find(
                proxy, feature_id, &feature, &offset));
    }
    uint32_t proxy_schema_index = 50U;
    uint32_t expected_offset = 351U;
    for (const auto *provider :
         ktag_proxy_features::KTAG_PROXY_PROVIDER_CATALOG) {
        for (size_t index = 0U; index < provider->feature_count; ++index) {
            ktag_feature_descriptor feature{};
            uint32_t offset = 0U;
            ASSERT_EQ(
                KTAG_SUCCESS,
                ktag_feature_schema_get_descriptor(
                    proxy, proxy_schema_index, &feature, &offset));
            EXPECT_STREQ(
                provider->features[index].feature_id,
                feature.feature_id);
            EXPECT_EQ(provider->features[index].dimension, feature.dimension);
            EXPECT_EQ(expected_offset, offset);
            expected_offset += feature.dimension;
            ++proxy_schema_index;
        }
    }
    EXPECT_EQ(118U, proxy_schema_index);
    EXPECT_EQ(619U, expected_offset);

    ktag_feature_schema *available =
        ktag_feature_schema_create("ktag_proxy_available_v1", 1U);
    ASSERT_NE(nullptr, available);
    for (uint32_t index = 0U;
         index < ktag_feature_schema_get_feature_count(legacy);
         ++index) {
        ktag_feature_descriptor feature{};
        uint32_t offset = 0U;
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_schema_get_descriptor(
                legacy, index, &feature, &offset));
        ASSERT_EQ(
            KTAG_SUCCESS,
            ktag_feature_schema_add_registered(
                available, registry, feature.feature_id));
    }
    ASSERT_TRUE(add_proxy_catalog(available, registry));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(available));
    EXPECT_EQ(
        121U,
        ktag_feature_schema_get_feature_count(available));
    EXPECT_EQ(
        622U,
        ktag_feature_schema_get_value_count(available));

    ktag_feature_schema_destroy(available);
    ktag_feature_schema_destroy(proxy);
    ktag_feature_schema_destroy(legacy);
    ktag_feature_registry_destroy(registry);
}

TEST(KTAGProxyFeaturePlugin, ProvidersWriteCompleteFeatureVector)
{
    const auto descriptor = query_proxy_plugin();
    ASSERT_NE(nullptr, descriptor.register_providers);
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ASSERT_NE(nullptr, registry);
    ASSERT_EQ(
        KTAG_SUCCESS,
        descriptor.register_providers(nullptr, registry));

    ktag_feature_schema *schema =
        ktag_feature_schema_create("ktag_proxy_features_only_v1", 1U);
    ASSERT_NE(nullptr, schema);
    ASSERT_TRUE(add_proxy_catalog(schema, registry));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_schema_freeze(schema));
    ASSERT_EQ(
        268U,
        ktag_feature_schema_get_value_count(schema));

    ktag_feature_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.schema = schema;
    config.registry = registry;
    config.flow_capacity = 8U;
    config.udp_packet_limit = 100U;
    config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
    ktag_feature_context *context = ktag_feature_create(&config);
    ktag_feature_batch *batch =
        ktag_feature_batch_create(schema, 1U);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, batch);

    const auto frame = make_proxy_test_udp_frame();
    ktag_packet packet{};
    ktag_packet_init(&packet);
    packet.data = frame.data();
    packet.captured_length = static_cast<uint32_t>(frame.size());
    packet.original_length = static_cast<uint32_t>(frame.size());
    packet.link_type = KTAG_CAPTURE_LINKTYPE_ETHERNET;
    packet.sequence = 1U;
    packet.timestamp.seconds = 1U;
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_process_packet(context, &packet, batch));
    EXPECT_EQ(0U, ktag_feature_batch_get_count(batch));
    ASSERT_EQ(KTAG_SUCCESS, ktag_feature_flush(context, batch));
    ASSERT_EQ(1U, ktag_feature_batch_get_count(batch));

    ktag_feature_record record{};
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_feature_batch_get_record(batch, 0U, &record));
    ASSERT_EQ(268U, record.value_count);
    for (uint32_t index = 0U; index < record.value_count; ++index) {
        EXPECT_TRUE(std::isfinite(record.values[index])) << index;
    }
    EXPECT_FLOAT_EQ(1.0F, record.values[0U]);
    EXPECT_FLOAT_EQ(1.0F, record.values[96U]);
    EXPECT_FLOAT_EQ(1.0F, record.values[97U]);
    EXPECT_FLOAT_EQ(1.0F, record.values[113U]);
    EXPECT_FLOAT_EQ(1.0F, record.values[147U]);
    for (uint32_t index = 148U; index < 200U; ++index) {
        EXPECT_FLOAT_EQ(0.0F, record.values[index]) << index;
    }
    std::array<float, 68U> expected_quic{};
    expected_quic[0U] = 1.0F;   /* packet_candidate_count */
    expected_quic[2U] = 1.0F;   /* parse_failure_count */
    expected_quic[3U] = 1.0F;   /* fixed_bit_count */
    expected_quic[5U] = 1.0F;   /* short_header_count */
    expected_quic[18U] = 1.0F;  /* signed_datagram_length[0] */
    expected_quic[34U] = 6.0F;  /* header_type_sequence[0] */
    expected_quic[50U] = 1.0F;  /* sequence_valid_count */
    expected_quic[55U] = 1.0F;  /* high_nibble_histogram[4] */
    for (size_t index = 0U; index < expected_quic.size(); ++index) {
        EXPECT_FLOAT_EQ(
            expected_quic[index], record.values[200U + index])
            << index;
    }

    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
}

} // namespace
