/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_CAPTURE_DPDK_INTERNAL_H
#define KTAG_CAPTURE_DPDK_INTERNAL_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <rte_ethdev.h>
#include <rte_version.h>

#include "ktag_capture_dpdk.h"

/*
 * This backend targets DPDK 24.11 and later. The DPDK/UB transport used by the
 * distributed pipeline requires 24.11 as well, and a process can only link one
 * libdpdk, so both features must agree on the version. Earlier releases are not
 * supported -- fail here rather than misbehave at runtime.
 */
#if RTE_VERSION < RTE_VERSION_NUM(24, 11, 0, 0)
#error "ktag_capture_dpdk requires DPDK 24.11 or later"
#endif

/* Cumulative per-queue counters, sampled by ktag_dpdk_capture_get_stats(). */
struct ktag_dpdk_queue_counters {
    uint64_t received = 0;
    uint64_t delivered = 0;
};

struct ktag_dpdk_context {
  uint16_t port = 0;
  ktag_dpdk_packet_mode mode = KTAG_DPDK_PACKET_BORROWED;
  struct rte_mempool *pool = nullptr;
  uint32_t burst_size = KTAG_DPDK_DEFAULT_BURST_SIZE;
  /* Receive queues configured on the port; always >= 1 once open succeeds. */
  uint16_t rx_queue_count = 1U;
  /* Queue whose cache is being drained. */
  uint16_t active_queue = 0U;
  /* Queue the next refill scan starts from, giving round-robin fairness. */
  uint16_t next_rx_queue = 0U;
  uint64_t next_sequence = 1U;
  uint32_t link_type = KTAG_CAPTURE_LINKTYPE_ETHERNET;
  uint64_t rx_idle_eof_us = 0U;
  uint32_t poll_interval_us = 0U;
  std::atomic<bool> stopped{false};
  std::atomic<uint64_t> last_rx_us{0U};
  /*
   * Per-queue burst cache: packets pulled by one rx_burst, drained one per
   * next() call. Indexed by queue id, so a single-queue port uses [0] only.
   */
  std::vector<std::vector<rte_mbuf *>> pending;
  /* Per-queue cumulative counters, indexed like pending. */
  std::vector<ktag_dpdk_queue_counters> counters;
  uint64_t received_total = 0U;
  uint64_t delivered_total = 0U;
  /* Packets freed by close() while still cached, never delivered. */
  uint64_t discarded_total = 0U;
  /* BORROWED mode: the mbuf backing the most recently returned packet. */
  rte_mbuf *current = nullptr;
};

void ktag_dpdk_set_error_internal(const std::string &message);

uint64_t ktag_dpdk_now_us_internal(void);

#endif /* KTAG_CAPTURE_DPDK_INTERNAL_H */
