/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ub_ring_transport.h"

#include "ub_feature_wire.h"

#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ub_ring.h>

namespace {

void set_error(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
}

std::string dpdk_error(const char *operation) {
  const int code = rte_errno;
  return std::string(operation) + " failed: " +
         (code == 0 ? "unknown DPDK error" : rte_strerror(code));
}

bool validate_config(const ktag_demo_ub_transport_config &config,
                     std::string *error) {
  if (config.provider_nodes.empty() || config.user_nodes.size() != 1U) {
    set_error(error,
              "UB transport requires at least one Provider and one User");
    return false;
  }
  if (config.provider_nodes.size() + config.user_nodes.size() > 256U) {
    set_error(error, "UB transport node count exceeds the DPDK limit");
    return false;
  }
  std::unordered_set<std::string> nodes;
  for (const auto &node : config.provider_nodes) {
    if (node.empty() || !nodes.insert(node).second) {
      set_error(error, "UB Provider hostnames must be non-empty and unique");
      return false;
    }
  }
  for (const auto &node : config.user_nodes) {
    if (node.empty() || !nodes.insert(node).second) {
      set_error(error, "UB User hostname must be non-empty and unique");
      return false;
    }
  }
  if (config.region.empty() || config.shm_prefix.empty()) {
    set_error(error, "UB region and shm prefix are required");
    return false;
  }
  if (config.data_element_size < ktag_demo_ub_feature_frame_size(0U)) {
    set_error(error, "UB data element cannot hold a KTAG Feature frame");
    return false;
  }
  if (config.queues_per_provider == 0U ||
      config.queues_per_provider > 512U ||
      config.provider_nodes.size() > 512U / config.queues_per_provider) {
    set_error(error, "UB total queue count exceeds the DPDK limit");
    return false;
  }
  if (config.role == ktag_demo_ub_role::provider &&
      (config.provider_shm_size == 0U ||
       config.queue_depth < 2U ||
       (config.queue_depth & (config.queue_depth - 1U)) != 0U)) {
    set_error(error,
              "UB Provider shm/queue values are invalid or not power-of-two");
    return false;
  }
  return true;
}

std::vector<const char *>
node_pointers(const std::vector<std::string> &nodes) {
  std::vector<const char *> result;
  result.reserve(nodes.size());
  for (const auto &node : nodes) {
    result.push_back(node.c_str());
  }
  return result;
}

bool initialize_eal(const ktag_demo_ub_transport_config &config,
                    std::string *error) {
  std::vector<std::string> arguments = config.eal_arguments;
  if (arguments.empty()) {
    arguments = {"kdadk_demo", "--no-huge", "-l", "0"};
  } else if (arguments.front().empty()) {
    set_error(error, "the first DPDK EAL argument must be a program name");
    return false;
  }
  if (arguments.size() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    set_error(error, "too many DPDK EAL arguments");
    return false;
  }

  std::vector<std::vector<char>> storage;
  std::vector<char *> argv;
  try {
    storage.reserve(arguments.size());
    argv.reserve(arguments.size());
    for (const auto &argument : arguments) {
      storage.emplace_back(argument.begin(), argument.end());
      storage.back().push_back('\0');
    }
    for (auto &argument : storage) {
      argv.push_back(argument.data());
    }
  } catch (...) {
    set_error(error, "out of memory preparing DPDK EAL arguments");
    return false;
  }
  const int argc = static_cast<int>(argv.size());
  argv.push_back(nullptr);
  if (rte_eal_init(argc, argv.data()) < 0) {
    set_error(error, dpdk_error("rte_eal_init"));
    return false;
  }
  return true;
}

} // namespace

uint32_t ktag_demo_ub_runtime_data_element_size(uint32_t payload_size) {
#if defined(UB_RING_BUF_HDR_SIZE)
  /* rte_ub_ring 24.11 aligns (payload + ownership header) to 128 bytes. */
  constexpr uint64_t kAlignment = 128U;
  const uint64_t runtime_size =
      static_cast<uint64_t>(payload_size) + UB_RING_BUF_HDR_SIZE;
  if (runtime_size > std::numeric_limits<uint32_t>::max()) {
    return 0U;
  }
  const uint64_t aligned_size =
      (runtime_size + (kAlignment - 1U)) & ~(kAlignment - 1U);
  if (aligned_size > std::numeric_limits<uint32_t>::max()) {
    return 0U;
  }
  return static_cast<uint32_t>(aligned_size);
#else
  /* DPDK/UB 24.1 and earlier had no ownership header. */
  return payload_size;
#endif
}

struct ktag_demo_ub_ring_transport::implementation {
  ~implementation() {
    if (handle != nullptr) {
      rte_ub_ring_fini(handle);
    }
    if (eal_initialized) {
      rte_eal_cleanup();
    }
  }

  ktag_demo_ub_role role = ktag_demo_ub_role::provider;
  rte_ub_ring_handle *handle = nullptr;
  bool eal_initialized = false;
};

ktag_demo_ub_ring_transport::ktag_demo_ub_ring_transport(
    std::unique_ptr<implementation> state)
    : state_(std::move(state)) {}

ktag_demo_ub_ring_transport::~ktag_demo_ub_ring_transport() = default;

std::unique_ptr<ktag_demo_ub_ring_transport>
ktag_demo_ub_ring_transport::create(
    const ktag_demo_ub_transport_config &config, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!validate_config(config, error)) {
    return nullptr;
  }
  std::unique_ptr<implementation> state(new (std::nothrow) implementation());
  if (!state) {
    set_error(error, "out of memory creating UB transport");
    return nullptr;
  }
  state->role = config.role;
  if (!initialize_eal(config, error)) {
    return nullptr;
  }
  state->eal_initialized = true;

  auto provider_nodes = node_pointers(config.provider_nodes);
  auto user_nodes = node_pointers(config.user_nodes);
  if (config.role == ktag_demo_ub_role::provider) {
    state->handle = rte_ub_ring_provider_init(
        provider_nodes.data(), static_cast<uint32_t>(provider_nodes.size()),
        user_nodes.data(), static_cast<uint32_t>(user_nodes.size()),
        config.region.c_str(), config.shm_prefix.c_str(),
        config.provider_shm_size, config.queues_per_provider,
        config.queue_depth);
  } else {
    state->handle = rte_ub_ring_user_init(
        provider_nodes.data(), static_cast<uint32_t>(provider_nodes.size()),
        user_nodes.data(), static_cast<uint32_t>(user_nodes.size()),
        config.region.c_str(), config.shm_prefix.c_str(),
        config.data_element_size);
  }
  if (state->handle == nullptr) {
    set_error(error, dpdk_error(config.role == ktag_demo_ub_role::provider
                                    ? "rte_ub_ring_provider_init"
                                    : "rte_ub_ring_user_init"));
    return nullptr;
  }

  auto *transport = new (std::nothrow)
      ktag_demo_ub_ring_transport(std::move(state));
  if (transport == nullptr) {
    set_error(error, "out of memory finalizing UB transport");
    return nullptr;
  }
  return std::unique_ptr<ktag_demo_ub_ring_transport>(transport);
}

ktag_demo_ub_role ktag_demo_ub_ring_transport::role() const {
  return state_->role;
}

bool ktag_demo_ub_ring_transport::node_index(uint32_t *index,
                                             std::string *error) const {
  if (index == nullptr) {
    set_error(error, "UB node-index output is required");
    return false;
  }
  if (rte_ub_ring_get_node_index(state_->handle, index) < 0) {
    set_error(error, "rte_ub_ring_get_node_index failed");
    return false;
  }
  return true;
}

bool ktag_demo_ub_ring_transport::queue_mapping(
    const std::string &provider_hostname, uint32_t sub_queue,
    ktag_demo_ub_queue_mapping *mapping, std::string *error) const {
  if (provider_hostname.empty() || mapping == nullptr) {
    set_error(error, "UB Provider hostname and queue output are required");
    return false;
  }
  ktag_demo_ub_queue_mapping result;
  if (rte_ub_ring_get_peer_index(state_->handle, provider_hostname.c_str(),
                                 &result.provider_node_index) < 0) {
    set_error(error, "rte_ub_ring_get_peer_index failed for " +
                         provider_hostname);
    return false;
  }
  if (rte_ub_ring_get_queue_mapping(
          state_->handle, result.provider_node_index, sub_queue,
          &result.tx_queue_id, &result.rx_queue_id) < 0) {
    set_error(error, "rte_ub_ring_get_queue_mapping failed for " +
                         provider_hostname);
    return false;
  }
  *mapping = result;
  return true;
}

uint32_t ktag_demo_ub_ring_transport::total_queues() const {
  return rte_ub_ring_get_total_queues(state_->handle);
}

uint32_t ktag_demo_ub_ring_transport::data_element_size(
    uint32_t provider_node_index) const {
  return rte_ub_ring_get_data_elt_size(state_->handle, provider_node_index);
}

void *ktag_demo_ub_ring_transport::allocate_buffer(
    uint32_t provider_node_index) {
  if (state_->role != ktag_demo_ub_role::user) {
    return nullptr;
  }
  return rte_ub_ring_alloc_buf(state_->handle, provider_node_index);
}

uint16_t ktag_demo_ub_ring_transport::send_burst(uint32_t queue_id,
                                                 void **buffers,
                                                 uint16_t count) {
  return rte_ub_ring_send_burst(state_->handle, queue_id, buffers, count);
}

uint16_t ktag_demo_ub_ring_transport::receive_burst(uint32_t queue_id,
                                                    void **buffers,
                                                    uint16_t count) {
  return rte_ub_ring_recv_burst(state_->handle, queue_id, buffers, count);
}

void ktag_demo_ub_ring_transport::free_buffer(void *buffer) {
  rte_ub_ring_free_buf(state_->handle, buffer);
}

void ktag_demo_ub_ring_transport::return_buffer(void *buffer) {
  if (state_->role == ktag_demo_ub_role::provider) {
    rte_ub_ring_return_buf(state_->handle, buffer);
  }
}

void ktag_demo_ub_ring_transport::collect_returned_buffers() {
  if (state_->role == ktag_demo_ub_role::user) {
    rte_ub_ring_collect(state_->handle);
  }
}
