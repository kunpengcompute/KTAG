#pragma once
#include "common.h"
#include <pcap/pcap.h>

namespace tcp_bench {
class LiveCapture {
 public:
  LiveCapture(const Config &, const std::string &source_mac, const std::string &destination_mac);
  ~LiveCapture();
  bool next(Packet &, uint64_t sequence);
  void wait();
  uint64_t drops();
 private:
  pcap_t *handle_ = nullptr;
};
} // namespace tcp_bench
