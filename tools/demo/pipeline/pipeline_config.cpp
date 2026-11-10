/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "pipeline_config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

#include "ub_feature_wire.h"

#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

namespace {

void set_error(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
}

bool has_value(const YAML::Node &node) {
  return node.IsDefined() && !node.IsNull();
}

bool read_u32(const YAML::Node &parent, const char *name, uint32_t *value,
              std::string *error) {
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

bool read_u64(const YAML::Node &parent, const char *name, uint64_t *value,
              std::string *error) {
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

bool read_bool(const YAML::Node &parent, const char *name, bool *value,
               std::string *error) {
  if (!has_value(parent)) {
    return true;
  }
  const YAML::Node node = parent[name];
  if (!has_value(node)) {
    return true;
  }
  try {
    *value = node.as<bool>();
    return true;
  } catch (const YAML::Exception &exception) {
    set_error(error, std::string(name) + ": " + exception.what());
    return false;
  }
}

bool read_string(const YAML::Node &parent, const char *name, std::string *value,
                 std::string *error) {
  if (!has_value(parent)) {
    return true;
  }
  const YAML::Node node = parent[name];
  if (!has_value(node)) {
    return true;
  }
  try {
    const std::string parsed = node.as<std::string>();
    if (parsed.empty()) {
      set_error(error, std::string(name) + " must not be empty");
      return false;
    }
    *value = parsed;
    return true;
  } catch (const YAML::Exception &exception) {
    set_error(error, std::string(name) + ": " + exception.what());
    return false;
  }
}

bool read_string_list(const YAML::Node &node, const char *name,
                      std::vector<std::string> *values, std::string *error) {
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
        set_error(error,
                  std::string(name) + " must contain unique non-empty strings");
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

bool read_ordered_string_list(const YAML::Node &node, const char *name,
                              std::vector<std::string> *values,
                              std::string *error) {
  if (!has_value(node)) {
    return true;
  }
  if (!node.IsSequence()) {
    set_error(error, std::string(name) + " must be an array");
    return false;
  }
  try {
    for (const auto &item : node) {
      const std::string value = item.as<std::string>();
      if (value.empty()) {
        set_error(error, std::string(name) + " must contain non-empty strings");
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

bool read_ground_truth_labels(
    const YAML::Node &node,
    std::map<std::string, std::vector<uint32_t>> *labels, std::string *error) {
  if (!has_value(node)) {
    return true;
  }
  if (!node.IsMap()) {
    set_error(error,
              "pipeline.distributed.ub_ring.ground_truth_labels must be a "
              "map");
    return false;
  }
  try {
    for (const auto &entry : node) {
      const std::string hostname = entry.first.as<std::string>();
      if (hostname.empty() || labels->find(hostname) != labels->end() ||
          !entry.second.IsSequence()) {
        set_error(error,
                  "ground_truth_labels requires unique hostnames and label "
                  "arrays");
        return false;
      }
      std::vector<uint32_t> values;
      values.reserve(entry.second.size());
      for (const auto &label : entry.second) {
        const uint64_t value = label.as<uint64_t>();
        if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
          set_error(error, "ground truth label exceeds the Demo int range");
          return false;
        }
        values.push_back(static_cast<uint32_t>(value));
      }
      labels->emplace(hostname, std::move(values));
    }
    return true;
  } catch (const YAML::Exception &exception) {
    set_error(error, std::string("ground_truth_labels: ") + exception.what());
    return false;
  } catch (...) {
    set_error(error, "out of memory parsing ground_truth_labels");
    return false;
  }
}

bool hash_config_file(const fs::path &path, uint64_t *hash,
                      std::string *error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    set_error(error, "failed to open pipeline config for ruleset hash");
    return false;
  }
  uint64_t value = 14695981039346656037ULL;
  char buffer[4096];
  while (input.good()) {
    input.read(buffer, sizeof(buffer));
    const std::streamsize count = input.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      value ^= static_cast<uint8_t>(buffer[index]);
      value *= 1099511628211ULL;
    }
  }
  if (!input.eof()) {
    set_error(error, "failed to read pipeline config for ruleset hash");
    return false;
  }
  *hash = value;
  return true;
}

bool parse_worker_cpus(const std::string &text, std::vector<uint32_t> *cpus,
                       std::string *error, const std::string &field = "backend_worker_cpus") {
  std::unordered_set<uint32_t> unique;
  size_t offset = 0U;
  auto number = [&](uint32_t *value) {
    if (offset >= text.size() || text[offset] < '0' || text[offset] > '9') {
      return false;
    }
    uint64_t parsed = 0U;
    while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
      parsed = parsed * 10U + static_cast<unsigned>(text[offset++] - '0');
      if (parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
      }
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
  };
  while (offset < text.size()) {
    uint32_t first = 0U, last = 0U;
    if (!number(&first)) { break; }
    last = first;
    if (offset < text.size() && text[offset] == '-') {
      ++offset;
      if (!number(&last)) { break; }
    }
    if (last < first || static_cast<uint64_t>(last) - first + 1U > 512U - cpus->size()) {
      break;
    }
    for (uint64_t cpu = first; cpu <= last; ++cpu) {
      if (!unique.insert(static_cast<uint32_t>(cpu)).second) {
        set_error(error, field + " contains duplicate CPU " + std::to_string(cpu));
        return false;
      }
      cpus->push_back(static_cast<uint32_t>(cpu));
    }
    if (offset == text.size()) { return true; }
    if (text[offset++] != ',' || offset == text.size()) { break; }
  }
  set_error(error, field + " must be a non-empty CPU list/range (at most 512 CPUs), for example 0-3,8-11");
  return false;
}

bool read_worker_cpus(const YAML::Node &node, const char *field,
                      std::vector<uint32_t> *cpus, std::string *error) {
  if (!has_value(node) || !has_value(node[field])) { return true; }
  std::string value;
  return read_string(node, field, &value, error) &&
      parse_worker_cpus(value, cpus, error, field);
}

bool parse_ub_config(const YAML::Node &pipeline, ktag_demo_ub_config *config,
                     std::string *error) {
  const YAML::Node distributed = pipeline["distributed"];
  if (!has_value(distributed)) {
    return true;
  }
  if (!distributed.IsMap()) {
    set_error(error, "pipeline.distributed must be a map");
    return false;
  }
  const YAML::Node ub = distributed["ub_ring"];
  if (!has_value(ub)) {
    return true;
  }
  if (!ub.IsMap()) {
    set_error(error, "pipeline.distributed.ub_ring must be a map");
    return false;
  }
  config->configured = true;
  config->legacy_credit_configured = has_value(ub["credits_per_provider"]);
  const auto inference = pipeline["inference"];
  config->inference_threads_configured =
      inference.IsMap() && has_value(inference["threads"]);
  std::string cpu_list;
  if (!read_string(ub, "backend_worker_cpus", &cpu_list, error) ||
      (!cpu_list.empty() && !parse_worker_cpus(cpu_list, &config->backend_worker_cpus, error)) ||
      !read_worker_cpus(ub, "sender_worker_cpus", &config->sender_worker_cpus, error) ||
      !read_u32(ub, "sender_threads", &config->sender_threads, error)) {
    return false;
  }
  if (!read_bool(ub, "enabled", &config->enabled, error) ||
      !read_string(ub, "region", &config->region, error) ||
      !read_string(ub, "shm_prefix", &config->shm_prefix, error) ||
      !read_u64(ub, "provider_shm_size", &config->provider_shm_size, error) ||
      !read_u32(ub, "queues_per_provider", &config->queues_per_provider,
                error) ||
      !read_u32(ub, "queue_depth", &config->queue_depth, error) ||
      !read_u32(ub, "data_element_size", &config->data_element_size, error) ||
      !read_u32(ub, "credits_per_queue", &config->credits_per_queue,
                error) ||
      !read_u32(ub, "poll_us", &config->poll_us, error) ||
      !read_string_list(ub["provider_nodes"],
                        "pipeline.distributed.ub_ring.provider_nodes",
                        &config->provider_nodes, error) ||
      !read_string_list(ub["user_nodes"],
                        "pipeline.distributed.ub_ring.user_nodes",
                        &config->user_nodes, error) ||
      !read_ordered_string_list(ub["eal_arguments"],
                                "pipeline.distributed.ub_ring.eal_arguments",
                                &config->eal_arguments, error) ||
      !read_ground_truth_labels(ub["ground_truth_labels"],
                                &config->ground_truth_labels, error)) {
    return false;
  }
  return true;
}

bool validate_ub_config(const ktag_demo_pipeline_config &pipeline,
                        std::string *error) {
  const auto &config = pipeline.ub;
  if (!config.enabled) {
    return true;
  }
  if (!config.configured) {
    set_error(error, "distributed role requires pipeline.distributed.ub_ring");
    return false;
  }
  if (config.role == ktag_demo_distributed_role::disabled ||
      config.node_id.empty()) {
    set_error(error,
              "distributed UB requires --distributed-role and --node-id");
    return false;
  }
  if (config.provider_nodes.empty() || config.user_nodes.size() != 1U) {
    set_error(error,
              "distributed UB requires Provider nodes and exactly one User");
    return false;
  }
  if (config.provider_nodes.size() + config.user_nodes.size() > 256U) {
    set_error(error, "distributed UB node count exceeds the DPDK limit");
    return false;
  }
  std::unordered_set<std::string> all_nodes(config.provider_nodes.begin(),
                                            config.provider_nodes.end());
  if (!all_nodes.insert(config.user_nodes.front()).second) {
    set_error(error, "distributed UB Provider/User hostnames must be unique");
    return false;
  }
  const bool is_provider =
      std::find(config.provider_nodes.begin(), config.provider_nodes.end(),
                config.node_id) != config.provider_nodes.end();
  const bool is_user = config.node_id == config.user_nodes.front();
  if ((config.role == ktag_demo_distributed_role::frontend && !is_provider) ||
      (config.role == ktag_demo_distributed_role::backend && !is_user)) {
    set_error(error,
              "--node-id does not belong to the selected distributed role");
    return false;
  }
  if (config.region.empty() || config.shm_prefix.empty() ||
      config.provider_shm_size == 0U || config.queues_per_provider == 0U ||
      config.queues_per_provider > 512U ||
      config.provider_nodes.size() > 512U / config.queues_per_provider ||
      config.queue_depth < 2U ||
      (config.queue_depth & (config.queue_depth - 1U)) != 0U ||
      config.data_element_size < ktag_demo_ub_feature_frame_size(0U) ||
      config.credits_per_queue == 0U ||
      config.credits_per_queue >= config.queue_depth) {
    set_error(error,
              "distributed UB memory, queue, element or credit values are "
              "invalid");
    return false;
  }
  if (config.provider_shm_size >
      static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
    set_error(error, "distributed UB Provider shm size exceeds size_t");
    return false;
  }
  if (pipeline.backpressure != ktag_demo_backpressure_policy::block) {
    set_error(error,
              "distributed UB requires pipeline.backpressure.policy: block");
    return false;
  }
  if (!config.ground_truth_labels.empty()) {
    if (config.ground_truth_labels.size() != config.provider_nodes.size()) {
      set_error(error, "ground_truth_labels must describe every Provider or be "
                       "omitted");
      return false;
    }
    for (const auto &entry : config.ground_truth_labels) {
      if (std::find(config.provider_nodes.begin(), config.provider_nodes.end(),
                    entry.first) == config.provider_nodes.end()) {
        set_error(error,
                  "ground_truth_labels contains a non-Provider hostname");
        return false;
      }
    }
  }
  if (pipeline.mode != ktag_demo_pipeline_mode::feature_parallel) {
    set_error(error, "distributed UB requires YAML pipeline.mode: feature_parallel; backend workers are configured with inference.threads");
    return false;
  }
  if (config.legacy_credit_configured) {
    set_error(error, "credits_per_provider is no longer supported; configure credits_per_queue in YAML");
    return false;
  }
  const size_t expected = config.provider_nodes.size() * config.queues_per_provider;
  if (!config.inference_threads_configured || pipeline.inference_threads != expected) {
    set_error(error, "UB worker count mismatch: providers=" + std::to_string(config.provider_nodes.size()) +
        ", queues_per_provider=" + std::to_string(config.queues_per_provider) +
        ", expected_workers=" + std::to_string(expected) +
        ", inference.threads=" + std::to_string(pipeline.inference_threads) +
        " (inference.threads must be explicitly configured in YAML)");
    return false;
  }
  if (config.backend_worker_cpus.size() != expected) {
    set_error(error, "backend_worker_cpus must specify exactly " + std::to_string(expected) + " distinct CPUs");
    return false;
  }
  if (pipeline.inference_intra_op_threads != 1U || pipeline.inference_inter_op_threads != 1U) {
    set_error(error, "distributed UB requires intra_op_threads: 1 and inter_op_threads: 1");
    return false;
  }
  if (config.sender_threads == 0U || config.sender_threads > config.queues_per_provider ||
      pipeline.feature_queue_capacity < config.sender_threads) {
    set_error(error, "UB sender_threads must be 1..queues_per_provider and not exceed inference.queue_capacity");
    return false;
  }
  if (pipeline.dpdk_rx_threads == 0U || pipeline.dpdk_rx_threads > 256U ||
      (pipeline.dpdk_rx_threads > 1U && pipeline.dpdk_rx_threads != pipeline.dpdk_rx_queue_count)) {
    set_error(error, "UB rx_threads must be 1, or equal rx_queue_count (at most 256)");
    return false;
  }
  const bool online = pipeline.capture_backend == ktag_demo_capture_backend::dpdk;
  if (pipeline.dpdk_ring_handoff && !online) {
    set_error(error,
              "pipeline.capture.dpdk.handoff: ring requires "
              "pipeline.capture.backend: dpdk");
    return false;
  }
  const bool parallel = config.sender_threads > 1U || (online && pipeline.dpdk_rx_threads > 1U);
  std::unordered_set<uint32_t> frontend_cpus;
  auto check_cpus = [&](const std::vector<uint32_t> &cpus, size_t count,
                         const char *field, bool required, size_t used) {
    if (cpus.empty() && !required) { return true; }
    if (cpus.size() != count) {
      set_error(error, std::string(field) + " must specify exactly " + std::to_string(count) + " CPUs");
      return false;
    }
    for (size_t i = 0; i < used; ++i) {
      if (!frontend_cpus.insert(cpus[i]).second) {
        set_error(error, std::string(field) + " overlaps another Frontend worker CPU: " + std::to_string(cpus[i]));
        return false;
      }
    }
    return true;
  };
  if (!check_cpus(pipeline.dpdk_rx_worker_cpus, pipeline.dpdk_rx_threads, "rx_worker_cpus",
                   parallel && online, online ? pipeline.dpdk_rx_threads : 1U) ||
      !check_cpus(pipeline.feature_worker_cpus, pipeline.feature_threads, "feature.worker_cpus",
                   parallel, pipeline.feature_threads) ||
      !check_cpus(config.sender_worker_cpus, config.sender_threads, "sender_worker_cpus",
                   parallel, config.sender_threads)) { return false; }
  return true;
}

bool resolve_path(const fs::path &config_directory,
                  const std::string &configured, std::string *resolved,
                  std::string *error) {
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

bool parse_mode(const YAML::Node &node, ktag_demo_pipeline_mode *mode,
                std::string *error) {
  const std::string value =
      has_value(node) ? node.as<std::string>() : "single_thread";
  if (!ktag_demo_pipeline_mode_from_name(value.c_str(), mode)) {
    set_error(error, "pipeline.mode must be single_thread, staged, "
                     "feature_parallel or full_parallel");
    return false;
  }
  return true;
}

bool parse_feature_lifecycle_mode(const YAML::Node &node,
                                  ktag_demo_feature_lifecycle_mode *mode,
                                  std::string *error) {
  const std::string value = has_value(node) ? node.as<std::string>() : "legacy";
  if (value == "legacy" || value == "legacy_compat") {
    *mode = ktag_demo_feature_lifecycle_mode::legacy_compat;
  } else if (value == "modern") {
    *mode = ktag_demo_feature_lifecycle_mode::modern;
  } else {
    set_error(error,
              "pipeline.feature.lifecycle_mode must be legacy or modern");
    return false;
  }
  return true;
}

bool parse_backpressure(const YAML::Node &node,
                        ktag_demo_backpressure_policy *policy,
                        std::string *error) {
  const std::string value = has_value(node) ? node.as<std::string>() : "block";
  if (value == "block") {
    *policy = ktag_demo_backpressure_policy::block;
  } else if (value == "drop_newest") {
    *policy = ktag_demo_backpressure_policy::drop_newest;
  } else if (value == "drop_oldest") {
    *policy = ktag_demo_backpressure_policy::drop_oldest;
  } else if (value == "fail") {
    *policy = ktag_demo_backpressure_policy::fail;
  } else {
    set_error(error, "pipeline.backpressure.policy must be block, drop_newest, "
                     "drop_oldest or fail");
    return false;
  }
  return true;
}

bool has_exact_features(const std::vector<std::string> &features,
                        const std::unordered_set<std::string> &required) {
  if (features.size() != required.size()) {
    return false;
  }
  const std::unordered_set<std::string> configured(features.cbegin(),
                                                   features.cend());
  return configured == required;
}

bool proxy_schema_exclusions(const std::string &schema_name,
                             std::unordered_set<std::string> *excluded) {
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
        "legacy.flow_id",   "network.src_ip",   "network.dst_ip",
        "network.src_port", "network.dst_port",
    };
    return true;
  }
  return false;
}

bool validate_frozen_schema_selection(const ktag_demo_pipeline_config &config,
                                      std::string *error) {
  if (config.schema_name == "ktag_legacy_v1") {
    if (config.schema_version != 1U || !config.feature_order.empty() ||
        !config.remove_features.empty() || !config.add_features.empty()) {
      set_error(error,
                "ktag_legacy_v1 is a frozen 53-feature/354-value contract; "
                "custom selections require a different Schema name/version");
      return false;
    }
    return true;
  }
  std::unordered_set<std::string> required_exclusions;
  if (!proxy_schema_exclusions(config.schema_name, &required_exclusions)) {
    return true;
  }
  if (config.schema_version != 1U) {
    set_error(error,
              config.schema_name + " requires pipeline.schema.version: 1");
    return false;
  }
  if (!config.feature_order.empty()) {
    set_error(error,
              config.schema_name +
                  " must use remove_features/add_features so its Built-in "
                  "and Proxy ordering remains auditable");
    return false;
  }
  if (config.plugin_paths.empty()) {
    set_error(error, config.schema_name +
                         " requires the official proxy Feature Plugin");
    return false;
  }
  if (!has_exact_features(config.remove_features, required_exclusions)) {
    set_error(error, config.schema_name +
                         " remove_features does not match its frozen ablation "
                         "contract");
    return false;
  }
  if (config.add_features.size() != 68U) {
    set_error(error, config.schema_name +
                         " add_features must contain all 68 Proxy Feature "
                         "Descriptors in Catalog order");
    return false;
  }
  for (const auto &feature_id : config.add_features) {
    if (feature_id.rfind("proxy.", 0U) != 0U) {
      set_error(error,
                config.schema_name +
                    " add_features may contain only proxy.* Feature IDs");
      return false;
    }
  }
  return true;
}

bool parse_enabled_config(const YAML::Node &pipeline,
                          const fs::path &config_directory, bool feature_only,
                          bool distributed_requested,
                          ktag_demo_pipeline_config *config,
                          std::string *error) {
  if (!parse_mode(pipeline["mode"], &config->mode, error)) {
    return false;
  }
  if (!parse_ub_config(pipeline, &config->ub, error)) {
    return false;
  }
  const bool read_inference = !feature_only || config->ub.enabled || distributed_requested;
  const YAML::Node model_package_path = pipeline["model_package_path"];
  if (!feature_only && has_value(model_package_path)) {
    if (!resolve_path(config_directory, model_package_path.as<std::string>(),
                      &config->model_package_path, error)) {
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
      (read_inference && has_value(inference) && !inference.IsMap()) ||
      (has_value(backpressure) && !backpressure.IsMap()) ||
      (has_value(schema) && !schema.IsMap())) {
    set_error(error, "pipeline nested configuration must use maps");
    return false;
  }

  if (!read_u32(capture, "packet_pool_size", &config->packet_pool_size,
                error) ||
      !read_worker_cpus(feature, "worker_cpus", &config->feature_worker_cpus, error) ||
      !read_u32(feature, "threads", &config->feature_threads, error) ||
      !read_u32(feature, "queue_capacity", &config->packet_queue_capacity,
                error) ||
      !read_u64(feature, "flow_hash_seed", &config->flow_hash_seed, error) ||
      !read_u32(feature, "expire_interval_ms", &config->expire_interval_ms,
                error) ||
      !read_u32(feature, "flow_capacity", &config->flow_capacity, error) ||
      !read_u64(feature, "tcp_idle_timeout_us", &config->tcp_idle_timeout_us,
                error) ||
      !read_u64(feature, "udp_idle_timeout_us", &config->udp_idle_timeout_us,
                error) ||
      !read_u32(feature, "udp_packet_limit", &config->udp_packet_limit,
                error) ||
      !parse_feature_lifecycle_mode(
          has_value(feature) ? feature["lifecycle_mode"] : YAML::Node(),
          &config->lifecycle_mode, error) ||
      !parse_backpressure(has_value(backpressure) ? backpressure["policy"]
                                                  : YAML::Node(),
                          &config->backpressure, error)) {
    return false;
  }
  if (read_inference &&
      (!read_u32(inference, "threads", &config->inference_threads, error) ||
       !read_u32(inference, "batch_size", &config->inference_batch_size,
                 error) ||
       !read_u32(inference, "batch_timeout_us",
                 &config->inference_batch_timeout_us, error) ||
       !read_u32(inference, "queue_capacity", &config->feature_queue_capacity,
                 error) ||
       !read_u32(inference, "intra_op_threads",
                 &config->inference_intra_op_threads, error) ||
       !read_u32(inference, "inter_op_threads",
                 &config->inference_inter_op_threads, error))) {
    return false;
  }

  /* Capture backend selection and optional DPDK settings. */
  const YAML::Node backend_node =
      has_value(capture) ? capture["backend"] : YAML::Node();
  if (has_value(backend_node)) {
    std::string backend_value;
    try {
      backend_value = backend_node.as<std::string>();
    } catch (const YAML::Exception &exception) {
      set_error(error, exception.what());
      return false;
    }
    if (!ktag_demo_capture_backend_from_name(backend_value.c_str(),
                                             &config->capture_backend)) {
      set_error(error, "pipeline.capture.backend must be pcap or dpdk");
      return false;
    }
  }
  const YAML::Node dpdk = has_value(capture) ? capture["dpdk"] : YAML::Node();
  if (has_value(dpdk) && !dpdk.IsMap()) {
    set_error(error, "pipeline.capture.dpdk must be a map");
    return false;
  }
  try {
    if (has_value(dpdk)) {
      if (has_value(dpdk["device_name"])) {
        config->dpdk_device_name = dpdk["device_name"].as<std::string>();
      }
      if (has_value(dpdk["eal_args"])) {
        config->dpdk_eal_args = dpdk["eal_args"].as<std::string>();
      }
      const YAML::Node handoff = dpdk["handoff"];
      if (has_value(handoff)) {
        const std::string value = handoff.as<std::string>();
        if (value == "ring") {
          config->dpdk_ring_handoff = true;
        } else if (value != "queue") {
          set_error(error,
                    "pipeline.capture.dpdk.handoff must be queue or ring");
          return false;
        }
      }
    }
  } catch (const YAML::Exception &exception) {
    set_error(error, exception.what());
    return false;
  }
  if (!read_u32(dpdk, "rx_queue_count", &config->dpdk_rx_queue_count, error) ||
      !read_u32(dpdk, "rx_threads", &config->dpdk_rx_threads, error) ||
      !read_worker_cpus(dpdk, "rx_worker_cpus", &config->dpdk_rx_worker_cpus, error) ||
      !read_u32(dpdk, "rx_queue_depth", &config->dpdk_rx_queue_depth, error) ||
      !read_u32(dpdk, "mbuf_pool_size", &config->dpdk_mbuf_pool_size, error) ||
      !read_u32(dpdk, "burst_size", &config->dpdk_burst_size, error) ||
      !read_u64(dpdk, "rx_idle_eof_us", &config->dpdk_rx_idle_eof_us, error) ||
      !read_u32(dpdk, "poll_interval_us", &config->dpdk_poll_interval_us,
                error)) {
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
          "pipeline.schema.feature_order", &config->feature_order, error) ||
      !read_string_list(
          has_value(schema) ? schema["remove_features"] : YAML::Node(),
          "pipeline.schema.remove_features", &config->remove_features, error) ||
      !read_string_list(
          has_value(schema) ? schema["add_features"] : YAML::Node(),
          "pipeline.schema.add_features", &config->add_features, error)) {
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
  if (!read_string_list(pipeline["plugins"], "pipeline.plugins",
                        &configured_plugins, error)) {
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
      config->packet_pool_size == 0U || config->packet_queue_capacity == 0U ||
      config->feature_queue_capacity == 0U ||
      config->inference_batch_size == 0U ||
      config->inference_batch_timeout_us == 0U ||
      config->feature_threads == 0U || config->inference_threads == 0U) {
    set_error(
        error,
        "pipeline sizes, thread counts and schema version must be positive");
    return false;
  }

  if (!validate_frozen_schema_selection(*config, error)) {
    return false;
  }

  return true;
}

bool apply_overrides_and_normalize(
    const ktag_demo_pipeline_config_overrides *overrides,
    ktag_demo_pipeline_config *config, std::string *error) {
  if (overrides != nullptr) {
    if (overrides->distributed_role_set) {
      config->ub.enabled = true;
      config->ub.role = overrides->distributed_role;
      config->ub.node_id = overrides->node_id;
    }
    if (config->ub.enabled &&
        (overrides->mode_set || overrides->feature_threads != 0U ||
         overrides->inference_threads != 0U || overrides->dpdk_eal_args != nullptr)) {
      set_error(error, "distributed UB execution settings are YAML-only; remove -n/--feature-threads, --inference-threads, --pipeline-mode and --dpdk-eal-args");
      return false;
    }
    config->feature_only =
        overrides->feature_only ||
        (overrides->distributed_role_set &&
         overrides->distributed_role == ktag_demo_distributed_role::frontend);
    if (overrides->mode_set) {
      config->mode = overrides->mode;
    }
    if (overrides->feature_threads != 0U) {
      config->feature_threads = overrides->feature_threads;
    }
    if (overrides->inference_threads != 0U) {
      config->inference_threads = overrides->inference_threads;
    }
    if (overrides->force_dpdk) {
      config->capture_backend = ktag_demo_capture_backend::dpdk;
    }
    if (overrides->dpdk_eal_args != nullptr &&
        overrides->dpdk_eal_args[0] != '\0') {
      config->dpdk_eal_args = overrides->dpdk_eal_args;
    }

    if (!config->ub.enabled && (config->mode == ktag_demo_pipeline_mode::single_thread ||
         config->mode == ktag_demo_pipeline_mode::staged) &&
        ((overrides->feature_threads != 0U &&
          overrides->feature_threads != 1U) ||
         (overrides->inference_threads != 0U &&
          overrides->inference_threads != 1U))) {
      set_error(error, "single_thread/staged require exactly one Feature and "
                       "one Inference worker");
      return false;
    }
    if (!config->ub.enabled && config->mode == ktag_demo_pipeline_mode::feature_parallel &&
        overrides->inference_threads != 0U &&
        overrides->inference_threads != 1U) {
      set_error(error,
                "feature_parallel requires exactly one Inference worker; "
                "use full_parallel for multiple Inference workers");
      return false;
    }
  }

  if (config->ub.enabled) {
    return validate_ub_config(*config, error);
  }
  if (config->dpdk_rx_threads != 1U || config->ub.sender_threads != 1U ||
      !config->dpdk_rx_worker_cpus.empty() || !config->feature_worker_cpus.empty() ||
      !config->ub.sender_worker_cpus.empty()) {
    set_error(error, "Frontend RX/sender threads and worker CPU lists require distributed UB");
    return false;
  }
  if (config->feature_only) {
    if (config->mode == ktag_demo_pipeline_mode::single_thread ||
        config->mode == ktag_demo_pipeline_mode::staged) {
      config->feature_threads = 1U;
      config->inference_threads = 1U;
    } else if (config->mode == ktag_demo_pipeline_mode::feature_parallel) {
      config->inference_threads = 1U;
    } else {
      config->inference_threads = config->feature_threads;
    }
  } else {
    if (config->mode == ktag_demo_pipeline_mode::single_thread ||
        config->mode == ktag_demo_pipeline_mode::staged) {
      config->feature_threads = 1U;
      config->inference_threads = 1U;
    } else if (config->mode == ktag_demo_pipeline_mode::feature_parallel) {
      config->inference_threads = 1U;
    }
  }
  return validate_ub_config(*config, error);
}

} // namespace

ktag_demo_pipeline_config_result
ktag_demo_load_pipeline_config(const char *config_path,
                               ktag_demo_pipeline_config *config,
                               std::string *error) {
  return ktag_demo_load_pipeline_config_with_overrides(config_path, nullptr,
                                                       config, error);
}

ktag_demo_pipeline_config_result ktag_demo_load_pipeline_config_with_overrides(
    const char *config_path,
    const ktag_demo_pipeline_config_overrides *overrides,
    ktag_demo_pipeline_config *config, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (config_path == nullptr || config_path[0] == '\0' || config == nullptr) {
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
        set_error(error, "command line selected the new pipeline, but the YAML "
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
    const bool enabled =
        has_value(enabled_node) ? enabled_node.as<bool>() : false;
    if (!enabled && (overrides == nullptr || !overrides->force_enable)) {
      return ktag_demo_pipeline_config_result::disabled;
    }

    *config = {};
    config->enabled = true;
    if (!hash_config_file(absolute_config, &config->ub.ruleset_hash, error)) {
      return ktag_demo_pipeline_config_result::error;
    }
    const bool feature_only =
        overrides != nullptr &&
        (overrides->feature_only ||
         (overrides->distributed_role_set &&
          overrides->distributed_role == ktag_demo_distributed_role::frontend));
    if (!parse_enabled_config(pipeline, absolute_config.parent_path(),
                              feature_only, overrides != nullptr && overrides->distributed_role_set,
                              config, error)) {
      if (error != nullptr) { *error = absolute_config.string() + ": " + *error; }
      return ktag_demo_pipeline_config_result::error;
    }
    if (!apply_overrides_and_normalize(overrides, config, error)) {
      if (error != nullptr) { *error = absolute_config.string() + ": " + *error; }
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

bool ktag_demo_pipeline_mode_from_name(const char *name,
                                       ktag_demo_pipeline_mode *mode) {
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

bool ktag_demo_capture_backend_from_name(const char *name,
                                         ktag_demo_capture_backend *backend) {
  if (name == nullptr || backend == nullptr) {
    return false;
  }
  if (std::strcmp(name, "pcap") == 0) {
    *backend = ktag_demo_capture_backend::pcap;
  } else if (std::strcmp(name, "dpdk") == 0) {
    *backend = ktag_demo_capture_backend::dpdk;
  } else {
    return false;
  }
  return true;
}

const char *ktag_demo_capture_backend_name(ktag_demo_capture_backend backend) {
  switch (backend) {
  case ktag_demo_capture_backend::pcap:
    return "pcap";
  case ktag_demo_capture_backend::dpdk:
    return "dpdk";
  }
  return "unknown";
}

const char *ktag_demo_pipeline_mode_name(ktag_demo_pipeline_mode mode) {
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

const char *
ktag_demo_feature_lifecycle_mode_name(ktag_demo_feature_lifecycle_mode mode) {
  switch (mode) {
  case ktag_demo_feature_lifecycle_mode::legacy_compat:
    return "legacy";
  case ktag_demo_feature_lifecycle_mode::modern:
    return "modern";
  }
  return "unknown";
}

const char *ktag_demo_backpressure_name(ktag_demo_backpressure_policy policy) {
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

bool ktag_demo_distributed_role_from_name(const char *name,
                                          ktag_demo_distributed_role *role) {
  if (name == nullptr || role == nullptr) {
    return false;
  }
  if (std::strcmp(name, "frontend") == 0) {
    *role = ktag_demo_distributed_role::frontend;
  } else if (std::strcmp(name, "backend") == 0) {
    *role = ktag_demo_distributed_role::backend;
  } else {
    return false;
  }
  return true;
}

const char *ktag_demo_distributed_role_name(ktag_demo_distributed_role role) {
  switch (role) {
  case ktag_demo_distributed_role::disabled:
    return "disabled";
  case ktag_demo_distributed_role::frontend:
    return "frontend";
  case ktag_demo_distributed_role::backend:
    return "backend";
  }
  return "unknown";
}
