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
#include "pipeline_config.h"
#include "pipeline_runner.h"
#include "proxy_feature_catalog.h"
#include "schema_feature_writer.h"

namespace {

namespace fs = std::filesystem;

const char *const EXAMPLE_PLUGIN =
    "../../../output/plugins/libktag_example_feature_plugin.so";
const char *const PROXY_PLUGIN =
    "../../../output/plugins/libktag_proxy_features.so";
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
        ktag_feature_registry_register_legacy_builtins(registry) !=
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

} // namespace
