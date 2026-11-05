/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_CAPTURE_SOURCE_H
#define KTAG_DEMO_CAPTURE_SOURCE_H

#include <memory>
#include <string>

#include "ktag_capture.h"

#include "pipeline_config.h"

/*
 * Capture backend abstraction used by the pipeline. It hides whether packets
 * come from libpcap (offline file or live interface) or from a DPDK PMD
 * port, and always yields standard ktag_packet objects.
 */
/* Counters for one RX queue; only the DPDK backend fills these in. */
struct demo_capture_queue_stats {
  uint32_t queue_id = 0U;
  uint64_t received = 0U;
  uint64_t delivered = 0U;
  uint64_t pending = 0U;
};

/*
 * Optional capture counters, sampled after the run. available stays false for
 * backends that do not track them (libpcap), so callers can skip the report.
 */
struct demo_capture_stats {
  bool available = false;
  uint64_t received = 0U;
  uint64_t delivered = 0U;
  uint64_t pending = 0U;
  uint64_t dropped = 0U;
  uint64_t discarded = 0U;
  std::vector<demo_capture_queue_stats> queues;
};

struct demo_capture_source {
  virtual ~demo_capture_source() = default;
  virtual ktag_status next(ktag_packet *packet) = 0;
  virtual void interrupt() = 0;
  virtual uint32_t link_type() const = 0;
  virtual const char *last_error() const = 0;
  virtual void get_capture_stats(demo_capture_stats *stats) const {
    *stats = demo_capture_stats{};
  }
};

#ifdef KTAG_DEMO_HAVE_DPDK
std::unique_ptr<demo_capture_source>
create_dpdk_demo_capture_source(const ktag_demo_pipeline_config &config,
                                const char *source, bool owned,
                                std::string *error);
#endif

/*
 * Factory. source is a pcap file path / libpcap interface (pcap backend) or
 * a PCI address / vdev name (DPDK backend). live is only meaningful for the
 * pcap backend. owned selects KTAG_CAPTURE_PACKET_OWNED / BORROWED. On
 * failure returns nullptr and fills *error.
 */
std::unique_ptr<demo_capture_source>
create_demo_capture_source(const ktag_demo_pipeline_config &config,
                           const char *source, bool live, bool owned,
                           std::string *error);

#endif /* KTAG_DEMO_CAPTURE_SOURCE_H */
