#include <gtest/gtest.h>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include "pipeline_config.h"
#include "ub_node_validation.h"

namespace {
std::filesystem::path repository_root() {
    if (const char *root = std::getenv("KTAG_TEST_CONFIG_ROOT")) return root;
    // Makefile builds pass a relative __FILE__ containing ".." and duplicate
    // separators. Normalize it before walking up to the repository root.
    return std::filesystem::absolute(__FILE__).lexically_normal()
        .parent_path().parent_path().parent_path().parent_path();
}

class ub_hostname_config_file {
public:
    ub_hostname_config_file(const std::vector<std::string> &providers, const std::string &user)
        : providers_(providers), user_(user) {
        path_ = std::filesystem::temp_directory_path() /
            ("ktag-ub-hostnames-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".yaml");
        std::ofstream file(path_);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file << "pipeline:\n  enabled: false\n  mode: feature_parallel\n"
                "  model_package_path: model_package\n"
                "  inference:\n    threads: " << providers.size() << "\n"
                "    intra_op_threads: 1\n    inter_op_threads: 1\n"
                "  distributed:\n    ub_ring:\n      enabled: false\n"
                "      provider_nodes: [";
        for (size_t index = 0; index < providers.size(); ++index) {
            if (index) file << ", ";
            file << providers[index];
        }
        file << "]\n      user_nodes: [" << user << "]\n"
                "      queues_per_provider: 1\n      credits_per_queue: 64\n"
                "      backend_worker_cpus: \"0";
        if (providers.size() > 1U) file << '-' << providers.size() - 1U;
        file << "\"\n";
    }
    ~ub_hostname_config_file() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    ktag_demo_pipeline_config_result load(bool frontend, std::string *error, bool enable_ub = true) {
        ktag_demo_pipeline_config_overrides overrides;
        overrides.force_enable = true;
        overrides.distributed_role_set = enable_ub;
        overrides.distributed_role = frontend ? ktag_demo_distributed_role::frontend
                                             : ktag_demo_distributed_role::backend;
        overrides.node_id = frontend ? providers_.front() : user_;
        ktag_demo_pipeline_config config;
        return ktag_demo_load_pipeline_config_with_overrides(
            path_.string().c_str(), &overrides, &config, error);
    }

private:
    std::filesystem::path path_;
    std::vector<std::string> providers_;
    std::string user_;
};
}

TEST(KTAGIntegrationConfig, RejectsEveryOutOfOrderProviderForEitherRole) {
    struct topology {
        std::vector<std::string> providers;
        std::string user;
        std::string invalid_provider;
    };
    for (const auto &item : {
             topology{{"server-z"}, "server-a", "server-z"},
             topology{{"frontend-a", "worker-z", "frontend-b"}, "user-m", "worker-z"},
             topology{{"server-2"}, "server-10", "server-2"},
             topology{{"frontend"}, "Backend", "frontend"}}) {
        ub_hostname_config_file file(item.providers, item.user);
        for (bool frontend : {true, false}) {
            std::string error;
            EXPECT_EQ(ktag_demo_pipeline_config_result::error, file.load(frontend, &error));
            EXPECT_NE(std::string::npos, error.find("UB hostname order invalid")) << error;
            EXPECT_NE(std::string::npos, error.find("Provider/Frontend '" + item.invalid_provider + "'")) << error;
            EXPECT_NE(std::string::npos, error.find("User/Backend '" + item.user + "'")) << error;
        }
    }
}

TEST(KTAGIntegrationConfig, AcceptsValidSingleAndMultipleProviderNames) {
    struct topology {
        std::vector<std::string> providers;
        std::string user;
    };
    for (const auto &item : {
             topology{{"computer01"}, "controller"},
             topology{{"frontend-a", "frontend-b", "frontend-c"}, "user"},
             topology{{"server"}, "server-01"},
             topology{{"Frontend"}, "backend"}}) {
        ub_hostname_config_file file(item.providers, item.user);
        for (bool frontend : {true, false}) {
            std::string error;
            EXPECT_EQ(ktag_demo_pipeline_config_result::enabled, file.load(frontend, &error)) << error;
        }
    }
}

TEST(KTAGIntegrationConfig, HostnameOrderDoesNotRestrictDisabledUb) {
    ub_hostname_config_file file({"server-z"}, "server-a");
    std::string error;
    EXPECT_EQ(ktag_demo_pipeline_config_result::enabled, file.load(true, &error, false)) << error;
}

TEST(KTAGIntegrationConfig, EqualProviderAndUserRemainADuplicateNameError) {
    ub_hostname_config_file file({"same-host"}, "same-host");
    std::string error;
    EXPECT_EQ(ktag_demo_pipeline_config_result::error, file.load(true, &error));
    EXPECT_NE(std::string::npos, error.find("hostnames must be unique")) << error;
}

TEST(KTAGIntegrationConfig, SharedTransportHostnameGuardHandlesNullErrorOutput) {
    EXPECT_FALSE(ktag_demo_validate_ub_hostname_order({"server-z"}, "server-a", nullptr));
    EXPECT_TRUE(ktag_demo_validate_ub_hostname_order({"computer01"}, "controller", nullptr));
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
