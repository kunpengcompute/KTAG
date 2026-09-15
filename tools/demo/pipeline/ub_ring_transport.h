/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_UB_RING_TRANSPORT_H
#define KTAG_DEMO_UB_RING_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class ktag_demo_ub_role { provider, user };

struct ktag_demo_ub_transport_config {
  ktag_demo_ub_role role = ktag_demo_ub_role::provider;
  std::vector<std::string> provider_nodes;
  std::vector<std::string> user_nodes;
  std::string region;
  std::string shm_prefix;
  size_t provider_shm_size = 0U;
  uint32_t queues_per_provider = 1U;
  uint32_t queue_depth = 1024U;
  /* Application-visible payload capacity; UB 24.11 adds its own header. */
  uint32_t data_element_size = 4096U;
  std::vector<std::string> eal_arguments;
};

struct ktag_demo_ub_queue_mapping {
  uint32_t provider_node_index = 0U;
  uint32_t tx_queue_id = 0U;
  uint32_t rx_queue_id = 0U;
};

/*
 * Convert the application payload capacity from YAML to the element stride
 * reported by the installed rte_ub_ring implementation.  DPDK/UB 24.11
 * stores a 128-byte ownership header in front of every app-visible buffer;
 * older releases reported the payload size directly.  Keeping this helper in
 * the KTAG adapter lets the YAML contract remain stable across both layouts.
 */
uint32_t ktag_demo_ub_runtime_data_element_size(uint32_t payload_size);

/*
 * Thin RAII wrapper over the installed DPDK rte_ub_ring public API. It owns
 * one EAL instance and one UB Ring handle but never calls ubs-mem directly.
 */
class ktag_demo_ub_ring_transport {
public:
  static std::unique_ptr<ktag_demo_ub_ring_transport>
  create(const ktag_demo_ub_transport_config &config, std::string *error);

  ~ktag_demo_ub_ring_transport();

  ktag_demo_ub_ring_transport(const ktag_demo_ub_ring_transport &) = delete;
  ktag_demo_ub_ring_transport &
  operator=(const ktag_demo_ub_ring_transport &) = delete;

  ktag_demo_ub_role role() const;
  bool node_index(uint32_t *index, std::string *error) const;
  bool queue_mapping(const std::string &provider_hostname,
                     uint32_t sub_queue,
                     ktag_demo_ub_queue_mapping *mapping,
                     std::string *error) const;
  uint32_t total_queues() const;
  uint32_t data_element_size(uint32_t provider_node_index) const;

  /* User-only allocation from a Provider-owned data area. */
  void *allocate_buffer(uint32_t provider_node_index);

  uint16_t send_burst(uint32_t queue_id, void **buffers, uint16_t count);
  uint16_t receive_burst(uint32_t queue_id, void **buffers, uint16_t count);

  /* User returns locally; Provider returns through the shared return ring. */
  void free_buffer(void *buffer);
  void return_buffer(void *buffer);
  void collect_returned_buffers();

private:
  struct implementation;

  explicit ktag_demo_ub_ring_transport(
      std::unique_ptr<implementation> state);

  std::unique_ptr<implementation> state_;
};

#endif /* KTAG_DEMO_UB_RING_TRANSPORT_H */
