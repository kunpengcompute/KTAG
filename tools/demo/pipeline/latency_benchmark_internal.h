/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#ifndef KTAG_DEMO_LATENCY_BENCHMARK_INTERNAL_H
#define KTAG_DEMO_LATENCY_BENCHMARK_INTERNAL_H
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "ktag_feature.h"
#include "ktag_inference.h"

namespace ktag_demo_latency {
using clock = std::chrono::steady_clock;
inline uint64_t now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      clock::now().time_since_epoch()).count());
}
inline std::string key_text(const ktag_flow_key &key) {
  // Logical fields only: never hash/compare ABI structure padding.
  std::string result;
  result.append(reinterpret_cast<const char *>(key.address_a.bytes), 16);
  result.append(reinterpret_cast<const char *>(key.address_b.bytes), 16);
  result.push_back(static_cast<char>(key.address_a.version));
  result.push_back(static_cast<char>(key.address_b.version));
  result.push_back(static_cast<char>(key.port_a >> 8));
  result.push_back(static_cast<char>(key.port_a));
  result.push_back(static_cast<char>(key.port_b >> 8));
  result.push_back(static_cast<char>(key.port_b));
  result.push_back(static_cast<char>(key.protocol));
  return result;
}
struct packet_stamp { uint64_t sequence = 0U, arrival_ns = 0U; };
class packet_history {
  std::map<std::string, std::deque<packet_stamp>> packets_;
public:
  void add(const std::string &key, packet_stamp packet) {
    auto &items = packets_[key];
    if (!items.empty() && items.back().sequence >= packet.sequence)
      throw std::runtime_error("non-increasing packet sequence in latency capture");
    items.push_back(packet);
  }
  std::vector<packet_stamp> take(const ktag_feature_record_metadata &metadata) {
    std::vector<packet_stamp> result;
    auto found = packets_.find(key_text(metadata.flow_key));
    if (found == packets_.end()) return result;
    auto &items = found->second;
    // A reused five-tuple is a different flow generation. Retired/rejected
    // packets before this record's first sequence must not be reassigned.
    while (!items.empty() && items.front().sequence < metadata.first_packet_sequence)
      items.pop_front();
    while (!items.empty() && items.front().sequence <= metadata.last_packet_sequence) {
      result.push_back(items.front()); items.pop_front();
    }
    if (items.empty()) packets_.erase(found);
    return result;
  }
};
struct distribution_result {
  uint64_t count = 0U;
  double mean = 0, p50 = 0, p95 = 0, p99 = 0, maximum = 0;
};
class distribution {
  std::vector<std::pair<double, uint64_t>> values_;
public:
  void add(double nanoseconds, uint64_t weight = 1U) {
    if (weight) values_.emplace_back(nanoseconds, weight);
  }
  distribution_result summarize() {
    distribution_result r;
    long double sum = 0;
    for (const auto &v : values_) { r.count += v.second; sum += v.first * v.second; }
    if (!r.count) return r;
    std::sort(values_.begin(), values_.end());
    r.mean = static_cast<double>(sum / r.count);
    r.maximum = values_.back().first;
    auto percentile = [&](double fraction) {
      const uint64_t rank = static_cast<uint64_t>(std::ceil(fraction * r.count));
      uint64_t seen = 0;
      for (const auto &v : values_) { seen += v.second; if (seen >= rank) return v.first; }
      return r.maximum;
    };
    r.p50 = percentile(.5); r.p95 = percentile(.95); r.p99 = percentile(.99);
    return r;
  }
};
struct flow {
  uint32_t source = 0U, feature_worker = 0U;
  ktag_feature_record_details details{};
  std::vector<float> values;
  std::vector<packet_stamp> packets;
  uint64_t packet_count = 0U, ready_ns = 0U;
  bool predicted = false, eof_feature = false;
  int64_t label = 0;
  float confidence = KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE;
};
struct batch_result {
  uint32_t source = 0U, worker = 0U;
  uint64_t batch = 0U, start_ns = 0U, finish_ns = 0U, packet_count = 0U;
  ktag_inference_timing timing{};
  bool first_call = false;
  std::string reason;
  std::vector<flow> flows;
};
struct source_result {
  uint64_t captured = 0U, bytes = 0U, emitted = 0U, matched_packets = 0U;
  uint64_t start_ns = 0U, eof_ns = 0U, last_packet_ns = 0U, finish_ns = 0U;
};
} // namespace ktag_demo_latency
#endif
