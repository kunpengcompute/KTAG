/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_PIPELINE_CONFIG_H
#define KTAG_DEMO_PIPELINE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "bounded_queue.h"

enum class ktag_demo_pipeline_mode {
  single_thread,
  staged,
  feature_parallel,
  full_parallel
};

enum class ktag_demo_feature_lifecycle_mode { legacy_compat, modern };

struct ktag_demo_pipeline_config {
  bool enabled = false;
  bool feature_only = false;
  ktag_demo_pipeline_mode mode = ktag_demo_pipeline_mode::single_thread;
  std::string model_package_path;

  uint32_t packet_pool_size = 65536U;
  uint32_t feature_threads = 4U;
  uint32_t packet_queue_capacity = 65536U;
  uint64_t flow_hash_seed = 2026U;
  uint32_t expire_interval_ms = 1000U;
  uint32_t flow_capacity = 0U;
  uint64_t tcp_idle_timeout_us = 0U;
  uint64_t udp_idle_timeout_us = 0U;
  uint32_t udp_packet_limit = 0U;
  ktag_demo_feature_lifecycle_mode lifecycle_mode =
      ktag_demo_feature_lifecycle_mode::legacy_compat;

  uint32_t inference_threads = 1U;
  uint32_t inference_batch_size = 256U;
  uint32_t feature_queue_capacity = 4096U;
  uint32_t inference_intra_op_threads = 1U;
  uint32_t inference_inter_op_threads = 1U;

  ktag_demo_backpressure_policy backpressure =
      ktag_demo_backpressure_policy::block;

  std::string schema_name = "ktag_legacy_v1";
  uint32_t schema_version = 1U;
  std::vector<std::string> feature_order;
  std::vector<std::string> remove_features;
  std::vector<std::string> add_features;
  std::vector<std::string> plugin_paths;
};

/* Command-line values are optional. Zero thread counts mean "use YAML". */
struct ktag_demo_pipeline_config_overrides {
  bool force_enable = false;
  bool feature_only = false;
  bool mode_set = false;
  ktag_demo_pipeline_mode mode = ktag_demo_pipeline_mode::single_thread;
  uint32_t feature_threads = 0U;
  uint32_t inference_threads = 0U;
};

enum class ktag_demo_pipeline_config_result { enabled, disabled, error };

ktag_demo_pipeline_config_result
ktag_demo_load_pipeline_config(const char *config_path,
                               ktag_demo_pipeline_config *config,
                               std::string *error);

ktag_demo_pipeline_config_result ktag_demo_load_pipeline_config_with_overrides(
    const char *config_path,
    const ktag_demo_pipeline_config_overrides *overrides,
    ktag_demo_pipeline_config *config, std::string *error);

bool ktag_demo_pipeline_mode_from_name(const char *name,
                                       ktag_demo_pipeline_mode *mode);

const char *ktag_demo_pipeline_mode_name(ktag_demo_pipeline_mode mode);
const char *
ktag_demo_feature_lifecycle_mode_name(ktag_demo_feature_lifecycle_mode mode);
const char *ktag_demo_backpressure_name(ktag_demo_backpressure_policy policy);

#endif /* KTAG_DEMO_PIPELINE_CONFIG_H */
