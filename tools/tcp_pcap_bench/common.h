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
#include "ktag_feature_plugin.h"
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
using FeaturePlugins = std::vector<Handle<ktag_loaded_feature_plugin, ktag_feature_plugin_unload>>;

struct Config {
  std::string role, config_path, local_ip, peer_ip, network_interface;
  std::string model, result, stats, audit;
  std::vector<std::string> pcaps;
  std::vector<std::string> plugin_paths;
  uint32_t port = 19090, feature_threads = 4;
  uint64_t flow_hash_seed = 2026;
  uint32_t packet_pool = 65536, packet_queue = 65536, feature_queue = 4096;
  uint32_t flow_capacity = 0, udp_packet_limit = 0, expire_ms = 1000;
  uint64_t tcp_idle_us = 0, udp_idle_us = 0;
  uint32_t compression_records = 256, compression_timeout_us = 1000;
  uint32_t inference_records = 256, inference_timeout_us = 1000;
  uint32_t max_body = 4194304, timeout_s = 120, prediction_limit = 1000000;
  int level = 1;
  double target_ratio = 10;
  bool modern = false;
  bool online = false;
  std::string device, eal_args;
  uint32_t rx_threads = 1, rx_queues = 1, rx_depth = 1024, mbuf_pool = 0, burst = 64;
  uint32_t rx_poll_us = 0, duration_s = 60, sender_threads = 1, inference_threads = 1;
  uint64_t rx_idle_eof_us = 0;
  std::vector<uint32_t> rx_cpus, feature_cpus, sender_cpus, inference_cpus;
  uint64_t rules_hash() const;
};
Config parse_config(int argc, char **argv);
// Keep these handles alive until all Feature Contexts and the Registry are gone.
void load_feature_plugins(const Config &, ktag_feature_registry *, FeaturePlugins &);
void usage();

struct SourceStats {
  uint32_t index = 0, link_type = 0;
  std::string name, warning;
  uint64_t packets = 0, bytes = 0, original_bytes = 0, short_captures = 0;
  uint64_t unsupported = 0, processed = 0, rejected = 0, records = 0, pool_waits = 0;
  uint64_t submitted = 0, filtered = 0, predictions = 0, elapsed_ns = 0;
};
struct Metrics {
  uint64_t started = now_ns(), process_cpu_started = 0, schema_hash = 0;
  std::string nic_interface;
  std::map<std::string, uint64_t> nic_begin;
  std::vector<SourceStats> sources;
  std::atomic<uint64_t> first_feature{0}, first_read{0}, completed{0}, inference_completed{0};
  std::atomic<uint64_t> packets{0}, bytes{0}, original_bytes{0}, short_captures{0};
  std::atomic<uint64_t> unsupported{0}, processed{0}, rejected{0}, extracted{0};
  std::atomic<uint64_t> records{0}, submitted{0}, filtered{0}, predictions{0};
  std::atomic<uint64_t> batches{0}, raw_bytes{0}, compressed_bytes{0}, socket_tx{0}, socket_rx{0};
  std::atomic<uint64_t> read_ns{0}, pool_waits{0}, pool_wait_ns{0}, queue_wait_ns{0};
  std::atomic<uint64_t> feature_ns{0}, encode_ns{0}, compress_ns{0}, send_ns{0};
  std::atomic<uint64_t> decompress_ns{0}, decode_ns{0}, inference_ns{0};
  std::atomic<uint64_t> output_ns{0}, initialization_ns{0}, inference_cpu_ns{0};
  std::atomic<uint64_t> capture_received{0}, capture_delivered{0}, capture_pending{0}, capture_drops{0};
  std::atomic<uint64_t> feature_cpu_ns{0}, read_cpu_ns{0}, output_cpu_ns{0}, backend_data_cpu_ns{0};
  void mark_first(uint64_t value);
  void write(const Config &, bool success, const std::string &error) const;
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
    if (items_.size() >= capacity_) ++full_waits_;
    space_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
    wait_ns += now_ns() - start;
    if (closed_) return false;
    items_.push_back(std::move(value)); peak_ = std::max(peak_, items_.size());
    available_.notify_one(); return true;
  }
  bool try_pop(T &value) {
    std::lock_guard<std::mutex> lock(mutex_); return take(value);
  }
  size_t peak() { std::lock_guard<std::mutex> lock(mutex_); return peak_; }
  uint64_t full_waits() { std::lock_guard<std::mutex> lock(mutex_); return full_waits_; }
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
  size_t peak_ = 0;
  uint64_t full_waits_ = 0;
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
void run_online_frontend(const Config &, Metrics &);
void run_online_backend(const Config &, Metrics &);
} // namespace tcp_bench
