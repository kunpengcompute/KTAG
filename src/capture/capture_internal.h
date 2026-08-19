/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_CAPTURE_INTERNAL_H
#define KTAG_CAPTURE_INTERNAL_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <pcap.h>

#include "ktag_capture.h"

struct ktag_packet_pool;

struct ktag_packet_buffer {
  std::atomic<uint32_t> references{0};
  ktag_packet_pool *pool = nullptr;
  std::vector<uint8_t> bytes;
};

struct ktag_packet_pool {
  std::mutex mutex;
  std::vector<ktag_packet_buffer *> free_buffers;
  uint32_t capacity = 0;
  uint32_t allocated = 0;
  uint32_t outstanding = 0;
  bool closing = false;
};

struct ktag_capture_context {
  pcap_t *handle = nullptr;
  ktag_packet_pool *pool = nullptr;
  uint32_t link_type = 0;
  uint64_t next_sequence = 1;
  ktag_capture_packet_mode packet_mode = KTAG_CAPTURE_PACKET_BORROWED;
};

struct ktag_capture_device_storage {
  std::string name;
  std::string description;
  std::vector<std::string> ipv4_addresses;
  std::vector<std::string> ipv6_addresses;
};

struct ktag_capture_device_list {
  std::vector<ktag_capture_device_storage> devices;
};

void ktag_capture_set_error_internal(const std::string &message);

ktag_packet_pool *ktag_packet_pool_create_internal(uint32_t capacity);

ktag_status ktag_packet_pool_acquire_internal(ktag_packet_pool *pool,
                                              ktag_packet_buffer **buffer);

void ktag_packet_buffer_release_internal(ktag_packet_buffer *buffer);

void ktag_packet_pool_close_internal(ktag_packet_pool *pool);

#endif /* KTAG_CAPTURE_INTERNAL_H */
