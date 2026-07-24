#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "bounded_queue.h"
#include "feature_plugin_loader.h"
#include "pipeline_config.h"

namespace {

const char *const EXAMPLE_PLUGIN =
    "../../../output/plugins/libktag_example_feature_plugin.so";
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
