/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "demo_capture_source.h"

#include <memory>
#include <new>
#include <string>

namespace {

ktag_capture_config make_pcap_capture_config(const char *source, bool live,
                                             bool owned,
                                             uint32_t packet_pool_size) {
  ktag_capture_config config{};
  config.struct_size = sizeof(config);
  config.api_version = KTAG_API_VERSION;
  config.source_type =
      live ? KTAG_CAPTURE_SOURCE_LIVE_INTERFACE : KTAG_CAPTURE_SOURCE_PCAP_FILE;
  config.source = source;
  config.packet_mode =
      owned ? KTAG_CAPTURE_PACKET_OWNED : KTAG_CAPTURE_PACKET_BORROWED;
  config.snapshot_length = 65535U;
  config.promiscuous = live ? 1U : 0U;
  config.read_timeout_ms = live ? 10U : 0U;
  config.live_buffer_size = live ? 1024U * 1024U * 1024U : 0U;
  config.packet_buffer_capacity = packet_pool_size;
  return config;
}

class pcap_capture_source : public demo_capture_source {
public:
  explicit pcap_capture_source(ktag_capture_context *context)
      : capture_(context) {}

  ~pcap_capture_source() override {
    if (capture_ != nullptr) {
      ktag_capture_close(capture_);
      capture_ = nullptr;
    }
  }

  ktag_status next(ktag_packet *packet) override {
    return ktag_capture_next(capture_, packet);
  }

  void interrupt() override {
    if (capture_ != nullptr) {
      ktag_capture_interrupt(capture_);
    }
  }

  uint32_t link_type() const override {
    return ktag_capture_get_link_type(capture_);
  }

  const char *last_error() const override {
    return ktag_capture_get_last_error();
  }

private:
  ktag_capture_context *capture_ = nullptr;
};

} // namespace

std::unique_ptr<demo_capture_source>
create_demo_capture_source(const ktag_demo_pipeline_config &config,
                           const char *source, bool live, bool owned,
                           std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (config.capture_backend == ktag_demo_capture_backend::dpdk) {
#ifdef KTAG_DEMO_HAVE_DPDK
    return create_dpdk_demo_capture_source(config, source, owned, error);
#else
    if (error != nullptr) {
      *error = "capture backend 'dpdk' requires a KTAG build with "
               "-DKTAG_ENABLE_DPDK=ON";
    }
    return nullptr;
#endif
  }

  const ktag_capture_config capture_config =
      make_pcap_capture_config(source, live, owned, config.packet_pool_size);
  ktag_capture_context *capture = ktag_capture_open(&capture_config);
  if (capture == nullptr) {
    if (error != nullptr) {
      *error =
          std::string("capture open failed: ") + ktag_capture_get_last_error();
    }
    return nullptr;
  }
  return std::unique_ptr<demo_capture_source>(new (std::nothrow)
                                                  pcap_capture_source(capture));
}
