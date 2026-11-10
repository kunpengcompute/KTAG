/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_DPDK_RING_HANDOFF_H
#define KTAG_DEMO_DPDK_RING_HANDOFF_H

#include <cstdint>

#include "ktag_capture_dpdk.h"

/*
 * One slot of the capture -> Feature handoff ring.
 *
 * The mbuf is owned by the consumer once it has been dequeued and must be
 * released with ktag_dpdk_mbuf_release(); packet->data points into that mbuf
 * and stays valid until then.
 */
struct demo_ring_item {
  void *mbuf = nullptr;
  ktag_packet packet{};
};

/*
 * Thin wrapper over a DPDK rte_ring. Kept in its own translation unit so the
 * DPDK include path and arch flags apply only here, not to the whole demo.
 *
 * The runner creates one ring per (RX worker, Feature worker) pair, so every
 * ring has exactly one producer and one consumer. create() therefore sets
 * RING_F_SP_ENQ | RING_F_SC_DEQ: no CAS or atomic RMW on the ring cursors,
 * which is what removes the producer-side contention of a shared MP ring.
 */
class dpdk_ring_handoff {
public:
  /* capacity is rounded up to a power of two by rte_ring_create(). */
  static dpdk_ring_handoff *create(const char *name, uint32_t capacity,
                                   int socket_id, bool single_producer);
  ~dpdk_ring_handoff();

  dpdk_ring_handoff(const dpdk_ring_handoff &) = delete;
  dpdk_ring_handoff &operator=(const dpdk_ring_handoff &) = delete;

  /* Returns false when the ring is full. */
  bool enqueue(const demo_ring_item &item);
  /* Returns false when the ring is empty. */
  bool dequeue(demo_ring_item *item);

  uint32_t size() const;
  uint32_t capacity() const;

private:
  dpdk_ring_handoff(void *ring, uint32_t capacity);

  void *ring_ = nullptr;
  uint32_t capacity_ = 0U;
};

#endif /* KTAG_DEMO_DPDK_RING_HANDOFF_H */
