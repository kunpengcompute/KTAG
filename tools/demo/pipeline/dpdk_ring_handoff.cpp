/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "dpdk_ring_handoff.h"

#include <new>

/*
 * rte_ring.h pulls rte_ring_elem.h in before its own extern "C" block, so from
 * C++ the *_elem API would get C++ linkage and fail to link. Wrap the DPDK
 * includes to force C linkage.
 */
extern "C" {
#include <rte_errno.h>
#include <rte_memzone.h>
#include <rte_ring.h>
#include <rte_ring_elem.h>
}

namespace {

uint32_t round_up_pow2(uint32_t value) {
  if (value < 2U) {
    return 2U;
  }
  --value;
  value |= value >> 1U;
  value |= value >> 2U;
  value |= value >> 4U;
  value |= value >> 8U;
  value |= value >> 16U;
  return value + 1U;
}

} // namespace

dpdk_ring_handoff::dpdk_ring_handoff(void *ring, uint32_t capacity)
    : ring_(ring), capacity_(capacity)
{
}

dpdk_ring_handoff *dpdk_ring_handoff::create(const char *name,
                                             uint32_t capacity,
                                             int socket_id,
                                             bool single_producer)
{
  const uint32_t count = round_up_pow2(capacity);
  const int socket = socket_id < 0 ? rte_socket_id() : socket_id;
  unsigned int flags = RING_F_SC_DEQ | RING_F_EXACT_SZ;
  if (single_producer) {
    flags |= RING_F_SP_ENQ;
  }
  struct rte_ring *ring = rte_ring_create_elem(
      name, sizeof(demo_ring_item), count, socket, flags);
  if (ring == nullptr) {
    return nullptr;
  }
  return new (std::nothrow) dpdk_ring_handoff(ring, count);
}

dpdk_ring_handoff::~dpdk_ring_handoff()
{
  if (ring_ != nullptr) {
    rte_ring_free(static_cast<struct rte_ring *>(ring_));
    ring_ = nullptr;
  }
}

bool dpdk_ring_handoff::enqueue(const demo_ring_item &item)
{
  if (ring_ == nullptr) {
    return false;
  }
  return rte_ring_enqueue_elem(static_cast<struct rte_ring *>(ring_),
                               const_cast<demo_ring_item *>(&item),
                               sizeof(demo_ring_item)) == 0;
}

bool dpdk_ring_handoff::dequeue(demo_ring_item *item)
{
  if (ring_ == nullptr || item == nullptr) {
    return false;
  }
  return rte_ring_dequeue_elem(static_cast<struct rte_ring *>(ring_), item,
                               sizeof(demo_ring_item)) == 0;
}

uint32_t dpdk_ring_handoff::size() const
{
  if (ring_ == nullptr) {
    return 0U;
  }
  return rte_ring_count(static_cast<const struct rte_ring *>(ring_));
}

uint32_t dpdk_ring_handoff::capacity() const
{
  return capacity_;
}
