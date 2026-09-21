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
  KTAG_DPDK_PACKET_OWNED = 1
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

KTAG_API uint32_t ktag_dpdk_capture_get_link_type(const void *context);

/* Stops a running capture; next() then reports END_OF_STREAM. */
KTAG_API void ktag_dpdk_capture_interrupt(void *context);

KTAG_API void ktag_dpdk_capture_close(void *context);

/* Thread-local detail for the most recent DPDK Capture failure. */
KTAG_API const char *ktag_dpdk_capture_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_CAPTURE_DPDK_H */
