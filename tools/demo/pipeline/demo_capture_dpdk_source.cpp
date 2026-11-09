/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifdef KTAG_DEMO_HAVE_DPDK

#include "demo_capture_source.h"

#include <memory>
#include <new>
#include <string>

#include "ktag_capture_dpdk.h"

namespace {

class dpdk_capture_source : public demo_capture_source {
public:
  dpdk_capture_source(void *context, uint32_t rx_queue_count)
      : capture_(context), rx_queue_count_(rx_queue_count == 0U
                                               ? 1U
                                               : rx_queue_count) {}

  ~dpdk_capture_source() override {
    if (capture_ != nullptr) {
      ktag_dpdk_capture_close(capture_);
      capture_ = nullptr;
    }
  }

  ktag_status next(ktag_packet *packet) override {
    return ktag_dpdk_capture_next(capture_, packet);
  }

  bool prepare_readers(uint32_t count, std::string *error) override {
    if (ktag_dpdk_capture_prepare_readers(capture_, count) == KTAG_SUCCESS) { return true; }
    *error = ktag_dpdk_capture_get_last_error();
    return false;
  }
  bool enter_reader(std::string *error) override {
    if (ktag_dpdk_capture_reader_enter(capture_) == KTAG_SUCCESS) { return true; }
    *error = ktag_dpdk_capture_get_last_error();
    return false;
  }
  void leave_reader() override { ktag_dpdk_capture_reader_leave(capture_); }
  ktag_status next_queue(uint32_t queue, ktag_packet *packet) override {
    return ktag_dpdk_capture_next_queue(capture_, queue, packet);
  }

  void interrupt() override {
    if (capture_ != nullptr) {
      ktag_dpdk_capture_interrupt(capture_);
    }
  }

  uint32_t link_type() const override {
    return ktag_dpdk_capture_get_link_type(capture_);
  }

  const char *last_error() const override {
    return ktag_dpdk_capture_get_last_error();
  }

  void get_capture_stats(demo_capture_stats *stats) const override {
    *stats = demo_capture_stats{};
    if (capture_ == nullptr) {
      return;
    }
    ktag_dpdk_capture_stats totals{};
    totals.struct_size = sizeof(totals);
    totals.api_version = KTAG_API_VERSION;
    if (ktag_dpdk_capture_get_stats(capture_, &totals) != KTAG_SUCCESS) {
      return;
    }
    stats->available = true;
    stats->received = totals.received;
    stats->delivered = totals.delivered;
    stats->pending = totals.pending;
    stats->dropped = totals.dropped;
    stats->discarded = totals.discarded;

    stats->queues.reserve(rx_queue_count_);
    for (uint32_t queue = 0U; queue < rx_queue_count_; ++queue) {
      ktag_dpdk_queue_stats queue_stats{};
      queue_stats.struct_size = sizeof(queue_stats);
      queue_stats.api_version = KTAG_API_VERSION;
      if (ktag_dpdk_capture_get_queue_stats(capture_, queue, &queue_stats) !=
          KTAG_SUCCESS) {
        continue;
      }
      demo_capture_queue_stats entry{};
      entry.queue_id = queue_stats.queue_id;
      entry.received = queue_stats.received;
      entry.delivered = queue_stats.delivered;
      entry.pending = queue_stats.pending;
      stats->queues.push_back(entry);
    }
  }

private:
  void *capture_ = nullptr;
  uint32_t rx_queue_count_ = 1U;
};

ktag_dpdk_capture_config
make_dpdk_capture_config(const ktag_demo_pipeline_config &config,
                         const char *source, bool owned) {
  ktag_dpdk_capture_config capture_config{};
  capture_config.struct_size = sizeof(capture_config);
  capture_config.api_version = KTAG_API_VERSION;
  capture_config.device_name = source;
  capture_config.rx_queue_count = config.dpdk_rx_queue_count;
  capture_config.rx_queue_depth = config.dpdk_rx_queue_depth;
  capture_config.mbuf_pool_size = config.dpdk_mbuf_pool_size;
  capture_config.burst_size = config.dpdk_burst_size;
  capture_config.rx_idle_eof_us = config.dpdk_rx_idle_eof_us;
  capture_config.poll_interval_us = config.dpdk_poll_interval_us;
  capture_config.packet_mode =
      owned ? KTAG_DPDK_PACKET_OWNED : KTAG_DPDK_PACKET_BORROWED;
  return capture_config;
}

} // namespace

std::unique_ptr<demo_capture_source>
create_dpdk_demo_capture_source(const ktag_demo_pipeline_config &config,
                                const char *source, bool owned,
                                std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  /* EAL init is idempotent; the first successful call wins. */
  const ktag_status init_status =
      ktag_dpdk_eal_init(config.dpdk_eal_args.c_str());
  if (init_status != KTAG_SUCCESS) {
    if (error != nullptr) {
      *error = std::string("DPDK EAL init failed: ") +
               ktag_dpdk_capture_get_last_error();
    }
    return nullptr;
  }

  const ktag_dpdk_capture_config capture_config =
      make_dpdk_capture_config(config, source, owned);
  void *capture = ktag_dpdk_capture_open(&capture_config);
  if (capture == nullptr) {
    if (error != nullptr) {
      *error = std::string("DPDK capture open failed: ") +
               ktag_dpdk_capture_get_last_error();
    }
    return nullptr;
  }
  return std::unique_ptr<demo_capture_source>(
      new (std::nothrow) dpdk_capture_source(capture,
                                             capture_config.rx_queue_count));
}

#endif /* KTAG_DEMO_HAVE_DPDK */
