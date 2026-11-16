#include <gtest/gtest.h>
#include <filesystem>
#include <cstdlib>
#include "pipeline_config.h"

namespace {
std::filesystem::path repository_root() {
    if (const char *root = std::getenv("KTAG_TEST_CONFIG_ROOT")) return root;
    return std::filesystem::absolute(__FILE__).parent_path().parent_path().parent_path().parent_path();
}
}

TEST(KTAGIntegrationConfig, UbExampleCombinesTrafficPluginWithPocScheduling) {
    const auto path = repository_root() / "tools/demo/config-ub-1to1.yaml";
    for (bool frontend : {true, false}) {
        ktag_demo_pipeline_config_overrides overrides;
        overrides.force_enable = true;
        overrides.distributed_role_set = true;
        overrides.distributed_role = frontend ? ktag_demo_distributed_role::frontend
                                             : ktag_demo_distributed_role::backend;
        overrides.node_id = frontend ? "computer01" : "controller";
        ktag_demo_pipeline_config config;
        std::string error;
        ASSERT_EQ(ktag_demo_pipeline_config_result::enabled,
                  ktag_demo_load_pipeline_config_with_overrides(path.string().c_str(), &overrides,
                                                                &config, &error)) << error;
        ASSERT_EQ(1U, config.plugin_paths.size());
        EXPECT_EQ("libktag_traffic_features.so", std::filesystem::path(config.plugin_paths[0]).filename());
        EXPECT_EQ(frontend, config.feature_only);
        EXPECT_EQ(16U, config.feature_threads);
        EXPECT_EQ(16U, config.inference_threads);
        EXPECT_EQ(16U, config.ub.queues_per_provider);
        EXPECT_EQ(16U, config.ub.sender_threads);
        EXPECT_EQ(64U, config.ub.credits_per_queue);
        EXPECT_EQ(16U, config.ub.backend_worker_cpus.size());
        EXPECT_EQ(4U, config.dpdk_rx_threads);
    }
}

TEST(KTAGIntegrationConfig, ApplicationAndProxyExamplesLoadRequiredPlugins) {
    for (const char *name : {"config.yaml", "proxy_config.yaml"}) {
        const auto path = repository_root() / "tools/demo" / name;
        ktag_demo_pipeline_config_overrides overrides;
        overrides.force_enable = true;
        ktag_demo_pipeline_config config;
        std::string error;
        ASSERT_EQ(ktag_demo_pipeline_config_result::enabled,
                  ktag_demo_load_pipeline_config_with_overrides(path.string().c_str(), &overrides,
                                                                &config, &error)) << error;
        ASSERT_FALSE(config.plugin_paths.empty());
        EXPECT_EQ("libktag_traffic_features.so", std::filesystem::path(config.plugin_paths[0]).filename());
        EXPECT_EQ(std::string(name) == "config.yaml" ? 1U : 2U, config.plugin_paths.size());
    }
}
