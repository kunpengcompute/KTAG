/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_CAPTURE_DPDK_H
#define KTAG_CAPTURE_DPDK_H

#include <stdint.h>

#include "ktag_capture.h"
#include "ktag_export.h"
#include "ktag_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DPDK poll-mode live capture. Produces the same ktag_packet objects as the
 * libpcap Capture layer, so Feature / Inference consumers are unchanged.
 * This library is optional (KTAG_ENABLE_DPDK) and does not affect the
 * libpcap-backed ktag_capture library.
 *
 * Setup requirements (documented in tools/README.md):
 *   - DPDK 24.11 or later, discoverable via pkg-config "libdpdk";
 *     earlier releases are rejected at compile time and by CMake;
 *   - the NIC bound to vfio-pci (or another DPDK-supported driver);
 *   - hugepages available to the EAL.
 */

#define KTAG_DPDK_DEFAULT_RX_QUEUE_DEPTH 1024U
/* Also the upper bound accepted in ktag_dpdk_capture_config.burst_size. */
#define KTAG_DPDK_DEFAULT_BURST_SIZE 64U
#define KTAG_DPDK_DEFAULT_MBUF_POOL_MULTIPLIER 2U

typedef enum {
  /* data is valid only until the next next() call or capture close. */
  KTAG_DPDK_PACKET_BORROWED = 0,
  /* data is copied into a reference-counted PacketBuffer; mbuf freed now. */
  KTAG_DPDK_PACKET_OWNED = 1,
  /*
   * No copy: the mbuf is handed to the caller together with the packet, and
   * stays valid until ktag_dpdk_mbuf_release(). Use next_mbuf() /
   * next_queue_mbuf(); plain next() rejects this mode.
   */
  KTAG_DPDK_PACKET_MBUF = 2
} ktag_dpdk_packet_mode;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  /* PCI address ("0000:03:00.0") or vdev name ("net_pcap0"); NULL = auto. */
  const char *device_name;
  /*
   * Receive queues to configure. Above 1 the port is set up with RSS and
   * next() drains the queues round-robin; RSS pins each flow to one queue,
   * so packet order is preserved within a flow. 0 = 1.
   */
  uint32_t rx_queue_count;
  uint32_t rx_queue_depth;
  uint32_t mbuf_pool_size;
  /*
   * Packets per rx_burst, 0 = KTAG_DPDK_DEFAULT_BURST_SIZE. Values above
   * that constant are rejected by ktag_dpdk_capture_open().
   */
  uint32_t burst_size;
  /* Reserved; the current implementation does not read it. */
  uint32_t packet_buffer_capacity;
  ktag_dpdk_packet_mode packet_mode;
  /* 0 = disabled; return END_OF_STREAM after this many idle microseconds. */
  uint64_t rx_idle_eof_us;
  /* 0 = busy poll; otherwise delay microseconds on an empty rx_burst. */
  uint32_t poll_interval_us;
} ktag_dpdk_capture_config;

/*
 * One-time DPDK EAL initialization. Idempotent: subsequent calls return
 * KTAG_SUCCESS without re-initializing. args is a space-separated EAL
 * argument string (e.g. "-a 0000:03:00.0 --log-level=lib.eal:7"); a program
 * name is prepended automatically. Must be called on the main thread.
 */
KTAG_API ktag_status ktag_dpdk_eal_init(const char *eal_args);

/*
 * Opens a DPDK port for RX. If EAL has not been initialized yet it is
 * initialized lazily with the EAL args embedded in the process (the first
 * successful init wins). Returns an opaque context or NULL on failure.
 */
KTAG_API void *ktag_dpdk_capture_open(const ktag_dpdk_capture_config *config);

/*
 * Returns KTAG_SUCCESS, KTAG_STATUS_TIMEOUT (empty poll), or
 * KTAG_STATUS_END_OF_STREAM (interrupt or rx_idle_eof_us exceeded). packet
 * must first be initialized with ktag_packet_init(). An owned packet must be
 * released before the same output object is passed to next() again.
 *
 * Not thread-safe: a context is meant to be driven by a single capture thread.
 * ktag_dpdk_capture_interrupt() is the one call safe from another thread.
 */
KTAG_API ktag_status ktag_dpdk_capture_next(void *context, ktag_packet *packet);

/* Configure readers BEFORE starting them: 1 keeps next() round-robin;
 * >1 requires readers == rx_queue_count, OWNED packets and working RSS.
 * Different queues may then call next_queue() concurrently; exactly one
 * reader owns each queue. Never mix next() and next_queue() in one run.
 * Stats/close require all readers to have stopped. */
KTAG_API ktag_status ktag_dpdk_capture_prepare_readers(void *context, uint32_t readers);
KTAG_API ktag_status ktag_dpdk_capture_next_queue(
    void *context, uint32_t queue_id, ktag_packet *packet);
/* Call enter/leave on the reader thread, once per multi-reader lifetime.
 * Registration is incompatible with same-host DPDK secondary processes. */
KTAG_API ktag_status ktag_dpdk_capture_reader_enter(void *context);
KTAG_API void ktag_dpdk_capture_reader_leave(void *context);

/*
 * Per-lcore mbuf cache integration for worker threads.
 *
 * DPDK only uses a mempool's per-lcore cache when the calling thread is a
 * registered EAL thread; for a plain pthread/std::thread rte_lcore_id() is
 * LCORE_ID_ANY and every mbuf get/put goes straight to the pool's shared
 * ring with multi-producer/consumer atomics. Worker threads that touch mbufs
 * (RX readers and any consumer that frees packets) must call thread_enter()
 * once at start and thread_leave() at exit, so the RX refill path draws from
 * its own cache instead.
 *
 * thread_enter() is a no-op for threads that already are EAL lcores.
 * thread_leave() flushes this thread's cached mbufs back to the capture pool
 * (objects left in a cache would be stranded once the lcore id is released)
 * and then unregisters; pass the context those mbufs came from.
 */
KTAG_API ktag_status ktag_dpdk_capture_thread_enter(void);
KTAG_API void ktag_dpdk_capture_thread_leave(void *context);

/*
 * KTAG_DPDK_PACKET_MBUF handoff: no copy is made. packet->data points into the
 * returned mbuf; the caller owns the mbuf and must release it with
 * ktag_dpdk_mbuf_release() once the packet has been consumed. These are the
 * only entry points accepted while the context runs in MBUF mode.
 *
 * next_mbuf() drives all RX queues round-robin (single reader);
 * next_queue_mbuf() reads one queue and is the multi-reader variant.
 */
KTAG_API ktag_status ktag_dpdk_capture_next_mbuf(
    void *context, ktag_packet *packet, void **mbuf);
KTAG_API ktag_status ktag_dpdk_capture_next_queue_mbuf(
    void *context, uint32_t queue_id, ktag_packet *packet, void **mbuf);

/* Accessors for a handed-off mbuf. Release returns it to the mempool. */
KTAG_API const uint8_t *ktag_dpdk_mbuf_data(const void *mbuf);
KTAG_API uint32_t ktag_dpdk_mbuf_length(const void *mbuf);
KTAG_API void ktag_dpdk_mbuf_release(void *mbuf);

KTAG_API uint32_t ktag_dpdk_capture_get_link_type(const void *context);

/* Stops a running capture; next() then reports END_OF_STREAM. */
KTAG_API void ktag_dpdk_capture_interrupt(void *context);

KTAG_API void ktag_dpdk_capture_close(void *context);

/*
 * Cumulative counters for one capture context. Sampled on demand and monotonic
 * until the context is closed:
 *   received  - packets pulled from the PMD, summed over all RX queues;
 *   delivered - packets returned to the caller by next();
 *   pending   - packets still sitting in the per-queue burst caches;
 *   dropped   - packets the device reported as missed or errored (NIC level);
 *   discarded - packets pulled but freed by close() without being delivered.
 */
typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t received;
    uint64_t delivered;
    uint64_t pending;
    uint64_t dropped;
    uint64_t discarded;
} ktag_dpdk_capture_stats;

/* Counters for a single RX queue. */
typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t queue_id;
    uint32_t reserved;
    uint64_t received;
    uint64_t delivered;
    uint64_t pending;
} ktag_dpdk_queue_stats;

KTAG_API ktag_status
ktag_dpdk_capture_get_stats(const void *context, ktag_dpdk_capture_stats *stats);

/* queue_id must be below the rx_queue_count the context was opened with. */
KTAG_API ktag_status ktag_dpdk_capture_get_queue_stats(
    const void *context, uint32_t queue_id, ktag_dpdk_queue_stats *stats);

/* Thread-local detail for the most recent DPDK Capture failure. */
KTAG_API const char *ktag_dpdk_capture_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_CAPTURE_DPDK_H */
