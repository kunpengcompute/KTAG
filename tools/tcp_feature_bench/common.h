#pragma once

#include <atomic>
#include <algorithm>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "ktag_feature.h"
#include "ktag_inference.h"
#include "ub_feature_wire.h"

namespace tcp_bench {
using Bytes = std::vector<uint8_t>;
using Record = ktag_demo_schema_feature_record;
using Clock = std::chrono::steady_clock;
inline uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now().time_since_epoch()).count();
}
inline void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
template<class T, void (*Destroy)(T *)>
using Handle = std::unique_ptr<T, decltype(Destroy)>;
using Schema = Handle<ktag_feature_schema, ktag_feature_schema_destroy>;
using Registry = Handle<ktag_feature_registry, ktag_feature_registry_destroy>;
using Feature = Handle<ktag_feature_context, ktag_feature_destroy>;
using FeatureBatch = Handle<ktag_feature_batch, ktag_feature_batch_destroy>;
using Domain = Handle<ktag_domain_store, ktag_domain_store_destroy>;
using Inference = Handle<ktag_inference_context, ktag_inference_destroy>;
using Predictions = Handle<ktag_prediction_batch, ktag_prediction_batch_destroy>;

struct Config {
  std::string role, config_path, local_ip, peer_ip;
  std::string capture_interface, replay_interface, model, result, stats, audit;
  std::string replay_program = "tcpreplay";
  std::string source_mac = "02:00:00:90:00:01";
  std::string destination_mac = "02:00:00:90:00:02";
  std::string rate_mode = "original";
  std::string replay_cpus;
  std::vector<std::string> pcaps;
  uint32_t port = 19090, feature_threads = 4, flow_hash_seed = 2026;
  uint32_t packet_queue = 65536, feature_queue = 4096, flow_capacity = 65536;
  uint64_t tcp_idle_us = 300000000, udp_idle_us = 30000000;
  uint32_t udp_packet_limit = 100, expire_ms = 1000;
  uint32_t compression_records = 256, compression_timeout_us = 1000;
  uint32_t inference_records = 256, inference_timeout_us = 1000;
  uint32_t max_body = 4194304, timeout_s = 120, drain_timeout_s = 30;
  uint32_t quiet_ms = 100, snapshot_length = 65535, capture_buffer = 67108864;
  int level = 1;
  double mbps = 0, target_ratio = 10;
  bool modern = false, preload = false, prepare_only = false;
  uint64_t rules_hash() const;
};
Config parse_config(int argc, char **argv);
void usage();

struct Metrics {
  uint64_t started = now_ns(), process_cpu_started = 0;
  std::map<std::string, uint64_t> nic_begin;
  std::string actual_rate_mode, replay_affinity;
  double actual_rate_mbps = 0;
  uint64_t schema_hash = 0;
  std::atomic<uint64_t> first_feature{0}, completed{0};
  std::atomic<uint64_t> expected_packets{0}, sent_packets{0}, packets{0}, bytes{0};
  std::atomic<uint64_t> unsupported{0}, processed{0}, rejected{0}, extracted{0};
  std::atomic<uint64_t> records{0}, submitted{0}, filtered{0}, predictions{0};
  std::atomic<uint64_t> batches{0}, raw_bytes{0}, compressed_bytes{0};
  std::atomic<uint64_t> socket_tx{0}, socket_rx{0}, capture_drops{0};
  std::atomic<uint64_t> feature_ns{0}, encode_ns{0}, compress_ns{0}, send_ns{0};
  std::atomic<uint64_t> decompress_ns{0}, decode_ns{0}, inference_ns{0};
  std::atomic<uint64_t> queue_wait_ns{0}, capture_ns{0}, replay_ns{0}, replay_bytes{0};
  std::atomic<uint64_t> replay_cpu_ns{0}, first_capture{0}, last_capture{0};
  std::atomic<uint64_t> output_ns{0}, initialization_ns{0}, inference_completed{0};
  std::atomic<uint64_t> inference_cpu_ns{0}, feature_cpu_ns{0}, capture_cpu_ns{0}, output_cpu_ns{0};
  std::atomic<uint64_t> backend_data_cpu_ns{0}, replay_coord_cpu_ns{0};
  void mark_first(uint64_t value);
  void write(const Config &config, bool success, const std::string &error) const;
};
uint64_t process_cpu_ns();
uint64_t thread_cpu_ns();
std::map<std::string, uint64_t> nic_snapshot(const std::string &iface);
std::string json_quote(const std::string &value);

// Closing wakes both producers and consumers. Queued values remain owned
// until consumed or destroyed, including during cancellation.
template<class T> class Queue {
 public:
  explicit Queue(size_t capacity) : capacity_(capacity) {}
  bool push(T value, std::atomic<uint64_t> &wait_ns) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto start = now_ns();
    space_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
    wait_ns += now_ns() - start;
    if (closed_) return false;
    items_.push_back(std::move(value)); available_.notify_one(); return true;
  }
  bool pop(T &value) {
    std::unique_lock<std::mutex> lock(mutex_);
    available_.wait(lock, [&] { return closed_ || !items_.empty(); });
    return take(value);
  }
  bool pop_until(T &value, Clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    available_.wait_until(lock, deadline, [&] { return closed_ || !items_.empty(); });
    return take(value);
  }
  bool drained() {
    std::lock_guard<std::mutex> lock(mutex_); return closed_ && items_.empty();
  }
  void close() {
    std::lock_guard<std::mutex> lock(mutex_); closed_ = true;
    available_.notify_all(); space_.notify_all();
  }
 private:
  bool take(T &value) {
    if (items_.empty()) return false;
    value = std::move(items_.front()); items_.pop_front(); space_.notify_one();
    return true;
  }
  size_t capacity_;
  bool closed_ = false;
  std::mutex mutex_;
  std::condition_variable available_, space_;
  std::deque<T> items_;
};

struct Packet {
  ktag_packet value{};
  Packet() { ktag_packet_init(&value); }
  ~Packet() { ktag_packet_release(&value); }
  Packet(const Packet &) = delete;
  Packet &operator=(const Packet &) = delete;
  Packet(Packet &&other) noexcept : value(other.value) { ktag_packet_init(&other.value); }
  Packet &operator=(Packet &&other) noexcept {
    if (this != &other) { ktag_packet_release(&value); value = other.value;
      ktag_packet_init(&other.value); }
    return *this;
  }
};
Schema make_schema();
void run_frontend(const Config &, Metrics &);
void run_backend(const Config &, Metrics &);
} // namespace tcp_bench
