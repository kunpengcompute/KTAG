/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "pipeline_runner.h"

#include "../utils.h"
#include "bounded_queue.h"
#include "demo_capture_source.h"
#include "feature_plugin_loader.h"
#include "schema_feature_writer.h"

#ifndef KTAG_ENABLE_DPDK_UB
#define KTAG_ENABLE_DPDK_UB 0
#endif

#if KTAG_ENABLE_DPDK_UB
#include "ub_feature_wire.h"
#include "ub_ring_transport.h"
#endif

#include "ktag_capture.h"
#include "ktag_feature.h"
#include "ktag_inference.h"

#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

volatile sig_atomic_t pipeline_interrupted = 0;

void pipeline_signal_handler(int) {
  if (pipeline_interrupted < 2) { pipeline_interrupted = pipeline_interrupted + 1; }
}

class signal_scope {
public:
  signal_scope() {
    struct sigaction action {};
    action.sa_handler = pipeline_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    installed_int_ = sigaction(SIGINT, &action, &old_int_) == 0;
    installed_term_ = sigaction(SIGTERM, &action, &old_term_) == 0;
  }

  ~signal_scope() {
    if (installed_int_) {
      sigaction(SIGINT, &old_int_, nullptr);
    }
    if (installed_term_) {
      sigaction(SIGTERM, &old_term_, nullptr);
    }
  }

private:
  struct sigaction old_int_ {};
  struct sigaction old_term_ {};
  bool installed_int_ = false;
  bool installed_term_ = false;
};

struct schema_deleter {
  void operator()(ktag_feature_schema *value) const {
    ktag_feature_schema_destroy(value);
  }
};

struct registry_deleter {
  void operator()(ktag_feature_registry *value) const {
    ktag_feature_registry_destroy(value);
  }
};

struct domain_store_deleter {
  void operator()(ktag_domain_store *value) const {
    ktag_domain_store_destroy(value);
  }
};

struct feature_context_deleter {
  void operator()(ktag_feature_context *value) const {
    ktag_feature_destroy(value);
  }
};

struct feature_batch_deleter {
  void operator()(ktag_feature_batch *value) const {
    ktag_feature_batch_destroy(value);
  }
};

struct inference_context_deleter {
  void operator()(ktag_inference_context *value) const {
    ktag_inference_destroy(value);
  }
};

struct prediction_batch_deleter {
  void operator()(ktag_prediction_batch *value) const {
    ktag_prediction_batch_destroy(value);
  }
};

using schema_ptr = std::unique_ptr<ktag_feature_schema, schema_deleter>;
using registry_ptr = std::unique_ptr<ktag_feature_registry, registry_deleter>;
using domain_store_ptr =
    std::unique_ptr<ktag_domain_store, domain_store_deleter>;
using feature_context_ptr =
    std::unique_ptr<ktag_feature_context, feature_context_deleter>;
using feature_batch_ptr =
    std::unique_ptr<ktag_feature_batch, feature_batch_deleter>;
using inference_context_ptr =
    std::unique_ptr<ktag_inference_context, inference_context_deleter>;
using prediction_batch_ptr =
    std::unique_ptr<ktag_prediction_batch, prediction_batch_deleter>;

class owned_packet {
public:
  owned_packet() { ktag_packet_init(&packet_); }

  explicit owned_packet(ktag_packet *source) {
    packet_ = *source;
    ktag_packet_init(source);
  }

  ~owned_packet() { ktag_packet_release(&packet_); }

  owned_packet(const owned_packet &) = delete;
  owned_packet &operator=(const owned_packet &) = delete;

  owned_packet(owned_packet &&other) noexcept {
    packet_ = other.packet_;
    ktag_packet_init(&other.packet_);
  }

  owned_packet &operator=(owned_packet &&other) noexcept {
    if (this != &other) {
      ktag_packet_release(&packet_);
      packet_ = other.packet_;
      ktag_packet_init(&other.packet_);
    }
    return *this;
  }

  const ktag_packet *get() const { return &packet_; }

private:
  ktag_packet packet_{};
};

using feature_record_owned = ktag_demo_schema_feature_record;
using pipeline_prediction = ktag_demo_pipeline_prediction;
using feature_record_handler =
    std::function<bool(feature_record_owned &&, std::string *)>;

struct capture_statistics {
  uint64_t packets = 0U;
  uint64_t bytes = 0U;
  uint64_t unsupported = 0U;
  uint64_t pool_waits = 0U;
  uint64_t read_nanoseconds = 0U;
};

struct feature_worker_statistics {
  uint64_t packets = 0U;
  uint64_t emitted_flows = 0U;
  uint64_t rejected_packets = 0U;
  uint64_t elapsed_nanoseconds = 0U;
  uint64_t work_nanoseconds = 0U;
  std::chrono::steady_clock::time_point first_process_at{};
};

struct inference_worker_statistics {
  uint64_t batches = 0U;
  uint64_t input_records = 0U;
  uint64_t predictions = 0U;
  uint64_t elapsed_nanoseconds = 0U;
  uint64_t work_nanoseconds = 0U;
};

struct pipeline_capture_queue_statistics {
    uint32_t queue_id = 0U;
    uint64_t received = 0U;
    uint64_t delivered = 0U;
    uint64_t pending = 0U;
};

struct pipeline_source_statistics {
  bool capture_stats_available = false;
  uint64_t capture_received = 0U;
  uint64_t capture_delivered = 0U;
  uint64_t capture_pending = 0U;
  uint64_t capture_dropped = 0U;
  uint64_t capture_discarded = 0U;
  std::vector<pipeline_capture_queue_statistics> capture_queues;
  uint64_t packets = 0U;
  uint64_t bytes = 0U;
  uint64_t unsupported_packets = 0U;
  uint64_t pool_waits = 0U;
  uint64_t emitted_flows = 0U;
  uint64_t inference_inputs = 0U;
  uint64_t predictions = 0U;
  uint64_t capture_nanoseconds = 0U;
  uint64_t feature_nanoseconds = 0U;
  uint64_t feature_wall_nanoseconds = 0U;
  uint64_t feature_work_sum_nanoseconds = 0U;
  uint64_t inference_nanoseconds = 0U;
  uint64_t elapsed_nanoseconds = 0U;
  uint64_t queue_drops = 0U;
  uint64_t queue_failures = 0U;
  std::string partial_capture_error;
  std::chrono::steady_clock::time_point first_feature_at{};
};

struct pipeline_run_statistics {
  bool capture_stats_available = false;
  uint64_t capture_received = 0U;
  uint64_t capture_delivered = 0U;
  uint64_t capture_pending = 0U;
  uint64_t capture_dropped = 0U;
  uint64_t capture_discarded = 0U;
  std::vector<pipeline_capture_queue_statistics> capture_queues;
  uint64_t packets = 0U;
  uint64_t bytes = 0U;
  uint64_t unsupported_packets = 0U;
  uint64_t pool_waits = 0U;
  uint64_t emitted_flows = 0U;
  uint64_t inference_inputs = 0U;
  uint64_t predictions = 0U;
  uint64_t capture_nanoseconds = 0U;
  uint64_t feature_nanoseconds = 0U;
  uint64_t feature_wall_nanoseconds = 0U;
  uint64_t feature_work_sum_nanoseconds = 0U;
  uint64_t inference_nanoseconds = 0U;
  uint64_t file_io_nanoseconds = 0U;
  uint64_t elapsed_nanoseconds = 0U;
  uint64_t queue_drops = 0U;
  uint64_t queue_failures = 0U;
  std::chrono::steady_clock::time_point first_feature_at{};
  uint64_t distributed_core_nanoseconds = 0U;
  uint64_t distributed_inference_wall_nanoseconds = 0U;
};

uint64_t
elapsed_nanoseconds(const std::chrono::steady_clock::time_point &start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

bool is_recoverable_truncated_capture(bool live, ktag_status status,
                                      uint64_t packet_count,
                                      const char *message) {
  return !live && status == KTAG_ERROR_IO && packet_count != 0U &&
         message != nullptr && std::strstr(message, "truncated") != nullptr;
}

/* Copies backend RX counters, if the backend tracks them (DPDK does). */
void collect_capture_statistics(const demo_capture_source *capture,
                                pipeline_source_statistics *statistics) {
    if (capture == nullptr || statistics == nullptr) {
        return;
    }
    demo_capture_stats capture_stats;
    capture->get_capture_stats(&capture_stats);
    if (!capture_stats.available) {
        return;
    }
    statistics->capture_stats_available = true;
    statistics->capture_received = capture_stats.received;
    statistics->capture_delivered = capture_stats.delivered;
    statistics->capture_pending = capture_stats.pending;
    statistics->capture_dropped = capture_stats.dropped;
    statistics->capture_discarded = capture_stats.discarded;
    statistics->capture_queues.clear();
    statistics->capture_queues.reserve(capture_stats.queues.size());
    for (const auto &queue : capture_stats.queues) {
        pipeline_capture_queue_statistics entry;
        entry.queue_id = queue.queue_id;
        entry.received = queue.received;
        entry.delivered = queue.delivered;
        entry.pending = queue.pending;
        statistics->capture_queues.push_back(entry);
    }
}

void add_source_statistics(pipeline_run_statistics *total,
                           const pipeline_source_statistics &source) {
  if (source.capture_stats_available) {
    total->capture_stats_available = true;
    total->capture_received += source.capture_received;
    total->capture_delivered += source.capture_delivered;
    total->capture_pending += source.capture_pending;
    total->capture_dropped += source.capture_dropped;
    total->capture_discarded += source.capture_discarded;
    /* Sources index their queues from 0, so merge by queue id. */
    for (const auto &queue : source.capture_queues) {
      bool merged = false;
      for (auto &existing : total->capture_queues) {
        if (existing.queue_id == queue.queue_id) {
          existing.received += queue.received;
          existing.delivered += queue.delivered;
          existing.pending += queue.pending;
          merged = true;
          break;
        }
      }
      if (!merged) {
        total->capture_queues.push_back(queue);
      }
    }
  }

  if (source.first_feature_at != std::chrono::steady_clock::time_point{} &&
      (total->first_feature_at == std::chrono::steady_clock::time_point{} ||
       source.first_feature_at < total->first_feature_at)) {
    total->first_feature_at = source.first_feature_at;
  }
  total->packets += source.packets;
  total->bytes += source.bytes;
  total->unsupported_packets += source.unsupported_packets;
  total->pool_waits += source.pool_waits;
  total->emitted_flows += source.emitted_flows;
  total->inference_inputs += source.inference_inputs;
  total->predictions += source.predictions;
  total->capture_nanoseconds += source.capture_nanoseconds;
  total->feature_nanoseconds += source.feature_nanoseconds;
  total->feature_wall_nanoseconds += source.feature_wall_nanoseconds;
  total->feature_work_sum_nanoseconds += source.feature_work_sum_nanoseconds;
  total->inference_nanoseconds += source.inference_nanoseconds;
  total->queue_drops += source.queue_drops;
  total->queue_failures += source.queue_failures;
}

uint64_t timestamp_us(const ktag_timestamp &timestamp) {
  return static_cast<uint64_t>(timestamp.seconds) * 1000000ULL +
         static_cast<uint32_t>(timestamp.microseconds);
}

bool flow_key_less(const ktag_flow_key &left, const ktag_flow_key &right) {
  if (left.address_a.version != right.address_a.version) {
    return left.address_a.version < right.address_a.version;
  }
  int comparison = std::memcmp(left.address_a.bytes, right.address_a.bytes,
                               KTAG_IPV6_ADDRESS_SIZE);
  if (comparison != 0) {
    return comparison < 0;
  }
  if (left.port_a != right.port_a) {
    return left.port_a < right.port_a;
  }
  if (left.address_b.version != right.address_b.version) {
    return left.address_b.version < right.address_b.version;
  }
  comparison = std::memcmp(left.address_b.bytes, right.address_b.bytes,
                           KTAG_IPV6_ADDRESS_SIZE);
  if (comparison != 0) {
    return comparison < 0;
  }
  if (left.port_b != right.port_b) {
    return left.port_b < right.port_b;
  }
  return left.protocol < right.protocol;
}

bool prediction_less(const pipeline_prediction &left,
                     const pipeline_prediction &right) {
  if (left.frontend_id_hash != right.frontend_id_hash) {
    return left.frontend_id_hash < right.frontend_id_hash;
  }
  if (left.source_index != right.source_index) {
    return left.source_index < right.source_index;
  }
  const auto &left_metadata = left.record.metadata;
  const auto &right_metadata = right.record.metadata;
  if (flow_key_less(left_metadata.flow_key, right_metadata.flow_key)) {
    return true;
  }
  if (flow_key_less(right_metadata.flow_key, left_metadata.flow_key)) {
    return false;
  }
  if (left_metadata.first_packet_sequence !=
      right_metadata.first_packet_sequence) {
    return left_metadata.first_packet_sequence <
           right_metadata.first_packet_sequence;
  }
  if (left_metadata.last_packet_sequence !=
      right_metadata.last_packet_sequence) {
    return left_metadata.last_packet_sequence <
           right_metadata.last_packet_sequence;
  }
  return left.record.label < right.record.label;
}

std::string address_text(const ktag_ip_address &address) {
  char buffer[INET6_ADDRSTRLEN] = {};
  const int family = address.version == 4U ? AF_INET : AF_INET6;
  if (address.version != 4U && address.version != 6U) {
    return "unknown";
  }
  return inet_ntop(family, address.bytes, buffer, sizeof(buffer)) == nullptr
             ? "invalid"
             : std::string(buffer);
}

std::string flow_text(const ktag_flow_key &key) {
  return ktag_demo_schema_flow_key_text(key);
}

bool append_predictions(
    const ktag_prediction_batch *batch, const ktag_feature_batch *features,
    const std::vector<ktag_feature_record_details> *details_override,
    uint32_t source_index, std::vector<pipeline_prediction> *predictions,
    std::string *error) {
  if (batch == nullptr || features == nullptr || predictions == nullptr ||
      error == nullptr) {
    return false;
  }
  const uint32_t count = ktag_prediction_batch_get_count(batch);
  try {
    for (uint32_t index = 0U; index < count; ++index) {
      pipeline_prediction result;
      result.frontend_id_hash = 0U;
      result.source_index = source_index;
      const ktag_status status =
          ktag_prediction_batch_get_record(batch, index, &result.record);
      if (status != KTAG_SUCCESS) {
        *error = "failed to read Prediction Batch record";
        return false;
      }
      const uint32_t record_index = result.record.source_record_index;
      if (details_override != nullptr) {
        if (record_index >= details_override->size()) {
          *error = "Prediction source index exceeds retained metadata";
          return false;
        }
        result.details = (*details_override)[record_index];
      } else if (ktag_feature_batch_get_record_details(
                     features, record_index, &result.details) != KTAG_SUCCESS) {
        *error = "failed to recover Prediction source metadata";
        return false;
      }
      predictions->push_back(result);
    }
    return true;
  } catch (...) {
    *error = "out of memory collecting predictions";
    return false;
  }
}

#if KTAG_ENABLE_DPDK_UB
bool append_distributed_predictions(
    const ktag_prediction_batch *batch,
    const std::vector<ktag_feature_record_details> &details,
    const std::vector<uint32_t> &source_indices,
    const std::vector<uint64_t> &frontend_hashes,
    const std::vector<uint8_t> &ground_truth_valid,
    std::vector<pipeline_prediction> *predictions, std::string *error) {
  if (batch == nullptr || predictions == nullptr || error == nullptr ||
      details.size() != source_indices.size() ||
      details.size() != frontend_hashes.size() ||
      details.size() != ground_truth_valid.size()) {
    if (error != nullptr) {
      *error = "distributed Prediction metadata is inconsistent";
    }
    return false;
  }
  const uint32_t count = ktag_prediction_batch_get_count(batch);
  try {
    for (uint32_t index = 0U; index < count; ++index) {
      pipeline_prediction result;
      const ktag_status status =
          ktag_prediction_batch_get_record(batch, index, &result.record);
      if (status != KTAG_SUCCESS) {
        *error = "failed to read distributed Prediction Batch record";
        return false;
      }
      const uint32_t record_index = result.record.source_record_index;
      if (record_index >= details.size()) {
        *error = "distributed Prediction source index exceeds its Batch";
        return false;
      }
      result.details = details[record_index];
      result.source_index = source_indices[record_index];
      result.frontend_id_hash = frontend_hashes[record_index];
      result.ground_truth_valid = ground_truth_valid[record_index] != 0U;
      predictions->push_back(result);
    }
    return true;
  } catch (...) {
    *error = "out of memory collecting distributed Predictions";
    return false;
  }
}
#endif

bool copy_feature_records(const ktag_feature_batch *batch,
                          uint32_t source_index,
                          std::vector<feature_record_owned> *records,
                          std::string *error) {
  const uint32_t count = ktag_feature_batch_get_count(batch);
  try {
    for (uint32_t index = 0U; index < count; ++index) {
      ktag_feature_record view{};
      const ktag_status status =
          ktag_feature_batch_get_record(batch, index, &view);
      if (status != KTAG_SUCCESS) {
        *error = "failed to read Feature Batch record";
        return false;
      }
      feature_record_owned record;
      record.source_index = source_index;
      if (ktag_feature_batch_get_record_details(
              batch, index, &record.details) != KTAG_SUCCESS) {
        *error = "failed to read Feature Batch record details";
        return false;
      }
      record.values.assign(view.values, view.values + view.value_count);
      records->push_back(std::move(record));
    }
    return true;
  } catch (...) {
    *error = "out of memory copying Feature Batch records";
    return false;
  }
}

domain_store_ptr create_domain_store() {
  ktag_domain_store_config config{};
  config.struct_size = sizeof(config);
  config.api_version = KTAG_API_VERSION;
  config.max_entries = KTAG_DOMAIN_DEFAULT_MAX_ENTRIES;
  config.journal_capacity = KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY;
  return domain_store_ptr(ktag_domain_store_create(&config));
}

feature_context_ptr
create_feature_context(const ktag_demo_pipeline_config &pipeline_config,
                       const ktag_feature_schema *schema,
                       const ktag_feature_registry *registry,
                       ktag_domain_store *domain_store) {
  ktag_feature_config config{};
  config.struct_size = sizeof(config);
  config.api_version = KTAG_API_VERSION;
  config.schema = schema;
  config.registry = registry;
  config.shared_domain_store = domain_store;
  config.flow_capacity = pipeline_config.flow_capacity;
  config.tcp_idle_timeout_us = pipeline_config.tcp_idle_timeout_us;
  config.udp_idle_timeout_us = pipeline_config.udp_idle_timeout_us;
  config.udp_packet_limit = pipeline_config.udp_packet_limit;
  config.domain_authority_lookup_on_miss = 0U;
  config.lifecycle_mode =
      pipeline_config.lifecycle_mode ==
              ktag_demo_feature_lifecycle_mode::legacy_compat
          ? KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT
          : KTAG_FEATURE_LIFECYCLE_MODERN;
  return feature_context_ptr(ktag_feature_create(&config));
}

inference_context_ptr
create_inference_context(const ktag_demo_pipeline_config &pipeline_config,
                         const ktag_feature_schema *schema) {
  ktag_inference_config config{};
  config.struct_size = sizeof(config);
  config.api_version = KTAG_API_VERSION;
  config.model_package_path = pipeline_config.model_package_path.c_str();
  config.feature_schema = schema;
  config.intra_op_threads = pipeline_config.inference_intra_op_threads;
  config.inter_op_threads = pipeline_config.inference_inter_op_threads;
  return inference_context_ptr(ktag_inference_create(&config));
}

schema_ptr compose_schema(const ktag_demo_pipeline_config &config,
                          const ktag_feature_registry *registry,
                          std::string *error) {
  schema_ptr result(ktag_feature_schema_create(config.schema_name.c_str(),
                                               config.schema_version));
  if (!result) {
    *error = "failed to create configured Feature Schema";
    return {};
  }

  std::vector<std::string> features;
  if (!config.feature_order.empty()) {
    features = config.feature_order;
  } else {
    schema_ptr legacy(ktag_feature_schema_create_legacy_v1());
    if (!legacy) {
      *error = "failed to create Legacy Schema template";
      return {};
    }
    const std::unordered_set<std::string> removed(
        config.remove_features.cbegin(), config.remove_features.cend());
    std::unordered_set<std::string> found_removed;
    for (uint32_t index = 0U;
         index < ktag_feature_schema_get_feature_count(legacy.get()); ++index) {
      ktag_feature_descriptor descriptor{};
      uint32_t offset = 0U;
      if (ktag_feature_schema_get_descriptor(legacy.get(), index, &descriptor,
                                             &offset) != KTAG_SUCCESS) {
        *error = "failed to read Legacy Schema template";
        return {};
      }
      if (removed.find(descriptor.feature_id) == removed.end()) {
        features.emplace_back(descriptor.feature_id);
      } else {
        found_removed.emplace(descriptor.feature_id);
      }
    }
    if (found_removed.size() != removed.size()) {
      for (const auto &feature_id : removed) {
        if (found_removed.find(feature_id) == found_removed.end()) {
          *error = "remove_features contains an unknown legacy "
                   "feature: " +
                   feature_id;
          return {};
        }
      }
    }
    features.insert(features.end(), config.add_features.cbegin(),
                    config.add_features.cend());
  }

  for (const auto &feature_id : features) {
    const ktag_status status = ktag_feature_schema_add_registered(
        result.get(), registry, feature_id.c_str());
    if (status != KTAG_SUCCESS) {
      *error =
          "configured feature is missing, duplicated or invalid: " + feature_id;
      return {};
    }
  }
  if (ktag_feature_schema_freeze(result.get()) != KTAG_SUCCESS) {
    *error = "failed to freeze configured Feature Schema";
    return {};
  }
  return result;
}

bool schema_contains_feature(const ktag_feature_schema *schema,
                             const char *feature_id) {
  ktag_feature_descriptor descriptor{};
  uint32_t value_offset = 0U;
  return ktag_feature_schema_find(schema, feature_id, &descriptor,
                                  &value_offset) == KTAG_SUCCESS;
}

uint32_t count_features_with_prefix(const ktag_feature_schema *schema,
                                    const char *prefix) {
  const size_t prefix_length = std::strlen(prefix);
  uint32_t count = 0U;
  const uint32_t feature_count = ktag_feature_schema_get_feature_count(schema);
  for (uint32_t index = 0U; index < feature_count; ++index) {
    ktag_feature_descriptor descriptor{};
    uint32_t value_offset = 0U;
    if (ktag_feature_schema_get_descriptor(schema, index, &descriptor,
                                           &value_offset) == KTAG_SUCCESS &&
        std::strncmp(descriptor.feature_id, prefix, prefix_length) == 0) {
      ++count;
    }
  }
  return count;
}

bool validate_frozen_schema_contract(const ktag_demo_pipeline_config &config,
                                     const ktag_feature_schema *schema,
                                     std::string *error) {
  constexpr uint64_t legacy_v1_hash = 0xdd48882b53bebfa5ULL;
  const uint32_t feature_count = ktag_feature_schema_get_feature_count(schema);
  const uint32_t value_count = ktag_feature_schema_get_value_count(schema);
  const uint64_t schema_hash = ktag_feature_schema_get_hash(schema);
  const char *actual_name = ktag_feature_schema_get_name(schema);
  if (actual_name == nullptr || config.schema_name != actual_name ||
      config.schema_version != ktag_feature_schema_get_version(schema)) {
    *error = "composed Feature Schema identity does not match configuration";
    return false;
  }

  if (config.schema_name == "ktag_legacy_v1") {
    if (config.schema_version != 1U || feature_count != 53U ||
        value_count != 354U || schema_hash != legacy_v1_hash) {
      *error = "ktag_legacy_v1 contract mismatch: expected version=1, "
               "feature_count=53, value_count=354 and "
               "hash=0xdd48882b53bebfa5";
      return false;
    }
    return true;
  }
  uint32_t expected_feature_count = 0U;
  uint32_t expected_value_count = 0U;
  uint64_t expected_hash = 0U;
  std::unordered_set<std::string> excluded;
  std::unordered_set<std::string> required{
      "network.protocol",
      "traffic.send.byte_count",
      "traffic.receive.byte_count",
      "rawbow.crc_histogram",
  };
  if (config.schema_name == "proxy_endpoint_ablation_v1") {
    expected_feature_count = 120U;
    expected_value_count = 621U;
    expected_hash = 0xd05c3d9b98a6062fULL;
    excluded = {"legacy.flow_id"};
    required.insert("network.src_ip");
    required.insert("network.dst_ip");
    required.insert("network.src_port");
    required.insert("network.dst_port");
  } else if (config.schema_name == "ktag_proxy_v1") {
    expected_feature_count = 118U;
    expected_value_count = 619U;
    expected_hash = 0xca1eae5086b41ee5ULL;
    excluded = {
        "legacy.flow_id",
        "network.src_ip",
        "network.dst_ip",
    };
    required.insert("network.src_port");
    required.insert("network.dst_port");
  } else if (config.schema_name == "proxy_no_endpoint_ablation_v1") {
    expected_feature_count = 116U;
    expected_value_count = 617U;
    expected_hash = 0xe307cb37ec0a70c8ULL;
    excluded = {
        "legacy.flow_id",   "network.src_ip",   "network.dst_ip",
        "network.src_port", "network.dst_port",
    };
  } else {
    return true;
  }

  if (config.schema_version != 1U || feature_count != expected_feature_count ||
      value_count != expected_value_count || schema_hash != expected_hash) {
    std::ostringstream message;
    message << config.schema_name
            << " contract mismatch: expected version=1, feature_count="
            << expected_feature_count
            << ", value_count=" << expected_value_count << " and hash=0x"
            << std::hex << std::setw(16) << std::setfill('0') << expected_hash
            << "; change the Schema name/version before changing "
               "Feature order";
    *error = message.str();
    return false;
  }
  for (const auto &feature_id : excluded) {
    if (schema_contains_feature(schema, feature_id.c_str())) {
      *error = config.schema_name + " contains excluded Feature: " + feature_id;
      return false;
    }
  }
  for (const auto &feature_id : required) {
    if (!schema_contains_feature(schema, feature_id.c_str())) {
      *error =
          config.schema_name + " is missing required Feature: " + feature_id;
      return false;
    }
  }
  if (count_features_with_prefix(schema, "proxy.") != 68U) {
    *error =
        config.schema_name + " must contain all 68 proxy.* Feature Descriptors";
    return false;
  }
  return true;
}

bool is_frozen_proxy_experiment(const std::string &schema_name) {
  return schema_name == "proxy_endpoint_ablation_v1" ||
         schema_name == "ktag_proxy_v1" ||
         schema_name == "proxy_no_endpoint_ablation_v1";
}

bool run_inference_batch(ktag_inference_context *inference,
                         const ktag_feature_batch *features,
                         ktag_prediction_batch *prediction_batch,
                         uint32_t source_index,
                         std::vector<pipeline_prediction> *predictions,
                         std::string *error) {
  const ktag_status status =
      ktag_inference_predict(inference, features, prediction_batch);
  if (status != KTAG_SUCCESS) {
    *error =
        std::string("inference failed: ") + ktag_inference_get_last_error();
    return false;
  }
  return append_predictions(prediction_batch, features, nullptr, source_index,
                            predictions, error);
}

bool process_feature_output_single(
    bool feature_only, ktag_inference_context *inference,
    ktag_feature_batch *features, ktag_prediction_batch *prediction_batch,
    uint32_t source_index, std::vector<pipeline_prediction> *predictions,
    std::vector<ktag_demo_schema_feature_record> *feature_records,
    pipeline_source_statistics *statistics, std::string *error) {
  const uint32_t input_count = ktag_feature_batch_get_count(features);
  if (input_count == 0U) {
    return true;
  }
  statistics->emitted_flows += input_count;
  if (feature_only) {
    if (feature_records == nullptr) {
      *error = "Schema Feature Record output is required";
      return false;
    }
    return copy_feature_records(features, source_index, feature_records, error);
  }
  if (inference == nullptr || prediction_batch == nullptr ||
      predictions == nullptr) {
    *error = "Inference output contexts are required";
    return false;
  }
  statistics->inference_inputs += input_count;
  const size_t prediction_start = predictions->size();
  const auto inference_start = std::chrono::steady_clock::now();
  const bool success = run_inference_batch(
      inference, features, prediction_batch, source_index, predictions, error);
  statistics->inference_nanoseconds += elapsed_nanoseconds(inference_start);
  statistics->predictions += predictions->size() - prediction_start;
  return success;
}

bool run_single_source(
    const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
    const ktag_feature_registry *registry, const char *source, bool live,
    uint32_t source_index, std::vector<pipeline_prediction> *predictions,
    std::vector<ktag_demo_schema_feature_record> *feature_records,
    pipeline_source_statistics *statistics, std::string *error) {
  *statistics = pipeline_source_statistics{};
  const auto execution_start = std::chrono::steady_clock::now();
  std::unique_ptr<demo_capture_source> capture =
      create_demo_capture_source(config, source, live, false, error);
  if (!capture) {
    return false;
  }
  domain_store_ptr domain_store = create_domain_store();
  feature_context_ptr feature =
      create_feature_context(config, schema, registry, domain_store.get());
  inference_context_ptr inference;
  if (!config.feature_only) {
    inference = create_inference_context(config, schema);
  }
  feature_batch_ptr feature_batch(ktag_feature_batch_create(schema, 64U));
  prediction_batch_ptr prediction_batch;
  if (!config.feature_only) {
    prediction_batch.reset(
        ktag_prediction_batch_create(config.inference_batch_size));
  }
  if (!domain_store || !feature || !feature_batch ||
      (!config.feature_only && (!inference || !prediction_batch))) {
    *error = std::string("failed to create single-thread pipeline contexts: ") +
             ktag_inference_get_last_error();
    return false;
  }

  uint64_t packet_count = 0U;
  uint64_t last_expire_us = 0U;
  ktag_packet packet{};
  ktag_packet_init(&packet);
  while (pipeline_interrupted == 0) {
    const auto capture_start = std::chrono::steady_clock::now();
    const ktag_status capture_status = capture->next(&packet);
    statistics->capture_nanoseconds += elapsed_nanoseconds(capture_start);
    if (capture_status == KTAG_STATUS_TIMEOUT) {
      continue;
    }
    if (capture_status == KTAG_STATUS_END_OF_STREAM) {
      break;
    }
    if (capture_status != KTAG_SUCCESS) {
      const char *capture_error = capture->last_error();
      const char *capture_message =
          capture_error == nullptr ? "" : capture_error;
      if (is_recoverable_truncated_capture(live, capture_status, packet_count,
                                           capture_message)) {
        statistics->partial_capture_error = capture_message;
        ktag_packet_release(&packet);
        break;
      }
      *error = std::string("capture read failed: ") + capture_message;
      ktag_packet_release(&packet);
      return false;
    }
    ++packet_count;
    statistics->bytes += packet.captured_length;
    const uint64_t now_us = timestamp_us(packet.timestamp);
    const auto feature_start = std::chrono::steady_clock::now();
    const ktag_status feature_status = ktag_feature_process_packet(
        feature.get(), &packet, feature_batch.get());
    statistics->feature_nanoseconds += elapsed_nanoseconds(feature_start);
    ktag_packet_release(&packet);
    if (feature_status == KTAG_ERROR_NOT_SUPPORTED ||
        feature_status == KTAG_ERROR_INVALID_ARGUMENT) {
      ++statistics->unsupported_packets;
      continue;
    }
    if (feature_status != KTAG_SUCCESS ||
        !process_feature_output_single(
            config.feature_only, inference.get(), feature_batch.get(),
            prediction_batch.get(), source_index, predictions, feature_records,
            statistics, error)) {
      if (feature_status != KTAG_SUCCESS) {
        *error = "feature processing failed with status " +
                 std::to_string(feature_status);
      }
      return false;
    }
    const uint64_t interval_us =
        static_cast<uint64_t>(config.expire_interval_ms) * 1000U;
    if (interval_us != 0U &&
        (last_expire_us == 0U || (now_us >= last_expire_us &&
                                  now_us - last_expire_us >= interval_us))) {
      const auto expire_start = std::chrono::steady_clock::now();
      const ktag_status expire_status =
          ktag_feature_expire(feature.get(), now_us, feature_batch.get());
      statistics->feature_nanoseconds += elapsed_nanoseconds(expire_start);
      if (expire_status != KTAG_SUCCESS ||
          !process_feature_output_single(
              config.feature_only, inference.get(), feature_batch.get(),
              prediction_batch.get(), source_index, predictions,
              feature_records, statistics, error)) {
        if (expire_status != KTAG_SUCCESS) {
          *error = "feature expiration failed";
        }
        return false;
      }
      last_expire_us = now_us;
    }
  }

  const auto flush_start = std::chrono::steady_clock::now();
  const ktag_status flush_status =
      ktag_feature_flush(feature.get(), feature_batch.get());
  statistics->feature_nanoseconds += elapsed_nanoseconds(flush_start);
  if (flush_status != KTAG_SUCCESS ||
      !process_feature_output_single(
          config.feature_only, inference.get(), feature_batch.get(),
          prediction_batch.get(), source_index, predictions, feature_records,
          statistics, error)) {
    if (flush_status != KTAG_SUCCESS) {
      *error = "feature flush failed";
    }
    return false;
  }
  ktag_feature_statistics feature_statistics{};
  feature_statistics.struct_size = sizeof(feature_statistics);
  feature_statistics.api_version = KTAG_API_VERSION;
  if (ktag_feature_get_statistics(feature.get(), &feature_statistics) !=
      KTAG_SUCCESS) {
    *error = "failed to collect Feature Context statistics";
    return false;
  }
  statistics->unsupported_packets = std::max(
      statistics->unsupported_packets, feature_statistics.rejected_packets);
  statistics->packets = packet_count;
  statistics->elapsed_nanoseconds = elapsed_nanoseconds(execution_start);
  collect_capture_statistics(capture.get(), statistics);
  return true;
}

class parallel_source_executor {
public:
  parallel_source_executor(const ktag_demo_pipeline_config &config,
                           const ktag_feature_schema *schema,
                           const ktag_feature_registry *registry,
                           uint32_t source_index,
                           feature_record_handler record_handler = {})
      : config_(config), schema_(schema), registry_(registry),
        source_index_(source_index), record_handler_(std::move(record_handler)),
        feature_queue_(config.feature_queue_capacity),
        active_feature_workers_(config.feature_threads),
        feature_stats_(config.feature_threads),
        inference_stats_(config.inference_threads) {
    packet_queues_.reserve(config.feature_threads);
    for (uint32_t index = 0U; index < config.feature_threads; ++index) {
      packet_queues_.emplace_back(new ktag_demo_bounded_queue<owned_packet>(
          config.packet_queue_capacity));
    }
  }

  bool run(const char *source, bool live,
           std::vector<pipeline_prediction> *predictions,
           std::vector<ktag_demo_schema_feature_record> *feature_records,
           pipeline_source_statistics *statistics, std::string *error) {
    *statistics = pipeline_source_statistics{};
    predictions_ = predictions;
    feature_records_ = feature_records;
    live_ = live;
    if ((config_.feature_only && feature_records_ == nullptr &&
         !record_handler_) ||
        (!config_.feature_only && predictions_ == nullptr)) {
      *error = "parallel pipeline output storage is required";
      return false;
    }
    std::string capture_error;
    capture_ =
        create_demo_capture_source(config_, source, live, true, &capture_error);
    domain_store_ = create_domain_store();
    if (!capture_ || !domain_store_ || !create_contexts()) {
      *error =
          error_message_.empty()
              ? (capture_error.empty()
                     ? std::string("parallel pipeline initialization failed")
                     : capture_error)
              : error_message_;
      return false;
    }

    const auto execution_start = std::chrono::steady_clock::now();
    try {
      const uint32_t output_workers = config_.ub.enabled && config_.feature_only
                                          ? 1U : config_.inference_threads;
      for (uint32_t index = 0U; index < output_workers; ++index) {
        if (config_.feature_only) {
          inference_threads_.emplace_back(
              &parallel_source_executor::feature_output_worker, this, index);
        } else {
          inference_threads_.emplace_back(
              &parallel_source_executor::inference_worker, this, index);
        }
      }
      for (uint32_t index = 0U; index < config_.feature_threads; ++index) {
        feature_threads_.emplace_back(&parallel_source_executor::feature_worker,
                                      this, index);
      }
      capture_thread_ =
          std::thread(&parallel_source_executor::capture_worker, this);
    } catch (...) {
      fail("failed to create pipeline threads");
    }

    if (capture_thread_.joinable()) {
      capture_thread_.join();
    } else {
      close_packet_queues();
    }
    for (auto &thread : feature_threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    feature_queue_.close();
    for (auto &thread : inference_threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    const auto execution_end = std::chrono::steady_clock::now();

    if (failed_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(error_mutex_);
      *error = error_message_;
      return false;
    }
    collect_statistics(static_cast<uint64_t>(
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               execution_end - execution_start)
                               .count()),
                       statistics);
    return true;
  }

private:
  bool create_contexts() {
    try {
      feature_contexts_.reserve(config_.feature_threads);
      if (!config_.feature_only) {
        inference_contexts_.reserve(config_.inference_threads);
      }
      for (uint32_t index = 0U; index < config_.feature_threads; ++index) {
        auto context = create_feature_context(config_, schema_, registry_,
                                              domain_store_.get());
        if (!context) {
          error_message_ = "failed to create Feature Context";
          return false;
        }
        feature_contexts_.push_back(std::move(context));
      }
      if (!config_.feature_only) {
        for (uint32_t index = 0U; index < config_.inference_threads; ++index) {
          auto context = create_inference_context(config_, schema_);
          if (!context) {
            error_message_ =
                std::string("failed to create Inference Context: ") +
                ktag_inference_get_last_error();
            return false;
          }
          inference_contexts_.push_back(std::move(context));
        }
      }
      return true;
    } catch (...) {
      error_message_ = "out of memory creating pipeline contexts";
      return false;
    }
  }

  void fail(const std::string &message) {
    bool expected = false;
    if (failed_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(error_mutex_);
      error_message_ = message;
    }
    close_packet_queues();
    feature_queue_.close();
  }

  void close_packet_queues() {
    for (auto &queue : packet_queues_) {
      queue->close();
    }
  }

  void capture_worker() {
    ktag_packet packet{};
    ktag_packet_init(&packet);
    while (!failed_.load(std::memory_order_acquire) &&
           pipeline_interrupted == 0) {
      const auto read_start = std::chrono::steady_clock::now();
      const ktag_status status = capture_->next(&packet);
      capture_stats_.read_nanoseconds += elapsed_nanoseconds(read_start);
      if (status == KTAG_STATUS_TIMEOUT) {
        continue;
      }
      if (status == KTAG_ERROR_CAPACITY) {
        ++capture_stats_.pool_waits;
        std::this_thread::yield();
        continue;
      }
      if (status == KTAG_STATUS_END_OF_STREAM) {
        break;
      }
      if (status != KTAG_SUCCESS) {
        const char *capture_error = capture_->last_error();
        const char *capture_message =
            capture_error == nullptr ? "" : capture_error;
        if (is_recoverable_truncated_capture(
                live_, status, capture_stats_.packets, capture_message)) {
          partial_capture_error_ = capture_message;
          break;
        }
        fail(std::string("capture read failed: ") + capture_message);
        break;
      }

      ++capture_stats_.packets;
      capture_stats_.bytes += packet.captured_length;
      ktag_flow_key key{};
      const ktag_status key_status = ktag_packet_get_flow_key(&packet, &key);
      if (key_status != KTAG_SUCCESS) {
        ++capture_stats_.unsupported;
        ktag_packet_release(&packet);
        continue;
      }
      const uint32_t worker = ktag_flow_select_worker(
          &key, config_.feature_threads, config_.flow_hash_seed);
      if (worker == KTAG_WORKER_INDEX_INVALID ||
          worker >= packet_queues_.size()) {
        ktag_packet_release(&packet);
        fail("dispatcher returned an invalid Feature worker");
        break;
      }

      owned_packet item(&packet);
      const auto result =
          packet_queues_[worker]->push(std::move(item), config_.backpressure);
      if (result == ktag_demo_queue_push_result::failed) {
        fail("packet queue reached capacity with fail policy");
        break;
      }
      if (result == ktag_demo_queue_push_result::closed) {
        break;
      }
    }
    ktag_packet_release(&packet);
    close_packet_queues();
  }

  bool enqueue_feature_batch(ktag_feature_batch *batch) {
    std::vector<feature_record_owned> records;
    std::string error;
    if (!copy_feature_records(batch, source_index_, &records, &error)) {
      fail(error);
      return false;
    }
    for (auto &record : records) {
      const auto result =
          feature_queue_.push(std::move(record), config_.backpressure);
      if (result == ktag_demo_queue_push_result::failed) {
        fail("feature queue reached capacity with fail policy");
        return false;
      }
      if (result == ktag_demo_queue_push_result::closed) {
        if (!failed_.load(std::memory_order_acquire)) {
          fail("feature queue closed before Feature workers drained");
        }
        return false;
      }
    }
    return true;
  }

  void feature_worker(uint32_t worker_index) {
    const auto start = std::chrono::steady_clock::now();
    feature_batch_ptr output(ktag_feature_batch_create(schema_, 64U));
    if (!output) {
      fail("failed to create Feature worker output batch");
      finish_feature_worker(worker_index, start);
      return;
    }

    uint64_t last_expire_us = 0U;
    owned_packet packet;
    while (packet_queues_[worker_index]->pop(&packet)) {
      if (failed_.load(std::memory_order_acquire)) {
        break;
      }
      ++feature_stats_[worker_index].packets;
      const uint64_t now_us = timestamp_us(packet.get()->timestamp);
      const auto process_start = std::chrono::steady_clock::now();
      if (feature_stats_[worker_index].first_process_at ==
          std::chrono::steady_clock::time_point{}) {
        feature_stats_[worker_index].first_process_at = process_start;
      }
      const ktag_status status = ktag_feature_process_packet(
          feature_contexts_[worker_index].get(), packet.get(), output.get());
      feature_stats_[worker_index].work_nanoseconds +=
          elapsed_nanoseconds(process_start);
      packet = owned_packet();
      if (status == KTAG_ERROR_NOT_SUPPORTED ||
          status == KTAG_ERROR_INVALID_ARGUMENT) {
        ++feature_stats_[worker_index].rejected_packets;
        continue;
      }
      if (status != KTAG_SUCCESS || !enqueue_feature_batch(output.get())) {
        if (status != KTAG_SUCCESS) {
          fail("Feature worker processing failed with status " +
               std::to_string(status));
        }
        break;
      }
      feature_stats_[worker_index].emitted_flows +=
          ktag_feature_batch_get_count(output.get());

      const uint64_t interval_us =
          static_cast<uint64_t>(config_.expire_interval_ms) * 1000U;
      if (interval_us != 0U &&
          (last_expire_us == 0U || (now_us >= last_expire_us &&
                                    now_us - last_expire_us >= interval_us))) {
        const auto expire_start = std::chrono::steady_clock::now();
        const ktag_status expire_status = ktag_feature_expire(
            feature_contexts_[worker_index].get(), now_us, output.get());
        feature_stats_[worker_index].work_nanoseconds +=
            elapsed_nanoseconds(expire_start);
        if (expire_status != KTAG_SUCCESS ||
            !enqueue_feature_batch(output.get())) {
          if (expire_status != KTAG_SUCCESS) {
            fail("Feature worker expiration failed");
          }
          break;
        }
        feature_stats_[worker_index].emitted_flows +=
            ktag_feature_batch_get_count(output.get());
        last_expire_us = now_us;
      }
    }

    if (!failed_.load(std::memory_order_acquire)) {
      const auto flush_start = std::chrono::steady_clock::now();
      const ktag_status flush_status = ktag_feature_flush(
          feature_contexts_[worker_index].get(), output.get());
      feature_stats_[worker_index].work_nanoseconds +=
          elapsed_nanoseconds(flush_start);
      if (flush_status != KTAG_SUCCESS ||
          !enqueue_feature_batch(output.get())) {
        if (flush_status != KTAG_SUCCESS) {
          fail("Feature worker flush failed");
        }
      } else {
        feature_stats_[worker_index].emitted_flows +=
            ktag_feature_batch_get_count(output.get());
      }
    }
    finish_feature_worker(worker_index, start);
  }

  void
  finish_feature_worker(uint32_t worker_index,
                        const std::chrono::steady_clock::time_point &start) {
    ktag_feature_statistics feature_statistics{};
    feature_statistics.struct_size = sizeof(feature_statistics);
    feature_statistics.api_version = KTAG_API_VERSION;
    if (ktag_feature_get_statistics(feature_contexts_[worker_index].get(),
                                    &feature_statistics) == KTAG_SUCCESS) {
      feature_stats_[worker_index].rejected_packets =
          std::max(feature_stats_[worker_index].rejected_packets,
                   feature_statistics.rejected_packets);
    }
    feature_stats_[worker_index].elapsed_nanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    if (active_feature_workers_.fetch_sub(1U, std::memory_order_acq_rel) ==
        1U) {
      feature_queue_.close();
    }
  }

  void inference_worker(uint32_t worker_index) {
    const auto start = std::chrono::steady_clock::now();
    feature_batch_ptr input(
        ktag_feature_batch_create(schema_, config_.inference_batch_size));
    prediction_batch_ptr output(
        ktag_prediction_batch_create(config_.inference_batch_size));
    if (!input || !output) {
      fail("failed to create Inference worker batches");
      return;
    }

    feature_record_owned record;
    std::vector<ktag_feature_record_details> retained_details;
    try {
      retained_details.reserve(config_.inference_batch_size);
    } catch (...) {
      fail("out of memory retaining Inference metadata");
      return;
    }
    while (feature_queue_.pop(&record)) {
      if (failed_.load(std::memory_order_acquire)) {
        break;
      }
      ktag_feature_batch_clear(input.get());
      retained_details.clear();
      uint32_t batch_count = 0U;
      while (true) {
        const ktag_status append_status = ktag_feature_batch_append(
            input.get(), &record.details.metadata, record.values.data(),
            static_cast<uint32_t>(record.values.size()));
        if (append_status != KTAG_SUCCESS) {
          fail("failed to build Inference worker batch");
          break;
        }
        try {
          retained_details.push_back(record.details);
        } catch (...) {
          fail("out of memory retaining Inference metadata");
          break;
        }
        ++batch_count;
        if (batch_count >= config_.inference_batch_size ||
            !feature_queue_.try_pop(&record)) {
          break;
        }
      }
      if (failed_.load(std::memory_order_acquire)) {
        break;
      }

      const auto inference_start = std::chrono::steady_clock::now();
      const ktag_status status = ktag_inference_predict(
          inference_contexts_[worker_index].get(), input.get(), output.get());
      inference_stats_[worker_index].work_nanoseconds +=
          elapsed_nanoseconds(inference_start);
      if (status != KTAG_SUCCESS) {
        fail(std::string("Inference worker failed: ") +
             ktag_inference_get_last_error());
        break;
      }
      std::vector<pipeline_prediction> local;
      std::string error;
      if (!append_predictions(output.get(), input.get(), &retained_details,
                              source_index_, &local, &error)) {
        fail(error);
        break;
      }
      {
        std::lock_guard<std::mutex> lock(prediction_mutex_);
        try {
          predictions_->insert(predictions_->end(), local.begin(), local.end());
        } catch (...) {
          fail("out of memory merging Inference results");
          break;
        }
      }
      ++inference_stats_[worker_index].batches;
      inference_stats_[worker_index].input_records += batch_count;
      inference_stats_[worker_index].predictions += local.size();
    }
    inference_stats_[worker_index].elapsed_nanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  }

  void feature_output_worker(uint32_t worker_index) {
    const auto start = std::chrono::steady_clock::now();
    feature_record_owned record;
    while (feature_queue_.pop(&record)) {
      if (failed_.load(std::memory_order_acquire)) {
        break;
      }
      if (record_handler_) {
        std::string error;
        if (!record_handler_(std::move(record), &error)) {
          fail(error.empty() ? "Feature Record handler failed" : error);
          break;
        }
      } else {
        std::lock_guard<std::mutex> lock(prediction_mutex_);
        try {
          feature_records_->push_back(std::move(record));
        } catch (...) {
          fail("out of memory merging Feature records");
          break;
        }
      }
      ++inference_stats_[worker_index].batches;
      ++inference_stats_[worker_index].input_records;
    }
    inference_stats_[worker_index].elapsed_nanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  }

  void collect_statistics(uint64_t source_elapsed_nanoseconds,
                          pipeline_source_statistics *statistics) const {
    statistics->packets = capture_stats_.packets;
    statistics->bytes = capture_stats_.bytes;
    statistics->unsupported_packets = capture_stats_.unsupported;
    statistics->pool_waits = capture_stats_.pool_waits;
    statistics->capture_nanoseconds = capture_stats_.read_nanoseconds;
    statistics->elapsed_nanoseconds = source_elapsed_nanoseconds;

    for (uint32_t index = 0U; index < packet_queues_.size(); ++index) {
      const auto queue = packet_queues_[index]->statistics();
      const auto &worker = feature_stats_[index];
      if (worker.first_process_at != std::chrono::steady_clock::time_point{} &&
          (statistics->first_feature_at ==
               std::chrono::steady_clock::time_point{} ||
           worker.first_process_at < statistics->first_feature_at)) {
        statistics->first_feature_at = worker.first_process_at;
      }
      statistics->unsupported_packets += worker.rejected_packets;
      statistics->emitted_flows += worker.emitted_flows;
      statistics->feature_nanoseconds =
          std::max(statistics->feature_nanoseconds, worker.work_nanoseconds);
      statistics->feature_wall_nanoseconds =
          std::max(statistics->feature_wall_nanoseconds,
                   worker.elapsed_nanoseconds);
      statistics->feature_work_sum_nanoseconds += worker.work_nanoseconds;
      statistics->queue_drops += queue.dropped_newest + queue.dropped_oldest;
      statistics->queue_failures += queue.failures;
    }
    const auto feature_queue = feature_queue_.statistics();
    statistics->queue_drops +=
        feature_queue.dropped_newest + feature_queue.dropped_oldest;
    statistics->queue_failures += feature_queue.failures;
    statistics->partial_capture_error = partial_capture_error_;
    for (uint32_t index = 0U; index < inference_stats_.size(); ++index) {
      const auto &worker = inference_stats_[index];
      statistics->inference_inputs += worker.input_records;
      statistics->predictions += worker.predictions;
      statistics->inference_nanoseconds += worker.work_nanoseconds;
    }
    collect_capture_statistics(capture_.get(), statistics);
  }

  const ktag_demo_pipeline_config &config_;
  const ktag_feature_schema *schema_;
  const ktag_feature_registry *registry_;
  uint32_t source_index_;
  feature_record_handler record_handler_;
  std::unique_ptr<demo_capture_source> capture_;
  domain_store_ptr domain_store_;
  std::vector<feature_context_ptr> feature_contexts_;
  std::vector<inference_context_ptr> inference_contexts_;
  std::vector<std::unique_ptr<ktag_demo_bounded_queue<owned_packet>>>
      packet_queues_;
  ktag_demo_bounded_queue<feature_record_owned> feature_queue_;
  std::atomic<uint32_t> active_feature_workers_;
  std::atomic<bool> failed_{false};
  std::mutex error_mutex_;
  std::string error_message_;
  capture_statistics capture_stats_;
  std::string partial_capture_error_;
  bool live_ = false;
  std::vector<feature_worker_statistics> feature_stats_;
  std::vector<inference_worker_statistics> inference_stats_;
  std::thread capture_thread_;
  std::vector<std::thread> feature_threads_;
  std::vector<std::thread> inference_threads_;
  std::mutex prediction_mutex_;
  std::vector<pipeline_prediction> *predictions_ = nullptr;
  std::vector<ktag_demo_schema_feature_record> *feature_records_ = nullptr;
};

bool run_parallel_source(
    const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
    const ktag_feature_registry *registry, const char *source, bool live,
    uint32_t source_index, std::vector<pipeline_prediction> *predictions,
    std::vector<ktag_demo_schema_feature_record> *feature_records,
    pipeline_source_statistics *statistics, std::string *error,
    feature_record_handler record_handler = {}) {
  parallel_source_executor executor(config, schema, registry, source_index,
                                    std::move(record_handler));
  return executor.run(source, live, predictions, feature_records, statistics,
                      error);
}

#if KTAG_ENABLE_DPDK_UB

void print_ub_throughput(const char *label, uint64_t input_bytes,
                         uint64_t nanoseconds) {
  if (nanoseconds == 0U) {
    std::printf("%s: N/A（无有效计时区间）\n", label);
    return;
  }
  const double seconds = static_cast<double>(nanoseconds) / 1.0e9;
  // Keep the unit conversion used by the existing performance report.
  constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
  constexpr double bits_per_gigabit = 1024.0 * 1024.0 * 1024.0;
  const double bytes = static_cast<double>(input_bytes);
  std::printf("%s: %10.4f MB/s (%10.4f Gbps)\n", label,
              bytes / bytes_per_megabyte / seconds,
              bytes * 8.0 / bits_per_gigabit / seconds);
}

void print_ub_performance_stats(const pipeline_run_statistics &statistics,
                               bool frontend) {
  constexpr double nanoseconds_per_second = 1.0e9;
  constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
  std::printf("\n======================= 性能统计 =======================\n");
  if (frontend) {
    std::printf("PCAP 读取累计耗时: %10.4f 秒\n",
                statistics.capture_nanoseconds / nanoseconds_per_second);
    std::printf("特征计算耗时: %10.4f 秒\n",
                statistics.feature_nanoseconds / nanoseconds_per_second);
    std::printf("特征线程经过耗时: %10.4f 秒\n",
                statistics.feature_wall_nanoseconds / nanoseconds_per_second);
    std::printf("特征 CPU 累计计算量: %10.4f CPU 秒\n",
                statistics.feature_work_sum_nanoseconds /
                    nanoseconds_per_second);
  } else {
    std::printf("推理阶段墙钟耗时（首条特征接收到末次推理完成）: %10.4f 秒\n",
                statistics.distributed_inference_wall_nanoseconds /
                    nanoseconds_per_second);
  }
  std::printf("文件 IO 耗时: %10.4f 秒\n",
              statistics.file_io_nanoseconds / nanoseconds_per_second);
  if (frontend) {
    const double local_compute_seconds =
        (statistics.feature_nanoseconds + statistics.file_io_nanoseconds) /
        nanoseconds_per_second;
    std::printf("核心累计耗时: %10.4f 秒 (本节点计算累计，多线程阶段可并行重叠)\n",
                local_compute_seconds);
    std::printf("核心链路耗时（特征提取、UB 传输、等待及后端推理）: %10.4f 秒\n",
                statistics.distributed_core_nanoseconds /
                    nanoseconds_per_second);
    std::printf("核心计时范围: 首条报文开始特征处理 → 收到后端完成确认\n");
  }
  std::printf("端到端总耗时: %10.4f 秒 (包含 PCAP 读取等所有开销)\n",
              statistics.elapsed_nanoseconds / nanoseconds_per_second);
  std::printf("处理包数: %10llu 个\n",
              static_cast<unsigned long long>(statistics.packets));
  std::printf("总数据量: %10.4f MB\n",
              static_cast<double>(statistics.bytes) / bytes_per_megabyte);
  if (frontend) {
    print_ub_throughput("特征计算吞吐量", statistics.bytes,
                        statistics.feature_nanoseconds);
    print_ub_throughput("特征有效吞吐量", statistics.bytes,
                        statistics.feature_wall_nanoseconds);
    print_ub_throughput("核心处理吞吐量（UB 全链路）", statistics.bytes,
                        statistics.distributed_core_nanoseconds);
  } else {
    print_ub_throughput("推理有效吞吐量", statistics.bytes,
                        statistics.distributed_inference_wall_nanoseconds);
  }
  print_ub_throughput("端到端吞吐量", statistics.bytes,
                      statistics.elapsed_nanoseconds);
  std::printf("========================================================\n\n");
}

uint64_t ub_identity_hash(const std::string &identity) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : identity) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void ub_poll_backoff(uint32_t poll_us) {
  if (poll_us == 0U) {
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(poll_us));
  }
}

ktag_demo_ub_transport_config
make_ub_transport_config(const ktag_demo_pipeline_config &config,
                         ktag_demo_ub_role role) {
  ktag_demo_ub_transport_config transport;
  transport.role = role;
  transport.provider_nodes = config.ub.provider_nodes;
  transport.user_nodes = config.ub.user_nodes;
  transport.region = config.ub.region;
  transport.shm_prefix = config.ub.shm_prefix;
  transport.provider_shm_size =
      static_cast<size_t>(config.ub.provider_shm_size);
  transport.queues_per_provider = config.ub.queues_per_provider;
  transport.queue_depth = config.ub.queue_depth;
  transport.data_element_size = config.ub.data_element_size;
  transport.eal_arguments = config.ub.eal_arguments;
  return transport;
}

bool validate_ub_contract(const ktag_demo_ub_frame_info &info,
                          uint64_t frontend_hash, uint64_t epoch,
                          uint64_t schema_hash, uint64_t ruleset_hash,
                          std::string *error) {
  if (info.contract.frontend_id_hash != frontend_hash) {
    *error = "UB frame arrived on the wrong Frontend queue";
    return false;
  }
  if (epoch != 0U && info.contract.epoch != epoch) {
    *error = "UB frame epoch does not match this Backend run";
    return false;
  }
  if (info.contract.schema_hash != schema_hash) {
    *error = "UB frame Schema hash does not match local configuration";
    return false;
  }
  if (info.contract.ruleset_hash != ruleset_hash) {
    *error = "UB frame ruleset hash does not match local YAML";
    return false;
  }
  return true;
}

class ub_frontend_record_sender {
  struct queue_state {
    ktag_demo_ub_queue_mapping mapping{};
    void *ready = nullptr;
    uint64_t sequence = 0U;
    bool drain_sent = false;
    bool acknowledged = false;
    bool mapped = false;
    bool error_sent = false;
  };
public:
  ~ub_frontend_record_sender() {
    abort_best_effort();
    if (transport_) {
      for (auto &queue : queues_) {
        if (queue.ready) { transport_->return_buffer(queue.ready); }
      }
    }
  }

  bool initialize(const ktag_demo_pipeline_config &config, uint64_t schema_hash,
                  uint32_t value_count, std::string *error) {
    config_ = &config;
    schema_hash_ = schema_hash;
    value_count_ = value_count;
    frontend_hash_ = ub_identity_hash(config.ub.node_id);
    const size_t required = ktag_demo_ub_feature_frame_size(value_count);
    if (required == 0U || required > config.ub.data_element_size) {
      *error = "UB data_element_size cannot hold configured Feature frame (required=" + std::to_string(required) + ")";
      return false;
    }
    transport_ = ktag_demo_ub_ring_transport::create(
        make_ub_transport_config(config, ktag_demo_ub_role::provider), error);
    if (!transport_) { return false; }
    queues_.resize(config.ub.queues_per_provider);
    for (size_t index = 0U; index < queues_.size(); ++index) {
      if (!transport_->queue_mapping(config.ub.node_id, static_cast<uint32_t>(index),
                                     &queues_[index].mapping, error)) { return false; }
      queues_[index].mapped = true;
    }
    const uint32_t expected = ktag_demo_ub_runtime_data_element_size(config.ub.data_element_size);
    uint32_t actual = 0U;
    while ((actual = transport_->data_element_size(queues_.front().mapping.provider_node_index)) == 0U) {
      if (interrupted(error)) { return false; }
      ub_poll_backoff(config.ub.poll_us);
    }
    if (expected == 0U || actual != expected) {
      *error = "Frontend/Backend UB runtime element size mismatch: expected=" +
          std::to_string(expected) + ", actual=" + std::to_string(actual);
      return false;
    }
    // Establish the session before input processing; preserve the first CREDIT.
    while (epoch_ == 0U) {
      if (interrupted(error)) { return false; }
      for (auto &queue : queues_) {
        if (!poll_control(queue, error)) { return false; }
        if (epoch_ != 0U) { break; }
      }
      if (epoch_ == 0U) { ub_poll_backoff(config.ub.poll_us); }
    }
    std::printf("UB Frontend ready: node=%s, queues=%zu, element=%u, epoch=%llu\n",
                config.ub.node_id.c_str(), queues_.size(), actual,
                static_cast<unsigned long long>(epoch_));
    return true;
  }

  bool send(feature_record_owned &&record, bool ground_truth_valid, std::string *error) {
    if (record.values.size() != value_count_) {
      *error = "Frontend Feature value count changed after Schema freeze";
      return false;
    }
    while (!interrupted(error)) {
      for (size_t offset = 0U; offset < queues_.size(); ++offset) {
        const size_t index = (next_queue_ + offset) % queues_.size();
        auto &queue = queues_[index];
        if (!poll_control(queue, error)) { return false; }
        if (!queue.ready) { continue; }
        auto contract = base_contract();
        contract.sequence = queue.sequence + 1U;
        size_t encoded = 0U;
        if (!ktag_demo_ub_encode_feature_frame(queue.ready, config_->ub.data_element_size,
                contract, record, ground_truth_valid, &encoded, error) ||
            !send_ready(queue, error)) { return false; }
        queue.sequence = contract.sequence;
        ++sent_features_;
        next_queue_ = (index + 1U) % queues_.size();
        return true;
      }
      ++receive_polls_;
      ub_poll_backoff(config_->ub.poll_us);
    }
    return false;
  }

  // First signal stops capture; finish queued features and DRAIN. A second
  // signal interrupts UB waits. Startup remains interruptible by one signal.
  void begin_input() { input_started_ = true; }

  bool drain(pipeline_run_statistics *statistics, std::string *error) {
    if (statistics->emitted_flows != sent_features_) {
      *error = "Frontend extracted/sent Feature totals do not match";
      return false;
    }
    size_t completed = 0U;
    while (completed < queues_.size() && !interrupted(error)) {
      bool progress = false;
      for (size_t index = 0U; index < queues_.size(); ++index) {
        auto &queue = queues_[index];
        if (queue.acknowledged) { continue; }
        if (!poll_control(queue, error)) { return false; }
        if (queue.acknowledged) {
          ++completed;
          progress = true;
          if (completed == queues_.size() && statistics->first_feature_at != std::chrono::steady_clock::time_point{}) {
            statistics->distributed_core_nanoseconds = elapsed_nanoseconds(statistics->first_feature_at);
          }
          continue;
        }
        if (!queue.drain_sent && queue.ready) {
          auto contract = base_contract();
          contract.sequence = queue.sequence;
          ktag_demo_ub_stream_statistics totals;
          totals.feature_records = queue.sequence;
          if (index == 0U) {
            totals.input_packets = statistics->packets;
            totals.input_bytes = statistics->bytes;
          }
          size_t encoded = 0U;
          if (!ktag_demo_ub_encode_drain_frame(queue.ready, config_->ub.data_element_size,
                  contract, totals, &encoded, error) || !send_ready(queue, error)) { return false; }
          queue.drain_sent = true;
          progress = true;
        }
      }
      if (!progress) { ++receive_polls_; ub_poll_backoff(config_->ub.poll_us); }
    }
    if (completed != queues_.size()) { return false; }
    std::printf("UB Frontend drained: features=%llu, queues=%zu, send_retries=%llu, empty_polls=%llu\n",
                static_cast<unsigned long long>(sent_features_), queues_.size(),
                static_cast<unsigned long long>(send_retries_), static_cast<unsigned long long>(receive_polls_));
    return true;
  }

  void abort_best_effort() {
    if (!transport_) { return; }
    for (auto &queue : queues_) {
      if (queue.acknowledged || queue.error_sent || !queue.mapped) { continue; }
      try {
        if (!queue.ready) { transport_->receive_burst(queue.mapping.rx_queue_id, &queue.ready, 1U); }
        if (!queue.ready) { continue; }
        auto contract = base_contract();
        contract.sequence = queue.sequence;
        size_t encoded = 0U;
        std::string ignored;
        if (ktag_demo_ub_encode_control_frame(queue.ready, config_->ub.data_element_size,
                ktag_demo_ub_message_type::error, contract, &encoded, &ignored) &&
            transport_->send_burst(queue.mapping.tx_queue_id, &queue.ready, 1U) == 1U) {
          queue.ready = nullptr;
          queue.error_sent = true;
        }
      } catch (...) { /* Best effort; the retained buffer remains owned here. */ }
    }
  }

private:
  bool interrupted(std::string *error) const {
    if (pipeline_interrupted < (input_started_ ? 2 : 1)) { return false; }
    *error = "Frontend UB operation interrupted";
    return true;
  }
  ktag_demo_ub_frame_contract base_contract() const {
    ktag_demo_ub_frame_contract contract;
    contract.frontend_id_hash = frontend_hash_;
    contract.epoch = epoch_;
    contract.schema_hash = schema_hash_;
    contract.ruleset_hash = config_->ub.ruleset_hash;
    return contract;
  }
  bool send_ready(queue_state &queue, std::string *error) {
    while (!interrupted(error)) {
      if (transport_->send_burst(queue.mapping.tx_queue_id, &queue.ready, 1U) == 1U) {
        queue.ready = nullptr;
        return true;
      }
      ++send_retries_;
      ub_poll_backoff(config_->ub.poll_us);
    }
    return false;
  }
  bool poll_control(queue_state &queue, std::string *error) {
    if (queue.ready || queue.acknowledged) { return true; }
    void *buffer = nullptr;
    if (transport_->receive_burst(queue.mapping.rx_queue_id, &buffer, 1U) == 0U) { return true; }
    // Keep ownership in queue.ready until sent back or returned on destruction.
    queue.ready = buffer;
    ktag_demo_ub_frame_info info;
    if (!ktag_demo_ub_inspect_frame(buffer, config_->ub.data_element_size, &info, error)) { return false; }
    if (info.message_type == ktag_demo_ub_message_type::error) {
      *error = "Backend rejected Frontend UB stream on queue " + std::to_string(queue.mapping.rx_queue_id);
      return false;
    }
    if (!validate_ub_contract(info, frontend_hash_, epoch_, schema_hash_, config_->ub.ruleset_hash, error) || info.contract.epoch == 0U) {
      if (error->empty()) { *error = "UB CREDIT epoch must be non-zero"; }
      return false;
    }
    if (info.message_type == ktag_demo_ub_message_type::credit) {
      epoch_ = info.contract.epoch;
      if (!queue.drain_sent) { return true; }
    } else if (info.message_type == ktag_demo_ub_message_type::drain_ack &&
               queue.drain_sent && info.contract.sequence == queue.sequence) {
      queue.acknowledged = true;
    } else {
      *error = "Frontend received unexpected UB control frame on queue " + std::to_string(queue.mapping.rx_queue_id);
      return false;
    }
    transport_->return_buffer(buffer);
    queue.ready = nullptr;
    return true;
  }

  const ktag_demo_pipeline_config *config_ = nullptr;
  std::unique_ptr<ktag_demo_ub_ring_transport> transport_;
  std::vector<queue_state> queues_;
  size_t next_queue_ = 0U;
  uint64_t frontend_hash_ = 0U, epoch_ = 0U, schema_hash_ = 0U;
  uint32_t value_count_ = 0U;
  bool input_started_ = false;
  uint64_t sent_features_ = 0U, send_retries_ = 0U, receive_polls_ = 0U;
};

struct ub_queue_worker {
  std::string hostname;
  uint32_t sub_queue = 0U, cpu = 0U;
  uint64_t frontend_hash = 0U;
  ktag_demo_ub_queue_mapping mapping{};
  inference_context_ptr inference;
  feature_batch_ptr input;
  prediction_batch_ptr output;
  std::vector<uint8_t> frame;
  std::vector<ktag_feature_record_details> details;
  std::vector<uint32_t> source_indices;
  std::vector<uint64_t> frontend_hashes;
  std::vector<uint8_t> ground_truth_valid;
  std::vector<pipeline_prediction> predictions;
  pipeline_run_statistics statistics;
  std::vector<void *> initial_credits;
  void *held = nullptr;
  void *error_buffer = nullptr;
  uint64_t sequence = 0U, batches = 0U, send_retries = 0U, empty_polls = 0U;
  bool acknowledged = false;
  std::chrono::steady_clock::time_point first_received{}, completed_at{}, batch_started{};
};

class ub_backend_executor {
public:
  ~ub_backend_executor() {
    cancel_.store(true, std::memory_order_relaxed);
    gate_.notify_all();
    join_workers();
    if (transport_) {
      abort_best_effort();
      for (auto &worker : workers_) {
        if (worker.held) { transport_->free_buffer(worker.held); }
        if (worker.error_buffer) { transport_->free_buffer(worker.error_buffer); }
        for (void *buffer : worker.initial_credits) { if (buffer) { transport_->free_buffer(buffer); } }
      }
    }
  }

  bool initialize(const ktag_demo_pipeline_config &config,
                  const ktag_feature_schema *schema, std::string *error) {
    config_ = &config;
    schema_ = schema;
    schema_hash_ = ktag_feature_schema_get_hash(schema);
    value_count_ = ktag_feature_schema_get_value_count(schema);
    const size_t feature_size = ktag_demo_ub_feature_frame_size(value_count_);
    frame_size_ = std::max(feature_size, static_cast<size_t>(KTAG_DEMO_UB_DRAIN_FRAME_SIZE));
    if (feature_size == 0U || frame_size_ > config.ub.data_element_size) {
      *error = "UB data_element_size cannot hold configured Feature/DRAIN frame";
      return false;
    }
    std::vector<uint32_t> allowed;
    if (!ktag_demo_ub_allowed_cpus(&allowed, error)) { return false; }
    if (config.ub.backend_worker_cpus.size() != config.inference_threads ||
        config.inference_threads != config.ub.provider_nodes.size() * config.ub.queues_per_provider) {
      *error = "UB worker, CPU and queue counts do not match";
      return false;
    }
    for (size_t index = 0; index < config.ub.backend_worker_cpus.size(); ++index) {
      const auto cpu = config.ub.backend_worker_cpus[index];
      if (std::find(allowed.begin(), allowed.end(), cpu) == allowed.end()) {
        *error = "UB affinity error: worker=" + std::to_string(index) + ", cpu=" + std::to_string(cpu) + " is outside the pre-EAL allowed CPU set";
        return false;
      }
    }
    transport_ = ktag_demo_ub_ring_transport::create(
        make_ub_transport_config(config, ktag_demo_ub_role::user), error);
    if (!transport_) { return false; }
    epoch_ = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) ^
             (static_cast<uint64_t>(getpid()) << 32U);
    if (epoch_ == 0U) { epoch_ = 1U; }
    workers_.reserve(config.inference_threads);
    for (const auto &hostname : config.ub.provider_nodes) {
      for (uint32_t sub = 0; sub < config.ub.queues_per_provider; ++sub) {
        ub_queue_worker worker;
        worker.hostname = hostname;
        worker.frontend_hash = ub_identity_hash(hostname);
        worker.sub_queue = sub;
        worker.cpu = config.ub.backend_worker_cpus[workers_.size()];
        if (!transport_->queue_mapping(hostname, sub, &worker.mapping, error)) { return false; }
        const auto expected = ktag_demo_ub_runtime_data_element_size(config.ub.data_element_size);
        if (expected == 0U || transport_->data_element_size(worker.mapping.provider_node_index) != expected) {
          *error = "Provider/Backend UB runtime element size mismatch for " + hostname;
          return false;
        }
        workers_.push_back(std::move(worker));
      }
    }
    // Allocate on the EAL main lcore, before any queue worker runs. Reserve an
    // extra control buffer per queue so errors never depend on alloc_buf in a std::thread.
    for (auto &worker : workers_) {
      worker.initial_credits.reserve(config.ub.credits_per_queue);
      for (uint32_t n = 0; n <= config.ub.credits_per_queue; ++n) {
        void *buffer = transport_->allocate_buffer(worker.mapping.provider_node_index);
        if (!buffer) {
          *error = "Provider " + worker.hostname + " does not have enough UB buffers for all queue CREDITs and error reserves";
          return false;
        }
        if (n == config.ub.credits_per_queue) { worker.error_buffer = buffer; }
        else { worker.initial_credits.push_back(buffer); }
      }
    }
    return true;
  }

  bool run(std::vector<pipeline_prediction> *predictions,
           pipeline_run_statistics *statistics, std::string *error) {
    try {
      threads_.reserve(workers_.size());
      for (size_t index = 0; index < workers_.size(); ++index) {
        threads_.emplace_back(&ub_backend_executor::worker_main, this, index);
      }
      while (ready_.load(std::memory_order_acquire) != workers_.size() && !cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!cancelled()) {
        for (auto &worker : workers_) {
          for (auto &buffer : worker.initial_credits) {
            if (!send_control(worker, buffer, ktag_demo_ub_message_type::credit, error)) {
              fail(*error);
              break;
            }
          }
          if (cancelled()) { break; }
        }
      }
      {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        started_ = true;
      }
      gate_.notify_all();
      if (!cancelled()) {
        std::printf("UB Backend ready: providers=%zu, queues/provider=%u, inference_workers=%zu, credits/queue=%u, epoch=%llu\n",
                    config_->ub.provider_nodes.size(), config_->ub.queues_per_provider,
                    workers_.size(), config_->ub.credits_per_queue, static_cast<unsigned long long>(epoch_));
        for (size_t index = 0; index < workers_.size(); ++index) {
          const auto &worker = workers_[index];
          std::printf("  UB worker %zu: provider=%s, sub_queue=%u, rx=%u, tx=%u, cpu=%u\n",
                      index, worker.hostname.c_str(), worker.sub_queue,
                      worker.mapping.rx_queue_id, worker.mapping.tx_queue_id, worker.cpu);
        }
      }
      while (done_.load(std::memory_order_acquire) != threads_.size() && !cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (const std::exception &failure) {
      fail(std::string("UB Backend execution failed: ") + failure.what());
    } catch (...) { fail("unknown UB Backend execution failure"); }
    if (pipeline_interrupted != 0) { fail("UB Backend interrupted"); }
    if (cancelled()) { gate_.notify_all(); }
    join_workers();
    if (cancelled()) {
      abort_best_effort();
      *error = error_.empty() ? "UB Backend cancelled" : error_;
      return false;
    }
    try {
      size_t count = predictions->size();
      for (const auto &worker : workers_) { count += worker.predictions.size(); }
      predictions->reserve(count);
      std::chrono::steady_clock::time_point first{}, last{};
      uint64_t features = 0U, batches = 0U;
      for (auto &worker : workers_) {
        if (!worker.acknowledged || worker.statistics.inference_inputs != worker.sequence) {
          *error = "UB Backend completed with missing ACK or inference count mismatch";
          return false;
        }
        const auto limit = std::numeric_limits<uint64_t>::max();
        if (worker.statistics.bytes > limit - statistics->bytes || worker.statistics.packets > limit - statistics->packets) {
          *error = "UB input statistics overflow";
          return false;
        }
        statistics->bytes += worker.statistics.bytes;
        statistics->packets += worker.statistics.packets;
        statistics->inference_inputs += worker.statistics.inference_inputs;
        statistics->predictions += worker.statistics.predictions;
        features += worker.sequence;
        batches += worker.batches;
        if (worker.first_received != std::chrono::steady_clock::time_point{} &&
            (first == std::chrono::steady_clock::time_point{} || worker.first_received < first)) { first = worker.first_received; }
        last = std::max(last, worker.completed_at);
        predictions->insert(predictions->end(), std::make_move_iterator(worker.predictions.begin()),
                            std::make_move_iterator(worker.predictions.end()));
        std::printf("  UB worker drained: provider=%s, sub_queue=%u, cpu=%u, features=%llu, batches=%llu, send_retries=%llu, empty_polls=%llu\n",
                    worker.hostname.c_str(), worker.sub_queue, worker.cpu,
                    static_cast<unsigned long long>(worker.sequence), static_cast<unsigned long long>(worker.batches),
                    static_cast<unsigned long long>(worker.send_retries), static_cast<unsigned long long>(worker.empty_polls));
      }
      statistics->emitted_flows += features;
      if (first != std::chrono::steady_clock::time_point{} && last != std::chrono::steady_clock::time_point{}) {
        statistics->distributed_inference_wall_nanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count());
      }
      std::printf("UB Backend drained: features=%llu, batches=%llu, queues=%zu\n",
                  static_cast<unsigned long long>(features), static_cast<unsigned long long>(batches), workers_.size());
      return true;
    } catch (const std::exception &failure) {
      *error = std::string("failed to merge UB Backend results: ") + failure.what();
      return false;
    }
  }

private:
  bool cancelled() const { return cancel_.load(std::memory_order_relaxed) || pipeline_interrupted != 0; }
  void fail(const std::string &message) {
    {
      std::lock_guard<std::mutex> lock(error_mutex_);
      if (error_.empty()) { error_ = message; }
    }
    cancel_.store(true, std::memory_order_relaxed);
    gate_.notify_all();
  }
  void join_workers() {
    for (auto &thread : threads_) { if (thread.joinable()) { thread.join(); } }
  }
  ktag_demo_ub_frame_contract contract_for(const ub_queue_worker &worker) const {
    ktag_demo_ub_frame_contract contract;
    contract.frontend_id_hash = worker.frontend_hash;
    contract.epoch = epoch_;
    contract.sequence = worker.sequence;
    contract.schema_hash = schema_hash_;
    contract.ruleset_hash = config_->ub.ruleset_hash;
    return contract;
  }
  // Consumes the buffer on success; on failure its caller retains ownership.
  bool send_control(ub_queue_worker &worker, void *&buffer,
                    ktag_demo_ub_message_type type, std::string *error) {
    size_t encoded = 0U;
    if (!ktag_demo_ub_encode_control_frame(buffer, config_->ub.data_element_size,
                                          type, contract_for(worker), &encoded, error)) { return false; }
    while (!cancelled()) {
      if (transport_->send_burst(worker.mapping.tx_queue_id, &buffer, 1U) == 1U) {
        buffer = nullptr;
        return true;
      }
      ++worker.send_retries;
      ub_poll_backoff(config_->ub.poll_us);
    }
    *error = "UB Backend control send cancelled";
    return false;
  }
  bool flush(ub_queue_worker &worker, std::string *error) {
    return infer_batch(worker.inference.get(), worker.input.get(), worker.output.get(),
        &worker.details, &worker.source_indices, &worker.frontend_hashes, &worker.ground_truth_valid,
        &worker.predictions, &worker.statistics, &worker.batches, &worker.completed_at, error);
  }
  bool receive_queue(ub_queue_worker &worker, std::string *error) {
    while (!cancelled()) {
      ktag_demo_ub_copy_receipt receipt;
      if (!transport_->receive_copy(worker.mapping.rx_queue_id, worker.frame.data(),
              worker.frame.size(), worker.frame.size(), &receipt)) {
        if (!worker.details.empty() && elapsed_nanoseconds(worker.batch_started) >=
            static_cast<uint64_t>(config_->inference_batch_timeout_us) * 1000U && !flush(worker, error)) { return false; }
        ++worker.empty_polls;
        ub_poll_backoff(config_->ub.poll_us);
        continue;
      }
      worker.held = receipt.ub_buffer;
      ktag_demo_ub_frame_info info;
      if (!ktag_demo_ub_inspect_frame(worker.frame.data(), worker.frame.size(), &info, error) ||
          !validate_ub_contract(info, worker.frontend_hash, epoch_, schema_hash_, config_->ub.ruleset_hash, error)) { return false; }
      if (info.message_type == ktag_demo_ub_message_type::error) {
        *error = "Frontend aborted UB stream";
        return false;
      }
      if (info.message_type == ktag_demo_ub_message_type::drain) {
        ktag_demo_ub_stream_statistics totals;
        if (info.contract.sequence != worker.sequence ||
            !ktag_demo_ub_decode_drain_frame(worker.frame.data(), worker.frame.size(), &totals, error) ||
            totals.feature_records != worker.sequence ||
            (worker.sub_queue != 0U && (totals.input_packets != 0U || totals.input_bytes != 0U))) {
          if (error->empty()) { *error = "invalid per-queue DRAIN sequence or statistics"; }
          return false;
        }
        worker.statistics.packets = totals.input_packets;
        worker.statistics.bytes = totals.input_bytes;
        if (!flush(worker, error) || worker.statistics.inference_inputs != worker.sequence) {
          if (error->empty()) { *error = "per-queue inference count mismatch"; }
          return false;
        }
        // ACK may let this provider destroy its region immediately. Finish
        // every remaining payload-header access before publishing the ACK.
        transport_->free_buffer(worker.error_buffer);
        worker.error_buffer = nullptr;
        if (!send_control(worker, worker.held, ktag_demo_ub_message_type::drain_ack, error)) { return false; }
        worker.acknowledged = true;
        return true;
      }
      if (info.message_type != ktag_demo_ub_message_type::feature || info.value_count != value_count_ ||
          info.contract.sequence != worker.sequence + 1U) {
        *error = "invalid Feature type, value count or per-queue sequence";
        return false;
      }
      if (worker.first_received == std::chrono::steady_clock::time_point{}) { worker.first_received = receipt.received_at; }
      feature_record_owned record;
      if (!ktag_demo_ub_decode_feature_frame(worker.frame.data(), worker.frame.size(), &info, &record, error)) { return false; }
      if (ktag_feature_batch_append(worker.input.get(), &record.details.metadata, record.values.data(),
                                    static_cast<uint32_t>(record.values.size())) != KTAG_SUCCESS) {
        *error = "failed to append distributed Feature to local Batch";
        return false;
      }
      worker.details.push_back(record.details);
      worker.source_indices.push_back(record.source_index);
      worker.frontend_hashes.push_back(worker.frontend_hash);
      worker.ground_truth_valid.push_back((info.flags & KTAG_DEMO_UB_FRAME_FLAG_GROUND_TRUTH_VALID) != 0U);
      worker.sequence = info.contract.sequence;
      if (worker.details.size() == 1U) { worker.batch_started = std::chrono::steady_clock::now(); }
      if (!send_control(worker, worker.held, ktag_demo_ub_message_type::credit, error)) { return false; }
      if ((worker.details.size() >= config_->inference_batch_size ||
           elapsed_nanoseconds(worker.batch_started) >= static_cast<uint64_t>(config_->inference_batch_timeout_us) * 1000U) &&
          !flush(worker, error)) { return false; }
    }
    *error = "UB queue worker cancelled";
    return false;
  }
  void worker_main(size_t index) {
    auto &worker = workers_[index];
    std::string error;
    try {
      if (!ktag_demo_ub_bind_cpu(worker.cpu, &error)) { throw std::runtime_error(error); }
      worker.inference = create_inference_context(*config_, schema_);
      worker.input.reset(ktag_feature_batch_create(schema_, config_->inference_batch_size));
      worker.output.reset(ktag_prediction_batch_create(config_->inference_batch_size));
      if (!worker.inference || !worker.input || !worker.output) {
        throw std::runtime_error(std::string("failed to initialize local model/Batch: ") + ktag_inference_get_last_error());
      }
      worker.frame.resize(frame_size_);
      worker.details.reserve(config_->inference_batch_size);
      worker.source_indices.reserve(config_->inference_batch_size);
      worker.frontend_hashes.reserve(config_->inference_batch_size);
      worker.ground_truth_valid.reserve(config_->inference_batch_size);
      ready_.fetch_add(1U, std::memory_order_release);
      {
        std::unique_lock<std::mutex> lock(gate_mutex_);
        while (!started_ && !cancelled()) {
          gate_.wait_for(lock, std::chrono::milliseconds(10));
        }
      }
      if (!cancelled() && !receive_queue(worker, &error)) { throw std::runtime_error(error); }
    } catch (const std::exception &failure) {
      fail("UB worker=" + std::to_string(index) + ", provider=" + worker.hostname +
           ", sub_queue=" + std::to_string(worker.sub_queue) + ": " + failure.what());
    } catch (...) { fail("unknown UB queue worker failure: " + std::to_string(index)); }
    done_.fetch_add(1U, std::memory_order_release);
  }
  void abort_best_effort() {
    // All owners have joined, so the main lcore may produce on these queues.
    if (abort_notified_) { return; }
    abort_notified_ = true;
    for (size_t first = 0U; first < workers_.size();) {
      size_t end = first + 1U;
      while (end < workers_.size() && workers_[end].hostname == workers_[first].hostname) { ++end; }
      ub_queue_worker *notification = nullptr;
      for (size_t index = first; index < end; ++index) {
        auto &worker = workers_[index];
        if (!worker.acknowledged && notification == nullptr) {
          if (!worker.error_buffer && worker.held) { std::swap(worker.error_buffer, worker.held); }
          if (!worker.error_buffer && !worker.initial_credits.empty()) {
            std::swap(worker.error_buffer, worker.initial_credits.back());
          }
          if (worker.error_buffer) { notification = &worker; }
        }
        if (worker.held) { transport_->free_buffer(worker.held); worker.held = nullptr; }
        for (auto &buffer : worker.initial_credits) {
          if (buffer) { transport_->free_buffer(buffer); buffer = nullptr; }
        }
        if (worker.error_buffer && notification != &worker) {
          transport_->free_buffer(worker.error_buffer);
          worker.error_buffer = nullptr;
        }
      }
      first = end;
      // One ERROR is sufficient: the frontend polls all its control queues.
      // No allocations here; cleanup also runs after allocation failures.
      if (!notification) { continue; }
      auto &worker = *notification;
      try {
        size_t encoded = 0U;
        std::string ignored;
        if (ktag_demo_ub_encode_control_frame(worker.error_buffer, config_->ub.data_element_size,
                ktag_demo_ub_message_type::error, contract_for(worker), &encoded, &ignored) &&
            encoded != 0U) {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
          do {
            if (transport_->send_burst(worker.mapping.tx_queue_id, &worker.error_buffer, 1U) == 1U) {
              worker.error_buffer = nullptr;
              break;
            }
            ub_poll_backoff(config_->ub.poll_us);
          } while (std::chrono::steady_clock::now() < deadline);
        }
      } catch (...) { /* Preserve local ownership on failure. */ }
    }
  }

  bool infer_batch(ktag_inference_context *inference, ktag_feature_batch *input,
                   ktag_prediction_batch *output,
                   std::vector<ktag_feature_record_details> *details,
                   std::vector<uint32_t> *source_indices,
                   std::vector<uint64_t> *frontend_hashes,
                   std::vector<uint8_t> *ground_truth_valid,
                   std::vector<pipeline_prediction> *predictions,
                   pipeline_run_statistics *statistics, uint64_t *batches,
                   std::chrono::steady_clock::time_point *completed_at,
                   std::string *error) {
    const uint32_t input_count = ktag_feature_batch_get_count(input);
    if (input_count == 0U) {
      return true;
    }
    const ktag_status status = ktag_inference_predict(inference, input, output);
    *completed_at = std::chrono::steady_clock::now();
    if (status != KTAG_SUCCESS) {
      *error = std::string("distributed Backend inference failed: ") +
               ktag_inference_get_last_error();
      return false;
    }
    const size_t prediction_start = predictions->size();
    if (!append_distributed_predictions(output, *details, *source_indices,
                                        *frontend_hashes, *ground_truth_valid,
                                        predictions, error)) {
      return false;
    }
    statistics->inference_inputs += input_count;
    statistics->predictions += predictions->size() - prediction_start;
    ++(*batches);
    ktag_feature_batch_clear(input);
    details->clear();
    source_indices->clear();
    frontend_hashes->clear();
    ground_truth_valid->clear();
    return true;
  }

  const ktag_demo_pipeline_config *config_ = nullptr;
  const ktag_feature_schema *schema_ = nullptr;
  std::unique_ptr<ktag_demo_ub_ring_transport> transport_;
  std::vector<ub_queue_worker> workers_;
  std::vector<std::thread> threads_;
  std::atomic<bool> cancel_{false};
  std::atomic<size_t> ready_{0U}, done_{0U};
  std::mutex gate_mutex_, error_mutex_;
  std::condition_variable gate_;
  bool started_ = false;
  bool abort_notified_ = false;
  std::string error_;
  size_t frame_size_ = 0U;
  uint32_t value_count_ = 0U;
  uint64_t schema_hash_ = 0U, epoch_ = 0U;
};

bool run_ub_frontend_inputs(
    const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
    const ktag_feature_registry *registry, const char **pcap_files,
    int pcap_file_count, const char *interface_name,
    pipeline_run_statistics *statistics,
    std::vector<std::pair<std::string, std::string>> *partial_captures,
    std::string *error) {
  ub_frontend_record_sender sender;
  if (!sender.initialize(config, ktag_feature_schema_get_hash(schema),
                         ktag_feature_schema_get_value_count(schema), error)) {
    sender.abort_best_effort();
    return false;
  }
  sender.begin_input();
  bool success = true;
  if (interface_name != nullptr && interface_name[0] != '\0') {
    std::printf("\n========== UB Frontend 网口采集 =========="
                "\n网口: %s\n",
                interface_name);
    feature_record_handler handler = [&sender](feature_record_owned &&record,
                                               std::string *handler_error) {
      return sender.send(std::move(record), false, handler_error);
    };
    pipeline_source_statistics source_statistics;
    success = run_parallel_source(config, schema, registry, interface_name,
                                  true, 0U, nullptr, nullptr,
                                  &source_statistics, error, handler);
    if (success) {
      add_source_statistics(statistics, source_statistics);
    }
  } else {
    std::printf("\n========== UB Frontend PCAP 特征发送 =========="
                "\n文件数量: %d\n",
                pcap_file_count);
    feature_record_handler handler = [&sender](feature_record_owned &&record,
                                               std::string *handler_error) {
      return sender.send(std::move(record), true, handler_error);
    };
    const auto configured_labels =
        config.ub.ground_truth_labels.find(config.ub.node_id);
    if (configured_labels != config.ub.ground_truth_labels.end() &&
        configured_labels->second.size() !=
            static_cast<size_t>(pcap_file_count)) {
      *error = "ground_truth_labels for " + config.ub.node_id +
               " must contain exactly one label for each -p input";
      sender.abort_best_effort();
      return false;
    }
    for (int index = 0;
         index < pcap_file_count && success && pipeline_interrupted == 0;
         ++index) {
      const uint32_t source_index =
          configured_labels == config.ub.ground_truth_labels.end()
              ? static_cast<uint32_t>(index)
              : configured_labels->second[static_cast<size_t>(index)];
      std::printf("\n处理文件 %d/%d: %s (ground_truth_label=%u)\n", index + 1,
                  pcap_file_count, pcap_files[index], source_index);
      pipeline_source_statistics source_statistics;
      success = run_parallel_source(config, schema, registry, pcap_files[index],
                                    false, source_index, nullptr, nullptr,
                                    &source_statistics, error, handler);
      if (success) {
        add_source_statistics(statistics, source_statistics);
        if (!source_statistics.partial_capture_error.empty()) {
          partial_captures->emplace_back(
              pcap_files[index], source_statistics.partial_capture_error);
        }
      }
    }
  }
  if (!success) {
    sender.abort_best_effort();
    return false;
  }
  return sender.drain(statistics, error);
}

bool run_ub_backend(const ktag_demo_pipeline_config &config,
                    const ktag_feature_schema *schema,
                    std::vector<pipeline_prediction> *predictions,
                    pipeline_run_statistics *statistics, std::string *error) {
  ub_backend_executor executor;
  return executor.initialize(config, schema, error) &&
         executor.run(predictions, statistics, error);
}

#endif /* KTAG_ENABLE_DPDK_UB */

bool same_address(const ktag_ip_address &left, const ktag_ip_address &right) {
  if (left.version != right.version ||
      (left.version != 4U && left.version != 6U)) {
    return false;
  }
  const uint32_t length = left.version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
  return std::memcmp(left.bytes, right.bytes, length) == 0;
}

bool same_flow_key(const ktag_flow_key &left, const ktag_flow_key &right) {
  return same_address(left.address_a, right.address_a) &&
         same_address(left.address_b, right.address_b) &&
         left.port_a == right.port_a && left.port_b == right.port_b &&
         left.protocol == right.protocol;
}

void write_csv_text(std::ostream &output, const std::string &value) {
  output << '"';
  for (char character : value) {
    if (character == '"') {
      output << "\"\"";
    } else {
      output << character;
    }
  }
  output << '"';
}

void write_json_text(std::ostream &output, const std::string &value) {
  output << '"';
  for (unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20U) {
        output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<uint32_t>(character) << std::dec
               << std::setfill(' ');
      } else {
        output << static_cast<char>(character);
      }
      break;
    }
  }
  output << '"';
}

bool validate_prediction_output(const pipeline_prediction &prediction,
                                std::string *error) {
  const auto &details = prediction.details;
  const auto &feature_metadata = details.metadata;
  const auto &inference_metadata = prediction.record.metadata;
  const uint8_t ip_version = details.source_address.version;
  if (details.struct_size < sizeof(details) ||
      details.api_version != KTAG_API_VERSION ||
      feature_metadata.struct_size < sizeof(feature_metadata) ||
      feature_metadata.api_version != KTAG_API_VERSION ||
      prediction.record.struct_size < sizeof(prediction.record) ||
      prediction.record.api_version != KTAG_API_VERSION ||
      inference_metadata.struct_size < sizeof(inference_metadata) ||
      inference_metadata.api_version != KTAG_API_VERSION) {
    *error = "Prediction contains invalid versioned metadata";
    return false;
  }
  if ((ip_version != 4U && ip_version != 6U) ||
      details.destination_address.version != ip_version) {
    *error = "Prediction contains invalid IP metadata";
    return false;
  }
  if (feature_metadata.flow_id != inference_metadata.flow_id ||
      feature_metadata.first_packet_sequence !=
          inference_metadata.first_packet_sequence ||
      feature_metadata.last_packet_sequence !=
          inference_metadata.last_packet_sequence ||
      !same_flow_key(feature_metadata.flow_key, inference_metadata.flow_key)) {
    *error = "Prediction metadata does not match its Feature Record";
    return false;
  }
  const float confidence = prediction.record.confidence;
  if (!std::isfinite(confidence) ||
      (confidence != KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE &&
       (confidence < 0.0F || confidence > 1.0F))) {
    *error = "Prediction confidence must be -1 or within [0, 1]";
    return false;
  }
  if (address_text(details.source_address) == "invalid" ||
      address_text(details.destination_address) == "invalid" ||
      flow_text(feature_metadata.flow_key).empty()) {
    *error = "failed to format Prediction IP metadata";
    return false;
  }
  return true;
}

bool write_prediction_csv(std::ostream &output,
                          const std::vector<pipeline_prediction> &predictions,
                          bool include_frontend, std::string *error) {
  if (include_frontend) {
    output << "frontend_id_hash,";
  }
  output << "source_index,flow_id,ip_version,src_ip,dst_ip,src_port,"
            "dst_port,protocol,flow_key,first_packet_sequence,"
            "last_packet_sequence,label,confidence\n";
  for (const auto &prediction : predictions) {
    if (!validate_prediction_output(prediction, error)) {
      return false;
    }
    const auto &details = prediction.details;
    const auto &metadata = details.metadata;
    if (include_frontend) {
      output << prediction.frontend_id_hash << ',';
    }
    output << prediction.source_index << ',' << metadata.flow_id << ','
           << static_cast<uint32_t>(details.source_address.version) << ',';
    write_csv_text(output, address_text(details.source_address));
    output << ',';
    write_csv_text(output, address_text(details.destination_address));
    output << ',' << details.source_port << ',' << details.destination_port
           << ',' << static_cast<uint32_t>(metadata.flow_key.protocol) << ',';
    write_csv_text(output, flow_text(metadata.flow_key));
    output << ',' << metadata.first_packet_sequence << ','
           << metadata.last_packet_sequence << ',' << prediction.record.label
           << ',' << prediction.record.confidence << '\n';
  }
  return static_cast<bool>(output);
}

bool write_prediction_json(std::ostream &output,
                           const std::vector<pipeline_prediction> &predictions,
                           bool include_frontend, std::string *error) {
  output << "[\n";
  for (size_t index = 0U; index < predictions.size(); ++index) {
    const auto &prediction = predictions[index];
    if (!validate_prediction_output(prediction, error)) {
      return false;
    }
    const auto &details = prediction.details;
    const auto &metadata = details.metadata;
    output << "  {";
    if (include_frontend) {
      output << "\"frontend_id_hash\":" << prediction.frontend_id_hash << ',';
    }
    output << "\"source_index\":" << prediction.source_index
           << ",\"flow_id\":" << metadata.flow_id << ",\"ip_version\":"
           << static_cast<uint32_t>(details.source_address.version)
           << ",\"src_ip\":";
    write_json_text(output, address_text(details.source_address));
    output << ",\"dst_ip\":";
    write_json_text(output, address_text(details.destination_address));
    output << ",\"src_port\":" << details.source_port
           << ",\"dst_port\":" << details.destination_port << ",\"protocol\":"
           << static_cast<uint32_t>(metadata.flow_key.protocol)
           << ",\"flow_key\":";
    write_json_text(output, flow_text(metadata.flow_key));
    output << ",\"first_packet_sequence\":" << metadata.first_packet_sequence
           << ",\"last_packet_sequence\":" << metadata.last_packet_sequence
           << ",\"label\":" << prediction.record.label
           << ",\"confidence\":" << prediction.record.confidence << '}'
           << (index + 1U == predictions.size() ? "\n" : ",\n");
  }
  output << "]\n";
  return static_cast<bool>(output);
}

bool write_predictions_impl(const char *path,
                            ktag_demo_pipeline_output_format format,
                            const std::vector<pipeline_prediction> &predictions,
                            std::string *error, bool include_frontend = false) {
  if (error == nullptr) {
    return false;
  }
  if (path == nullptr || path[0] == '\0') {
    std::ostringstream buffer;
    buffer << std::setprecision(9);
    const bool success =
        format == ktag_demo_pipeline_output_format::csv
            ? write_prediction_csv(buffer, predictions, include_frontend, error)
            : write_prediction_json(buffer, predictions, include_frontend,
                                    error);
    if (success) {
      const std::string text = buffer.str();
      if (std::fwrite(text.data(), 1U, text.size(), stdout) != text.size()) {
        *error = "failed while writing Pipeline output to stdout";
        return false;
      }
    }
    return success;
  }
  std::ofstream output(path);
  if (!output) {
    *error = std::string("failed to open pipeline output: ") + path;
    return false;
  }
  output << std::setprecision(9);
  const bool success =
      format == ktag_demo_pipeline_output_format::csv
          ? write_prediction_csv(output, predictions, include_frontend, error)
          : write_prediction_json(output, predictions, include_frontend, error);
  if (!success && error->empty()) {
    *error = "failed while writing pipeline output";
  }
  return success;
}

} // namespace

bool ktag_demo_write_pipeline_predictions(
    const char *path, ktag_demo_pipeline_output_format format,
    const std::vector<ktag_demo_pipeline_prediction> &predictions,
    std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  return write_predictions_impl(path, format, predictions, error);
}

int ktag_demo_run_pipeline_internal(
    const ktag_demo_pipeline_config &config, const char *config_file,
    const char **pcap_files, int pcap_file_count, const char *interface_name,
    const char *output_file, ktag_demo_pipeline_output_format output_format) {
  pipeline_interrupted = 0;
  signal_scope signals;
  const bool distributed_frontend =
      config.ub.enabled &&
      config.ub.role == ktag_demo_distributed_role::frontend;
  const bool distributed_backend =
      config.ub.enabled &&
      config.ub.role == ktag_demo_distributed_role::backend;
#if !KTAG_ENABLE_DPDK_UB
  if (config.ub.enabled) {
    std::fprintf(stderr,
                 "pipeline: DPDK UB support is disabled in this build\n");
    return -1;
  }
#endif
  std::printf("========== %s ==========\n",
              distributed_frontend
                  ? "UB Frontend 特征发送模式"
                  : (distributed_backend
                         ? "UB Backend 推理模式"
                         : (config.feature_only ? "Schema 特征提取模式"
                                                : "在线推理模式")));
  std::printf("配置文件: %s\n", config_file == nullptr ? "" : config_file);
  std::printf("管线模式: %s\n", ktag_demo_pipeline_mode_name(config.mode));
  std::printf("特征线程数: %u\n", config.feature_threads);
  if (!config.feature_only) {
    std::printf("推理线程数: %u\n", config.inference_threads);
  }
  std::printf("流生命周期: %s\n",
              ktag_demo_feature_lifecycle_mode_name(config.lifecycle_mode));
  std::printf("背压策略: %s\n",
              ktag_demo_backpressure_name(config.backpressure));
  if (config.ub.enabled) {
    std::printf("UB 角色: %s, node_id=%s\n",
                ktag_demo_distributed_role_name(config.ub.role),
                config.ub.node_id.c_str());
  }
  if (!config.feature_only) {
    std::printf("模型包: %s\n", config.model_package_path.c_str());
  }

  ktag_demo_feature_plugin_loader plugin_loader;
  registry_ptr registry(ktag_feature_registry_create());
  if (!registry || ktag_feature_registry_register_legacy_builtins(
                       registry.get()) != KTAG_SUCCESS) {
    std::fprintf(stderr, "pipeline: failed to register built-in Providers\n");
    return -1;
  }

  std::string error;
  for (const auto &plugin : config.plugin_paths) {
    if (!plugin_loader.load(plugin, registry.get(), &error)) {
      std::fprintf(stderr, "pipeline: %s\n", error.c_str());
      registry.reset();
      plugin_loader.unload_all();
      return -1;
    }
  }
  schema_ptr schema = compose_schema(config, registry.get(), &error);
  if (!schema) {
    std::fprintf(stderr, "pipeline: %s\n", error.c_str());
    registry.reset();
    plugin_loader.unload_all();
    return -1;
  }

  if (!validate_frozen_schema_contract(config, schema.get(), &error)) {
    std::fprintf(stderr, "pipeline: %s\n", error.c_str());
    schema.reset();
    registry.reset();
    plugin_loader.unload_all();
    return -1;
  }

  const uint64_t schema_hash = ktag_feature_schema_get_hash(schema.get());
  const uint32_t schema_feature_count =
      ktag_feature_schema_get_feature_count(schema.get());
  const uint32_t schema_value_count =
      ktag_feature_schema_get_value_count(schema.get());
  uint32_t model_input_value_count = 0U;
  if (!config.feature_only) {
    ktag_model_package_validation validation{};
    validation.struct_size = sizeof(validation);
    validation.api_version = KTAG_API_VERSION;
    if (ktag_inference_validate_model_package(config.model_package_path.c_str(),
                                              schema.get(),
                                              &validation) != KTAG_SUCCESS) {
      std::fprintf(stderr, "pipeline: model package validation failed: %s\n",
                   ktag_inference_get_last_error());
      schema.reset();
      registry.reset();
      plugin_loader.unload_all();
      return -1;
    }
    model_input_value_count = validation.input_value_count;
    if (config.schema_name == "ktag_legacy_v1" &&
        (validation.input_feature_count != 50U ||
         model_input_value_count != 351U)) {
      std::fprintf(stderr,
                   "pipeline: ktag_legacy_v1 application model package must "
                   "select the frozen 50 Features/351 values, but manifest "
                   "selects %u/%u\n",
                   validation.input_feature_count, model_input_value_count);
      schema.reset();
      registry.reset();
      plugin_loader.unload_all();
      return -1;
    }
    if (is_frozen_proxy_experiment(config.schema_name) &&
        (validation.input_feature_count != schema_feature_count ||
         model_input_value_count != schema_value_count)) {
      std::fprintf(stderr,
                   "pipeline: %s model package must use all %u Features/%u "
                   "values, but manifest selects %u/%u\n",
                   config.schema_name.c_str(), schema_feature_count,
                   schema_value_count, validation.input_feature_count,
                   model_input_value_count);
      schema.reset();
      registry.reset();
      plugin_loader.unload_all();
      return -1;
    }
  }

  std::printf("Schema: %s v%u (feature_count=%u, value_count=%u, "
              "hash=0x%016llx, 插件=%zu)\n",
              ktag_feature_schema_get_name(schema.get()),
              ktag_feature_schema_get_version(schema.get()),
              schema_feature_count, schema_value_count,
              static_cast<unsigned long long>(schema_hash),
              plugin_loader.size());
  if (!config.feature_only) {
    std::printf("模型输入值数量: %u\n", model_input_value_count);
  }

  std::vector<pipeline_prediction> predictions;
  std::vector<ktag_demo_schema_feature_record> feature_records;
  std::vector<std::pair<std::string, std::string>> partial_captures;
  pipeline_run_statistics run_statistics;
  const auto run_start = std::chrono::steady_clock::now();
  bool success = true;
#if KTAG_ENABLE_DPDK_UB
  if (distributed_frontend) {
    success = run_ub_frontend_inputs(
        config, schema.get(), registry.get(), pcap_files, pcap_file_count,
        interface_name, &run_statistics, &partial_captures, &error);
  } else if (distributed_backend) {
    success = run_ub_backend(config, schema.get(), &predictions,
                             &run_statistics, &error);
  } else
#endif
      if (interface_name != nullptr && interface_name[0] != '\0') {
    std::printf("\n========== 网口实时抓包模式 ==========\n");
    std::printf("网口: %s\n", interface_name);
    std::printf("输出: %s\n",
                output_file == nullptr ? "仅控制台显示" : output_file);
    std::printf("开始实时抓包 (Ctrl+C 退出)...\n\n");
    pipeline_source_statistics source_statistics;
    success =
        config.mode == ktag_demo_pipeline_mode::single_thread
            ? run_single_source(config, schema.get(), registry.get(),
                                interface_name, true, 0U, &predictions,
                                &feature_records, &source_statistics, &error)
            : run_parallel_source(config, schema.get(), registry.get(),
                                  interface_name, true, 0U, &predictions,
                                  &feature_records, &source_statistics, &error);
    if (success) {
      add_source_statistics(&run_statistics, source_statistics);
    }
  } else {
    std::printf("\n========== PCAP文件%s模式 ==========\n",
                config.feature_only ? "特征提取" : "推理");
    std::printf("文件数量: %d\n", pcap_file_count);
    std::printf("管线模式: %s\n", ktag_demo_pipeline_mode_name(config.mode));
    if (output_file != nullptr) {
      std::printf("输出文件: %s (格式: %s)\n", output_file,
                  output_format == ktag_demo_pipeline_output_format::csv
                      ? "CSV"
                      : "JSON");
    }
    for (int index = 0;
         index < pcap_file_count && success && pipeline_interrupted == 0;
         ++index) {
      if (config.feature_only) {
        std::printf("\n处理文件 %d/%d: %s\n", index + 1, pcap_file_count,
                    pcap_files[index]);
      } else {
        std::printf("\n处理文件 %d/%d: %s (Label=%d)\n", index + 1,
                    pcap_file_count, pcap_files[index], index);
      }
      pipeline_source_statistics source_statistics;
      success =
          config.mode == ktag_demo_pipeline_mode::single_thread
              ? run_single_source(config, schema.get(), registry.get(),
                                  pcap_files[index], false,
                                  static_cast<uint32_t>(index), &predictions,
                                  &feature_records, &source_statistics, &error)
              : run_parallel_source(
                    config, schema.get(), registry.get(), pcap_files[index],
                    false, static_cast<uint32_t>(index), &predictions,
                    &feature_records, &source_statistics, &error);
      if (success) {
        add_source_statistics(&run_statistics, source_statistics);
        if (!source_statistics.partial_capture_error.empty()) {
          partial_captures.emplace_back(
              pcap_files[index], source_statistics.partial_capture_error);
        }
      }
    }
  }

  if (success) {
    const auto io_start = std::chrono::steady_clock::now();
    if (distributed_frontend) {
      /* Frontend output is the UB Feature stream; only Backend writes
       * Prediction files. */
    } else if (config.feature_only) {
      std::sort(feature_records.begin(), feature_records.end(),
                ktag_demo_schema_feature_record_less);
      success = output_format == ktag_demo_pipeline_output_format::csv &&
                ktag_demo_write_schema_feature_csv(output_file, schema.get(),
                                                   feature_records, &error);
      if (output_format != ktag_demo_pipeline_output_format::csv) {
        error = "Schema feature extraction currently requires CSV output";
      }
    } else {
      std::sort(predictions.begin(), predictions.end(), prediction_less);
      success = distributed_backend
                    ? write_predictions_impl(output_file, output_format,
                                             predictions, &error, true)
                    : ktag_demo_write_pipeline_predictions(
                          output_file, output_format, predictions, &error);
    }
    run_statistics.file_io_nanoseconds += elapsed_nanoseconds(io_start);
  }
  run_statistics.elapsed_nanoseconds = elapsed_nanoseconds(run_start);
  if (!success) {
    std::fprintf(stderr, "pipeline: %s\n", error.c_str());
  } else {
    for (const auto &partial : partial_captures) {
      std::fprintf(
          stderr,
          "WARNING: PCAP文件截断，已保留此前完整报文并生成部分结果: %s (%s)\n",
          partial.first.c_str(), partial.second.c_str());
    }
#if KTAG_ENABLE_DPDK_UB
    if (distributed_frontend || distributed_backend) {
      print_ub_performance_stats(run_statistics, distributed_frontend);
    } else
#endif
    {
      PerformanceStats performance{};
      constexpr double nanoseconds_per_second = 1000000000.0;
      performance.pcap_read_time =
          run_statistics.capture_nanoseconds / nanoseconds_per_second;
      performance.feature_extraction_time =
          run_statistics.feature_nanoseconds / nanoseconds_per_second;
      performance.inference_time =
          run_statistics.inference_nanoseconds / nanoseconds_per_second;
      performance.file_io_time =
          run_statistics.file_io_nanoseconds / nanoseconds_per_second;
      performance.total_time = performance.feature_extraction_time +
                               performance.inference_time +
                               performance.file_io_time;
      performance.end_to_end_time =
          run_statistics.elapsed_nanoseconds / nanoseconds_per_second;
      performance.total_packets = run_statistics.packets;
      performance.total_bytes = run_statistics.bytes;
      print_performance_stats_ex(
          &performance,
          config.mode == ktag_demo_pipeline_mode::single_thread ? 0 : 1);
    }

    if (distributed_frontend) {
      std::printf(
          "UB 发送特征记录数: %llu\n",
          static_cast<unsigned long long>(run_statistics.emitted_flows));
    } else if (config.feature_only) {
      std::printf("Schema CSV记录数: %llu\n",
                  static_cast<unsigned long long>(feature_records.size()));
    } else {
      const uint64_t kept = run_statistics.predictions;
      const uint64_t filtered = run_statistics.inference_inputs >= kept
                                    ? run_statistics.inference_inputs - kept
                                    : 0U;
      std::printf("过滤统计: 保留 %llu 个样本, 过滤掉 %llu 个样本\n",
                  static_cast<unsigned long long>(kept),
                  static_cast<unsigned long long>(filtered));
    }
    std::printf("\n======================= 管线统计 "
                "=======================\n");
    std::printf("特征提取流数:     %10llu 个\n",
                static_cast<unsigned long long>(run_statistics.emitted_flows));
    if (!config.feature_only) {
      std::printf(
          "推理输入流数:     %10llu 个\n",
          static_cast<unsigned long long>(run_statistics.inference_inputs));
    }
    std::printf(
        "不支持数据包:     %10llu 个\n"
        "Packet Pool等待:%10llu 次\n",
        static_cast<unsigned long long>(run_statistics.unsupported_packets),
        static_cast<unsigned long long>(run_statistics.pool_waits));
    std::printf("队列丢弃:       %10llu 个\n"
                "队列失败:       %10llu 次\n"
                "是否中断:         %10s\n"
                "========================================================\n",
                static_cast<unsigned long long>(run_statistics.queue_drops),
                static_cast<unsigned long long>(run_statistics.queue_failures),
                pipeline_interrupted != 0 ? "是" : "否");
    if (run_statistics.capture_stats_available) {
      std::printf("\n======================= DPDK 收包统计 "
                  "=======================\n");
      std::printf("DPDK 总收包数:    %10llu 个\n",
                  static_cast<unsigned long long>(
                      run_statistics.capture_received));
      std::printf("丢包数(网卡):     %10llu 个\n",
                  static_cast<unsigned long long>(
                      run_statistics.capture_dropped));
      std::printf("送往特征提取包数: %10llu 个\n",
                  static_cast<unsigned long long>(
                      run_statistics.capture_delivered));
      std::printf(
          "缓存未交付:       %10llu 个\n"
          "关闭时丢弃:       %10llu 个\n",
          static_cast<unsigned long long>(run_statistics.capture_pending),
          static_cast<unsigned long long>(run_statistics.capture_discarded));
      if (!run_statistics.capture_queues.empty()) {
        std::printf("--------------------------------------------------------\n");
        std::printf(" 队列    收包数      交付数      缓存\n");
        for (const auto &queue : run_statistics.capture_queues) {
          std::printf("  %2u   %10llu  %10llu  %8llu\n",
                      queue.queue_id,
                      static_cast<unsigned long long>(queue.received),
                      static_cast<unsigned long long>(queue.delivered),
                      static_cast<unsigned long long>(queue.pending));
        }
      }
      std::printf("========================================================\n");
    }

    /* 执行度 = 推理完成率：产生预测的流数 / 进入推理的流数。 */
    if (run_statistics.inference_inputs > 0U) {
      std::printf(
          "执行度(推理完成率):  %7.2f%%  (%llu / %llu)\n",
          100.0 * static_cast<double>(run_statistics.predictions) /
              static_cast<double>(run_statistics.inference_inputs),
          static_cast<unsigned long long>(run_statistics.predictions),
          static_cast<unsigned long long>(run_statistics.inference_inputs));
    } else {
      std::printf("执行度(推理完成率):   N/A (无推理输入)\n");
    }

    if (distributed_backend && !predictions.empty()) {
      std::map<int64_t, uint64_t> predicted_counts;
      for (const auto &prediction : predictions) {
        ++predicted_counts[prediction.record.label];
      }
      std::printf("\n预测类别计数:\n");
      for (const auto &entry : predicted_counts) {
        std::printf("  label=%lld: %llu\n", static_cast<long long>(entry.first),
                    static_cast<unsigned long long>(entry.second));
      }
    }
    /* 分类报告与 pcap 文件输入模式一致：在线无真值标签，label 用
     * source_index（单网口恒为 0），accuracy = 预测为类别 0 的流占比，
     * 用于和 pcap 输入模式做同口径对比。 */
    if (!config.feature_only && !predictions.empty()) {
      std::vector<int> labels;
      std::vector<int> predicted_labels;
      labels.reserve(predictions.size());
      predicted_labels.reserve(predictions.size());
      for (const auto &prediction : predictions) {
        if (!prediction.ground_truth_valid) {
          continue;
        }
        labels.push_back(static_cast<int>(prediction.source_index));
        predicted_labels.push_back(static_cast<int>(prediction.record.label));
      }
      if (labels.empty()) {
        std::printf("无有效 ground truth，不生成 Classification Report。\n");
      } else {
        ClassificationReport *report = calculate_classification_report(
            labels.data(), predicted_labels.data(),
            static_cast<int>(labels.size()));
        if (report != nullptr) {
          (void)print_and_save_classification_report(report, output_file);
          free_classification_report(report);
        }
      }
    }
    if (!distributed_frontend && output_file != nullptr) {
      std::printf("%s已保存到: %s\n",
                  config.feature_only ? "Schema 特征 CSV" : "推理结果",
                  output_file);
    }
  }

  /* Required order: Contexts are local and already gone, then Schema and
   * Registry, finally plugin state and dlclose. */
  schema.reset();
  registry.reset();
  plugin_loader.unload_all();
  return success ? 0 : -1;
}

int ktag_demo_run_pipeline(const ktag_demo_pipeline_config &config,
                           const char *config_file, const char **pcap_files,
                           int pcap_file_count, const char *interface_name,
                           const char *output_file,
                           ktag_demo_pipeline_output_format output_format) {
  try {
    return ktag_demo_run_pipeline_internal(config, config_file, pcap_files,
                                           pcap_file_count, interface_name,
                                           output_file, output_format);
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "pipeline: unhandled runtime failure: %s\n",
                 exception.what());
  } catch (...) {
    std::fprintf(stderr, "pipeline: unknown runtime failure\n");
  }
  return -1;
}
