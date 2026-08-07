/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "pipeline_config.h"

#include <array>
#include <filesystem>
#include <cstring>
#include <limits>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

namespace {

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool has_value(const YAML::Node &node)
{
    return node.IsDefined() && !node.IsNull();
}

bool read_u32(
    const YAML::Node &parent,
    const char *name,
    uint32_t *value,
    std::string *error)
{
    if (!has_value(parent)) {
        return true;
    }
    const YAML::Node node = parent[name];
    if (!has_value(node)) {
        return true;
    }
    try {
        const uint64_t parsed = node.as<uint64_t>();
        if (parsed > std::numeric_limits<uint32_t>::max()) {
            set_error(error, std::string(name) + " exceeds uint32 range");
            return false;
        }
        *value = static_cast<uint32_t>(parsed);
        return true;
    } catch (const YAML::Exception &exception) {
        set_error(error, std::string(name) + ": " + exception.what());
        return false;
    }
}

bool read_u64(
    const YAML::Node &parent,
    const char *name,
    uint64_t *value,
    std::string *error)
{
    if (!has_value(parent)) {
        return true;
    }
    const YAML::Node node = parent[name];
    if (!has_value(node)) {
        return true;
    }
    try {
        *value = node.as<uint64_t>();
        return true;
    } catch (const YAML::Exception &exception) {
        set_error(error, std::string(name) + ": " + exception.what());
        return false;
    }
}

bool read_string_list(
    const YAML::Node &node,
    const char *name,
    std::vector<std::string> *values,
    std::string *error)
{
    if (!has_value(node)) {
        return true;
    }
    if (!node.IsSequence()) {
        set_error(error, std::string(name) + " must be an array");
        return false;
    }
    std::unordered_set<std::string> unique;
    try {
        for (const auto &item : node) {
            const std::string value = item.as<std::string>();
            if (value.empty() || !unique.insert(value).second) {
                set_error(
                    error,
                    std::string(name) +
                        " must contain unique non-empty strings");
                return false;
            }
            values->push_back(value);
        }
        return true;
    } catch (const YAML::Exception &exception) {
        set_error(error, std::string(name) + ": " + exception.what());
        return false;
    }
}

bool resolve_path(
    const fs::path &config_directory,
    const std::string &configured,
    std::string *resolved,
    std::string *error)
{
    if (configured.empty()) {
        set_error(error, "configured path must not be empty");
        return false;
    }
    try {
        fs::path path(configured);
        if (!path.is_absolute()) {
            path = config_directory / path;
        }
        *resolved = fs::absolute(path).lexically_normal().string();
        return true;
    } catch (const std::exception &exception) {
        set_error(error, exception.what());
        return false;
    }
}

bool parse_mode(
    const YAML::Node &node,
    ktag_demo_pipeline_mode *mode,
    std::string *error)
{
    const std::string value = has_value(node) ?
        node.as<std::string>() : "single_thread";
    if (!ktag_demo_pipeline_mode_from_name(value.c_str(), mode)) {
        set_error(
            error,
            "pipeline.mode must be single_thread, staged, "
            "feature_parallel or full_parallel");
        return false;
    }
    return true;
}

bool parse_feature_lifecycle_mode(
    const YAML::Node &node,
    ktag_demo_feature_lifecycle_mode *mode,
    std::string *error)
{
    const std::string value =
        has_value(node) ? node.as<std::string>() : "legacy";
    if (value == "legacy" || value == "legacy_compat") {
        *mode = ktag_demo_feature_lifecycle_mode::legacy_compat;
    } else if (value == "modern") {
        *mode = ktag_demo_feature_lifecycle_mode::modern;
    } else {
        set_error(
            error,
            "pipeline.feature.lifecycle_mode must be legacy or modern");
        return false;
    }
    return true;
}

bool parse_backpressure(
    const YAML::Node &node,
    ktag_demo_backpressure_policy *policy,
    std::string *error)
{
    const std::string value =
        has_value(node) ? node.as<std::string>() : "block";
    if (value == "block") {
        *policy = ktag_demo_backpressure_policy::block;
    } else if (value == "drop_newest") {
        *policy = ktag_demo_backpressure_policy::drop_newest;
    } else if (value == "drop_oldest") {
        *policy = ktag_demo_backpressure_policy::drop_oldest;
    } else if (value == "fail") {
        *policy = ktag_demo_backpressure_policy::fail;
    } else {
        set_error(
            error,
            "pipeline.backpressure.policy must be block, drop_newest, "
            "drop_oldest or fail");
        return false;
    }
    return true;
}

bool has_exact_features(
    const std::vector<std::string> &features,
    const std::unordered_set<std::string> &required)
{
    if (features.size() != required.size()) {
        return false;
    }
    const std::unordered_set<std::string> configured(
        features.cbegin(), features.cend());
    return configured == required;
}

bool proxy_schema_exclusions(
    const std::string &schema_name,
    std::unordered_set<std::string> *excluded)
{
    if (schema_name == "proxy_endpoint_ablation_v1") {
        *excluded = {"legacy.flow_id"};
        return true;
    }
    if (schema_name == "ktag_proxy_v1") {
        *excluded = {
            "legacy.flow_id",
            "network.src_ip",
            "network.dst_ip",
        };
        return true;
    }
    if (schema_name == "proxy_no_endpoint_ablation_v1") {
        *excluded = {
            "legacy.flow_id",
            "network.src_ip",
            "network.dst_ip",
            "network.src_port",
            "network.dst_port",
        };
        return true;
    }
    return false;
}

bool validate_frozen_schema_selection(
    const ktag_demo_pipeline_config &config,
    std::string *error)
{
    if (config.schema_name == "ktag_legacy_v1") {
        if (config.schema_version != 1U ||
            !config.feature_order.empty() ||
            !config.remove_features.empty() ||
            !config.add_features.empty()) {
            set_error(
                error,
                "ktag_legacy_v1 is a frozen 53-feature/354-value contract; "
                "custom selections require a different Schema name/version");
            return false;
        }
        return true;
    }
    std::unordered_set<std::string> required_exclusions;
    if (!proxy_schema_exclusions(
            config.schema_name, &required_exclusions)) {
        return true;
    }
    if (config.schema_version != 1U) {
        set_error(
            error,
            config.schema_name +
                " requires pipeline.schema.version: 1");
        return false;
    }
    if (!config.feature_order.empty()) {
        set_error(
            error,
            config.schema_name +
                " must use remove_features/add_features so its Built-in "
                "and Proxy ordering remains auditable");
        return false;
    }
    if (config.plugin_paths.empty()) {
        set_error(
            error,
            config.schema_name +
                " requires the official proxy Feature Plugin");
        return false;
    }
    if (!has_exact_features(
            config.remove_features, required_exclusions)) {
        set_error(
            error,
            config.schema_name +
                " remove_features does not match its frozen ablation "
                "contract");
        return false;
    }
    if (config.add_features.size() != 68U) {
        set_error(
            error,
            config.schema_name +
                " add_features must contain all 68 Proxy Feature "
                "Descriptors in Catalog order");
        return false;
    }
    for (const auto &feature_id : config.add_features) {
        if (feature_id.rfind("proxy.", 0U) != 0U) {
            set_error(
                error,
                config.schema_name +
                    " add_features may contain only proxy.* Feature IDs");
            return false;
        }
    }
    return true;
}

bool parse_enabled_config(
    const YAML::Node &pipeline,
    const fs::path &config_directory,
    bool feature_only,
    ktag_demo_pipeline_config *config,
    std::string *error)
{
    if (!parse_mode(pipeline["mode"], &config->mode, error)) {
        return false;
    }
    const YAML::Node model_package_path = pipeline["model_package_path"];
    if (!feature_only && has_value(model_package_path)) {
        if (!resolve_path(
            config_directory,
            model_package_path.as<std::string>(),
            &config->model_package_path,
            error)) {
            return false;
        }
    } else if (!feature_only) {
        set_error(error, "pipeline.model_package_path is required");
        return false;
    }

    const YAML::Node capture = pipeline["capture"];
    const YAML::Node feature = pipeline["feature"];
    const YAML::Node inference = pipeline["inference"];
    const YAML::Node backpressure = pipeline["backpressure"];
    const YAML::Node schema = pipeline["schema"];
    if ((has_value(capture) && !capture.IsMap()) ||
        (has_value(feature) && !feature.IsMap()) ||
        (!feature_only && has_value(inference) && !inference.IsMap()) ||
        (has_value(backpressure) && !backpressure.IsMap()) ||
        (has_value(schema) && !schema.IsMap())) {
        set_error(error, "pipeline nested configuration must use maps");
        return false;
    }

    if (!read_u32(capture, "packet_pool_size", &config->packet_pool_size, error) ||
        !read_u32(feature, "threads", &config->feature_threads, error) ||
        !read_u32(feature, "queue_capacity", &config->packet_queue_capacity, error) ||
        !read_u64(feature, "flow_hash_seed", &config->flow_hash_seed, error) ||
        !read_u32(feature, "expire_interval_ms", &config->expire_interval_ms, error) ||
        !read_u32(feature, "flow_capacity", &config->flow_capacity, error) ||
        !read_u64(feature, "tcp_idle_timeout_us", &config->tcp_idle_timeout_us, error) ||
        !read_u64(feature, "udp_idle_timeout_us", &config->udp_idle_timeout_us, error) ||
        !read_u32(feature, "udp_packet_limit", &config->udp_packet_limit, error) ||
        !parse_feature_lifecycle_mode(
            has_value(feature) ? feature["lifecycle_mode"] : YAML::Node(),
            &config->lifecycle_mode,
            error) ||
        !parse_backpressure(
            has_value(backpressure) ?
                backpressure["policy"] : YAML::Node(),
            &config->backpressure,
            error)) {
        return false;
    }
    if (!feature_only &&
        (!read_u32(
            inference, "threads", &config->inference_threads, error) ||
         !read_u32(
            inference,
            "batch_size",
            &config->inference_batch_size,
            error) ||
         !read_u32(
            inference,
            "queue_capacity",
            &config->feature_queue_capacity,
            error) ||
         !read_u32(
            inference,
            "intra_op_threads",
            &config->inference_intra_op_threads,
            error) ||
         !read_u32(
            inference,
            "inter_op_threads",
            &config->inference_inter_op_threads,
            error))) {
        return false;
    }

    try {
        if (has_value(schema)) {
            const YAML::Node schema_name = schema["name"];
            if (has_value(schema_name)) {
                config->schema_name = schema_name.as<std::string>();
            }
        }
    } catch (const YAML::Exception &exception) {
        set_error(error, exception.what());
        return false;
    }
    if (!read_u32(schema, "version", &config->schema_version, error) ||
        !read_string_list(
            has_value(schema) ? schema["feature_order"] : YAML::Node(),
            "pipeline.schema.feature_order",
            &config->feature_order,
            error) ||
        !read_string_list(
            has_value(schema) ? schema["remove_features"] : YAML::Node(),
            "pipeline.schema.remove_features",
            &config->remove_features,
            error) ||
        !read_string_list(
            has_value(schema) ? schema["add_features"] : YAML::Node(),
            "pipeline.schema.add_features",
            &config->add_features,
            error)) {
        return false;
    }
    if (!config->feature_order.empty() &&
        (!config->remove_features.empty() || !config->add_features.empty())) {
        set_error(
            error,
            "feature_order cannot be combined with remove_features/add_features");
        return false;
    }

    std::vector<std::string> configured_plugins;
    if (!read_string_list(
            pipeline["plugins"],
            "pipeline.plugins",
            &configured_plugins,
            error)) {
        return false;
    }
    for (const auto &plugin : configured_plugins) {
        std::string resolved;
        if (!resolve_path(config_directory, plugin, &resolved, error)) {
            return false;
        }
        config->plugin_paths.push_back(std::move(resolved));
    }

    if (config->schema_name.empty() || config->schema_version == 0U ||
        config->packet_pool_size == 0U ||
        config->packet_queue_capacity == 0U ||
        config->feature_queue_capacity == 0U ||
        config->inference_batch_size == 0U ||
        config->feature_threads == 0U || config->inference_threads == 0U) {
        set_error(error, "pipeline sizes, thread counts and schema version must be positive");
        return false;
    }

    if (!validate_frozen_schema_selection(*config, error)) {
        return false;
    }

    return true;
}

bool apply_overrides_and_normalize(
    const ktag_demo_pipeline_config_overrides *overrides,
    ktag_demo_pipeline_config *config,
    std::string *error)
{
    if (overrides != nullptr) {
        config->feature_only = overrides->feature_only;
        if (overrides->mode_set) {
            config->mode = overrides->mode;
        }
        if (overrides->feature_threads != 0U) {
            config->feature_threads = overrides->feature_threads;
        }
        if (overrides->inference_threads != 0U) {
            config->inference_threads = overrides->inference_threads;
        }

        if ((config->mode == ktag_demo_pipeline_mode::single_thread ||
             config->mode == ktag_demo_pipeline_mode::staged) &&
            ((overrides->feature_threads != 0U &&
              overrides->feature_threads != 1U) ||
             (overrides->inference_threads != 0U &&
              overrides->inference_threads != 1U))) {
            set_error(
                error,
                "single_thread/staged require exactly one Feature and "
                "one Inference worker");
            return false;
        }
        if (config->mode == ktag_demo_pipeline_mode::feature_parallel &&
            overrides->inference_threads != 0U &&
            overrides->inference_threads != 1U) {
            set_error(
                error,
                "feature_parallel requires exactly one Inference worker; "
                "use full_parallel for multiple Inference workers");
            return false;
        }
    }

    if (config->feature_only) {
        if (config->mode == ktag_demo_pipeline_mode::single_thread ||
            config->mode == ktag_demo_pipeline_mode::staged) {
            config->feature_threads = 1U;
            config->inference_threads = 1U;
        } else if (config->mode ==
                   ktag_demo_pipeline_mode::feature_parallel) {
            config->inference_threads = 1U;
        } else {
            config->inference_threads = config->feature_threads;
        }
        return true;
    }
    if (config->mode == ktag_demo_pipeline_mode::single_thread ||
        config->mode == ktag_demo_pipeline_mode::staged) {
        config->feature_threads = 1U;
        config->inference_threads = 1U;
    } else if (config->mode == ktag_demo_pipeline_mode::feature_parallel) {
        config->inference_threads = 1U;
    }
    return true;
}

} // namespace

ktag_demo_pipeline_config_result ktag_demo_load_pipeline_config(
    const char *config_path,
    ktag_demo_pipeline_config *config,
    std::string *error)
{
    return ktag_demo_load_pipeline_config_with_overrides(
        config_path, nullptr, config, error);
}

ktag_demo_pipeline_config_result
ktag_demo_load_pipeline_config_with_overrides(
    const char *config_path,
    const ktag_demo_pipeline_config_overrides *overrides,
    ktag_demo_pipeline_config *config,
    std::string *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (config_path == nullptr || config_path[0] == '\0' ||
        config == nullptr) {
        set_error(error, "pipeline config path is required");
        return ktag_demo_pipeline_config_result::error;
    }
    try {
        const fs::path absolute_config =
            fs::absolute(config_path).lexically_normal();
        const YAML::Node root = YAML::LoadFile(absolute_config.string());
        const YAML::Node pipeline = root["pipeline"];
        if (!has_value(pipeline)) {
            if (overrides != nullptr && overrides->force_enable) {
                set_error(
                    error,
                    "command line selected the new pipeline, but the YAML "
                    "has no pipeline block");
                return ktag_demo_pipeline_config_result::error;
            }
            return ktag_demo_pipeline_config_result::disabled;
        }
        if (!pipeline.IsMap()) {
            set_error(error, "pipeline must be a map");
            return ktag_demo_pipeline_config_result::error;
        }
        const YAML::Node enabled_node = pipeline["enabled"];
        const bool enabled = has_value(enabled_node) ?
            enabled_node.as<bool>() : false;
        if (!enabled &&
            (overrides == nullptr || !overrides->force_enable)) {
            return ktag_demo_pipeline_config_result::disabled;
        }

        *config = {};
        config->enabled = true;
        const bool feature_only =
            overrides != nullptr && overrides->feature_only;
        if (!parse_enabled_config(
                pipeline,
                absolute_config.parent_path(),
                feature_only,
                config,
                error)) {
            return ktag_demo_pipeline_config_result::error;
        }
        if (!apply_overrides_and_normalize(overrides, config, error)) {
            return ktag_demo_pipeline_config_result::error;
        }
        return ktag_demo_pipeline_config_result::enabled;
    } catch (const YAML::Exception &exception) {
        set_error(error, exception.what());
        return ktag_demo_pipeline_config_result::error;
    } catch (const std::exception &exception) {
        set_error(error, exception.what());
        return ktag_demo_pipeline_config_result::error;
    } catch (...) {
        set_error(error, "unknown pipeline configuration error");
        return ktag_demo_pipeline_config_result::error;
    }
}

bool ktag_demo_pipeline_mode_from_name(
    const char *name,
    ktag_demo_pipeline_mode *mode)
{
    if (name == nullptr || mode == nullptr) {
        return false;
    }
    if (std::strcmp(name, "single_thread") == 0) {
        *mode = ktag_demo_pipeline_mode::single_thread;
    } else if (std::strcmp(name, "staged") == 0) {
        *mode = ktag_demo_pipeline_mode::staged;
    } else if (std::strcmp(name, "feature_parallel") == 0) {
        *mode = ktag_demo_pipeline_mode::feature_parallel;
    } else if (std::strcmp(name, "full_parallel") == 0) {
        *mode = ktag_demo_pipeline_mode::full_parallel;
    } else {
        return false;
    }
    return true;
}

const char *ktag_demo_pipeline_mode_name(ktag_demo_pipeline_mode mode)
{
    switch (mode) {
        case ktag_demo_pipeline_mode::single_thread:
            return "single_thread";
        case ktag_demo_pipeline_mode::staged:
            return "staged";
        case ktag_demo_pipeline_mode::feature_parallel:
            return "feature_parallel";
        case ktag_demo_pipeline_mode::full_parallel:
            return "full_parallel";
    }
    return "unknown";
}

const char *ktag_demo_feature_lifecycle_mode_name(
    ktag_demo_feature_lifecycle_mode mode)
{
    switch (mode) {
        case ktag_demo_feature_lifecycle_mode::legacy_compat:
            return "legacy";
        case ktag_demo_feature_lifecycle_mode::modern:
            return "modern";
    }
    return "unknown";
}

const char *ktag_demo_backpressure_name(
    ktag_demo_backpressure_policy policy)
{
    switch (policy) {
        case ktag_demo_backpressure_policy::block:
            return "block";
        case ktag_demo_backpressure_policy::drop_newest:
            return "drop_newest";
        case ktag_demo_backpressure_policy::drop_oldest:
            return "drop_oldest";
        case ktag_demo_backpressure_policy::fail:
            return "fail";
    }
    return "unknown";
}
