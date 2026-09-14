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
struct demo_capture_source {
  virtual ~demo_capture_source() = default;
  virtual ktag_status next(ktag_packet *packet) = 0;
  virtual void interrupt() = 0;
  virtual uint32_t link_type() const = 0;
  virtual const char *last_error() const = 0;
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
