/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_PIPELINE_CONFIG_H
#define KTAG_DEMO_PIPELINE_CONFIG_H

#include <cstdint>
#include <map>
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

enum class ktag_demo_capture_backend { pcap, dpdk };

enum class ktag_demo_distributed_role { disabled, frontend, backend };

struct ktag_demo_ub_config {
  bool configured = false;
  bool enabled = false;
  ktag_demo_distributed_role role = ktag_demo_distributed_role::disabled;
  std::string node_id;
  std::vector<std::string> provider_nodes;
  std::vector<std::string> user_nodes;
  std::string region = "ktag_ub_region";
  std::string shm_prefix = "ktag_feature";
  uint64_t provider_shm_size = 268435456U;
  uint32_t queues_per_provider = 1U;
  uint32_t queue_depth = 1024U;
  /* YAML payload capacity; rte_ub_ring may report a larger internal stride. */
  uint32_t data_element_size = 4096U;
  uint32_t credits_per_queue = 64U;
  std::vector<uint32_t> backend_worker_cpus;
  uint32_t sender_threads = 1U;
  std::vector<uint32_t> sender_worker_cpus;
  bool legacy_credit_configured = false;
  bool inference_threads_configured = false;
  uint32_t poll_us = 50U;
  uint64_t ruleset_hash = 0U;
  std::vector<std::string> eal_arguments;
  std::map<std::string, std::vector<uint32_t>> ground_truth_labels;
};

struct ktag_demo_pipeline_config {
  bool enabled = false;
  bool feature_only = false;
  ktag_demo_pipeline_mode mode = ktag_demo_pipeline_mode::single_thread;
  std::string model_package_path;
  bool pcap_labels_specified = false;
  std::string pcap_labels;

  uint32_t packet_pool_size = 65536U;
  uint32_t feature_threads = 4U;
  std::vector<uint32_t> feature_worker_cpus;
  uint32_t packet_queue_capacity = 65536U;
  uint64_t flow_hash_seed = 2026U;
  uint32_t expire_interval_ms = 1000U;
  uint32_t flow_capacity = 0U;
  uint64_t tcp_idle_timeout_us = 0U;
  uint64_t udp_idle_timeout_us = 0U;
  uint32_t udp_packet_limit = 0U;
  ktag_demo_feature_lifecycle_mode lifecycle_mode =
      ktag_demo_feature_lifecycle_mode::legacy_compat;

  /* Capture backend selection (pcap default; dpdk optional). */
  ktag_demo_capture_backend capture_backend = ktag_demo_capture_backend::pcap;
  /* DPDK backend options; only used when capture_backend == dpdk. */
  std::string dpdk_device_name;
  std::string dpdk_eal_args;
  uint32_t dpdk_rx_queue_count = 1U;
  uint32_t dpdk_rx_threads = 1U;
  /* Capture -> Feature handoff: queue keeps the mutex/condvar path,
   * ring uses an rte_ring of mbuf pointers (no copy). */
  bool dpdk_ring_handoff = false;
  std::vector<uint32_t> dpdk_rx_worker_cpus;
  // Runtime only: captured before the first EAL init narrows the main thread.
  std::vector<uint32_t> initial_allowed_cpus;
  uint32_t dpdk_rx_queue_depth = 1024U;
  uint32_t dpdk_mbuf_pool_size = 0U;
  uint32_t dpdk_burst_size = 64U;
  uint64_t dpdk_rx_idle_eof_us = 0U;
  uint32_t dpdk_poll_interval_us = 0U;

  uint32_t inference_threads = 1U;
  uint32_t inference_batch_size = 256U;
  uint32_t inference_batch_timeout_us = 1000U;
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
  ktag_demo_ub_config ub;
};

/* Command-line values are optional. Zero thread counts mean "use YAML". */
struct ktag_demo_pipeline_config_overrides {
  bool force_enable = false;
  bool feature_only = false;
  bool mode_set = false;
  ktag_demo_pipeline_mode mode = ktag_demo_pipeline_mode::single_thread;
  uint32_t feature_threads = 0U;
  uint32_t inference_threads = 0U;
  bool distributed_role_set = false;
  ktag_demo_distributed_role distributed_role =
      ktag_demo_distributed_role::disabled;
  std::string node_id;
  bool force_dpdk = false;
  const char *dpdk_eal_args = nullptr;
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
bool ktag_demo_distributed_role_from_name(const char *name,
                                          ktag_demo_distributed_role *role);
const char *ktag_demo_distributed_role_name(ktag_demo_distributed_role role);

bool ktag_demo_capture_backend_from_name(const char *name,
                                         ktag_demo_capture_backend *backend);
const char *ktag_demo_capture_backend_name(ktag_demo_capture_backend backend);

#endif /* KTAG_DEMO_PIPELINE_CONFIG_H */
