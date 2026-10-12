#include "common.h"
#include "live_capture.h"
#include "replay_controller.h"
#include "tcp_transport.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <time.h>
#include <cmath>

namespace tcp_bench {
class SourcePipeline {
 public:
  SourcePipeline(const Config &c, const ktag_feature_schema *schema, Channel &channel,
    Metrics &metrics, uint64_t run, uint32_t source, uint64_t expected,
    const std::string &smac, const std::string &dmac, std::ofstream &audit)
    : c_(c), schema_(schema), channel_(channel), metrics_(metrics), run_(run), source_(source),
      expected_(expected), output_(c.feature_queue), audit_(audit), active_(c.feature_threads),
      registry_(ktag_feature_registry_create(), ktag_feature_registry_destroy),
      domain_(nullptr, ktag_domain_store_destroy) {
    require(bool(registry_) && ktag_feature_registry_register_legacy_builtins(registry_.get()) == KTAG_SUCCESS,
            "cannot create builtin feature registry");
    ktag_domain_store_config dc{}; dc.struct_size = sizeof(dc); dc.api_version = KTAG_API_VERSION;
    dc.max_entries = KTAG_DOMAIN_DEFAULT_MAX_ENTRIES; dc.journal_capacity = KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY;
    domain_.reset(ktag_domain_store_create(&dc)); require(bool(domain_), "cannot create domain store");
    for (uint32_t i = 0; i < c.feature_threads; ++i) {
      packets_.push_back(std::make_unique<Queue<Packet>>(c.packet_queue));
      ktag_feature_config fc{}; fc.struct_size = sizeof(fc); fc.api_version = KTAG_API_VERSION;
      fc.schema = schema; fc.registry = registry_.get(); fc.shared_domain_store = domain_.get();
      fc.flow_capacity = c.flow_capacity; fc.tcp_idle_timeout_us = c.tcp_idle_us;
      fc.udp_idle_timeout_us = c.udp_idle_us; fc.udp_packet_limit = c.udp_packet_limit;
      fc.lifecycle_mode = c.modern ? KTAG_FEATURE_LIFECYCLE_MODERN : KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
      contexts_.emplace_back(ktag_feature_create(&fc), ktag_feature_destroy);
      require(bool(contexts_.back()), "cannot create feature context");
    }
    capture_ = std::make_unique<LiveCapture>(c, smac, dmac);
  }
  ~SourcePipeline() { cancel(); join(); }
  void start() {
    try {
      sender_ = std::thread([this] { guard([&] { send_features(); }); });
      for (uint32_t i = 0; i < c_.feature_threads; ++i)
        workers_.emplace_back([this, i] {
          guard([&] { feature_worker(i); });
          if (active_.fetch_sub(1) == 1) output_.close();
        });
      capture_thread_ = std::thread([this] { guard([&] { capture_packets(); }); });
    } catch (...) { cancel(); join(); throw; }
  }
  void replay_done(uint64_t sent) {
    require(sent == expected_, "replay sent count differs from SOURCE_BEGIN");
    replay_done_at_ = now_ns(); replay_done_ = true;
  }
  void finish() {
    check_failure(); require(source_ended_, "source ended without SOURCE_END");
  }
  void check_failure() {
    join(); std::lock_guard<std::mutex> lock(error_mutex_); require(error_.empty(), error_);
  }
  void cancel() {
    cancelled_ = true; close_packets(); output_.close();
  }
 private:
  template<class Fn> void guard(Fn fn) {
    try { fn(); } catch (const std::exception &e) {
      { std::lock_guard<std::mutex> lock(error_mutex_); if (error_.empty()) error_ = e.what(); }
      cancel(); channel_.stop();
    } catch (...) {
      { std::lock_guard<std::mutex> lock(error_mutex_); if (error_.empty()) error_ = "unexpected worker exception"; }
      cancel(); channel_.stop();
    }
  }
  void close_packets() { for (auto &q : packets_) q->close(); }
  void join() {
    if (capture_thread_.joinable()) capture_thread_.join();
    for (auto &worker : workers_) if (worker.joinable()) worker.join();
    if (sender_.joinable()) sender_.join();
  }
  void capture_packets() {
    auto cpu = thread_cpu_ns(); uint64_t count = 0, last = now_ns();
    while (!cancelled_) {
      require(!interrupted, "interrupted capturing packets");
      Packet packet; auto started = now_ns(); bool got = capture_->next(packet, count+1);
      metrics_.capture_ns += now_ns()-started;
      if (got) {
        ++count; source_bytes_ += packet.value.captured_length;
        source_seen_ = count;
        ++metrics_.packets; metrics_.bytes += packet.value.captured_length; last = now_ns();
        uint64_t zero = 0; metrics_.first_capture.compare_exchange_strong(zero, last);
        metrics_.last_capture = last;
        require(count <= expected_, "received more replay packets than expected");
        ktag_flow_key key{}; auto status = ktag_packet_get_flow_key(&packet.value, &key);
        if (status == KTAG_ERROR_NOT_SUPPORTED || status == KTAG_ERROR_INVALID_ARGUMENT) {
          ++metrics_.unsupported; continue;
        }
        require(status == KTAG_SUCCESS, "cannot parse packet flow key");
        auto worker = ktag_flow_select_worker(&key, c_.feature_threads, c_.flow_hash_seed);
        if (!packets_[worker]->push(std::move(packet), metrics_.queue_wait_ns)) break;
      } else {
        if (replay_done_) {
          auto elapsed = now_ns()-replay_done_at_.load();
          require(elapsed <= uint64_t(c_.drain_timeout_s)*1000000000ULL,
                  "missing replay packets after replay finished; received="+std::to_string(count)+
                  " expected="+std::to_string(expected_));
          if (count == expected_ && now_ns()-last >= uint64_t(c_.quiet_ms)*1000000ULL) break;
        }
        capture_->wait();
      }
    }
    if (!cancelled_) {
      auto dropped = capture_->drops(); metrics_.capture_drops += dropped;
      require(dropped == 0, "NIC/libpcap reported dropped packets");
      require(count == expected_ && replay_done_, "incomplete source capture");
      source_packets_ = count;
    }
    close_packets(); metrics_.capture_cpu_ns += thread_cpu_ns()-cpu;
  }
  void emit(ktag_feature_batch *batch) {
    for (uint32_t i = 0; i < ktag_feature_batch_get_count(batch); ++i) {
      ktag_feature_record view{}; Record r; r.source_index = source_;
      require(ktag_feature_batch_get_record(batch, i, &view) == KTAG_SUCCESS &&
              ktag_feature_batch_get_record_details(batch, i, &r.details) == KTAG_SUCCESS,
              "cannot read extracted feature record");
      r.values.assign(view.values, view.values+view.value_count);
      require(output_.push(std::move(r), metrics_.queue_wait_ns), "feature queue cancelled");
      ++metrics_.extracted;
    }
  }
  void feature_worker(uint32_t index) {
    auto cpu = thread_cpu_ns();
    FeatureBatch batch(ktag_feature_batch_create(schema_, 64), ktag_feature_batch_destroy);
    require(bool(batch), "cannot allocate extraction batch");
    Packet packet; uint64_t last_expire = 0;
    while (packets_[index]->pop(packet) && !cancelled_) {
      uint64_t timestamp = uint64_t(packet.value.timestamp.seconds)*1000000ULL+
        uint64_t(packet.value.timestamp.microseconds);
      auto start = now_ns(); metrics_.mark_first(start);
      auto status = ktag_feature_process_packet(contexts_[index].get(), &packet.value, batch.get());
      metrics_.feature_ns += now_ns()-start; ++metrics_.processed;
      packet = Packet{};
      if (status == KTAG_ERROR_NOT_SUPPORTED || status == KTAG_ERROR_INVALID_ARGUMENT) {
        ++metrics_.rejected; continue;
      }
      require(status == KTAG_SUCCESS, "feature process failed with status "+std::to_string(status)); emit(batch.get());
      if (c_.expire_ms && (last_expire == 0 || (timestamp >= last_expire &&
          timestamp-last_expire >= uint64_t(c_.expire_ms)*1000ULL))) {
        start = now_ns(); status = ktag_feature_expire(contexts_[index].get(), timestamp, batch.get());
        metrics_.feature_ns += now_ns()-start; require(status == KTAG_SUCCESS, "feature expire failed");
        emit(batch.get()); last_expire = timestamp;
      }
    }
    if (!cancelled_) {
      auto start = now_ns(); auto status = ktag_feature_flush(contexts_[index].get(), batch.get());
      metrics_.feature_ns += now_ns()-start; require(status == KTAG_SUCCESS, "feature flush failed"); emit(batch.get());
      ktag_feature_statistics s{}; s.struct_size = sizeof(s); s.api_version = KTAG_API_VERSION;
      require(ktag_feature_get_statistics(contexts_[index].get(), &s) == KTAG_SUCCESS,
              "cannot read feature statistics");
      require(!s.dropped_flows && !s.capacity_rejections && !s.callback_failures,
              "feature extractor lost flows or failed callbacks");
    }
    metrics_.feature_cpu_ns += thread_cpu_ns()-cpu;
  }
  void send_features() {
    auto cpu = thread_cpu_ns(); Codec codec(c_.level); Bytes raw; uint32_t count = 0;
    uint64_t source_records = 0; Clock::time_point deadline{};
    auto heartbeat_interval = std::chrono::nanoseconds(std::min<uint64_t>(1000000000ULL,
      uint64_t(c_.timeout_s)*1000000000ULL/3));
    auto heartbeat = Clock::now()+heartbeat_interval;
    auto send_batch = [&] {
      if (!count) return;
      Message m; m.type = Type::features; m.run = run_; m.sequence = metrics_.batches.load()+1;
      m.count = count; m.raw_size = uint32_t(raw.size());
      auto start = now_ns(); m.body = codec.compress(raw); metrics_.compress_ns += now_ns()-start;
      if (audit_.is_open()) { audit_.write(reinterpret_cast<const char *>(raw.data()), std::streamsize(raw.size()));
        require(bool(audit_), "cannot write frontend feature audit"); }
      start = now_ns(); channel_.send(m); metrics_.send_ns += now_ns()-start;
      metrics_.raw_bytes += raw.size(); metrics_.compressed_bytes += m.body.size();
      metrics_.records += count; source_records += count; ++metrics_.batches;
      raw.clear(); count = 0;
    };
    Record record;
    while (!cancelled_) {
      bool got = output_.pop_until(record, count ? std::min(deadline, heartbeat) : heartbeat);
      if (!got) {
        if (count && Clock::now() >= deadline) send_batch();
        if (output_.drained()) break;
        if (Clock::now() >= heartbeat) {
          Writer progress; progress.u32(source_); progress.u64(source_seen_);
          channel_.control(Type::progress, run_, progress); heartbeat = Clock::now()+heartbeat_interval;
        }
        continue;
      }
      if (!count) deadline = Clock::now()+std::chrono::microseconds(c_.compression_timeout_us);
      auto start = now_ns(); auto offset = raw.size();
      raw.resize(offset+ktag_demo_ub_feature_frame_size(uint32_t(record.values.size())));
      ktag_demo_ub_frame_contract contract{}; contract.frontend_id_hash = 1; contract.epoch = run_;
      contract.sequence = metrics_.records.load()+count+1; contract.schema_hash = ktag_feature_schema_get_hash(schema_);
      contract.ruleset_hash = c_.rules_hash(); size_t encoded = 0; std::string error;
      auto encoded_ok = ktag_demo_ub_encode_feature_frame(raw.data()+offset, raw.size()-offset,
        contract, record, false, &encoded, &error);
      require(encoded_ok, "feature encode: "+error);
      require(encoded == raw.size()-offset, "feature encoder length mismatch");
      metrics_.encode_ns += now_ns()-start;
      if (++count >= c_.compression_records || Clock::now() >= deadline) send_batch();
      if (Clock::now() >= heartbeat) {
        Writer progress; progress.u32(source_); progress.u64(source_seen_);
        channel_.control(Type::progress, run_, progress); heartbeat = Clock::now()+heartbeat_interval;
      }
    }
    if (!cancelled_) {
      send_batch(); Writer w; w.u32(source_); w.u64(source_packets_); w.u64(source_bytes_);
      w.u64(source_records); channel_.control(Type::source_end, run_, w); source_ended_ = true;
    }
    metrics_.output_cpu_ns += thread_cpu_ns()-cpu;
  }
  const Config &c_; const ktag_feature_schema *schema_; Channel &channel_; Metrics &metrics_;
  uint64_t run_; uint32_t source_; uint64_t expected_, source_bytes_ = 0, source_packets_ = 0;
  Queue<Record> output_; std::ofstream &audit_;
  std::atomic<uint32_t> active_;
  // Contexts borrow these objects; destroy contexts first, including on a
  // partially constructed pipeline.
  Registry registry_; Domain domain_;
  std::vector<std::unique_ptr<Queue<Packet>>> packets_;
  std::unique_ptr<LiveCapture> capture_;
  std::vector<Feature> contexts_;
  std::thread capture_thread_, sender_; std::vector<std::thread> workers_;
  std::atomic<bool> cancelled_{false}, replay_done_{false}, source_ended_{false};
  std::atomic<uint64_t> replay_done_at_{0};
  std::atomic<uint64_t> source_seen_{0};
  std::mutex error_mutex_; std::string error_;
};

void run_frontend(const Config &c, Metrics &metrics) {
  auto schema = make_schema();
  metrics.schema_hash = ktag_feature_schema_get_hash(schema.get());
  Channel channel(connect_to(c), c.timeout_s, c.max_body, metrics);
  uint64_t run = 0;
  try {
    Writer hello; hello.u64(ktag_feature_schema_get_hash(schema.get())); hello.u64(c.rules_hash());
    hello.u32(ktag_feature_schema_get_value_count(schema.get())); hello.u32(KTAG_DEMO_UB_FRAME_VERSION);
    hello.u32(uint32_t(c.level)); hello.u32(c.max_body); channel.control(Type::hello, 0, hello);
    auto ready = channel.receive();
    if (ready.type == Type::error) expect(ready, Type::ready, ready.run);
    require(ready.type == Type::ready && ready.run != 0, "invalid READY"); run = ready.run;
    Reader r(ready.body); auto sources = r.u32(); auto smac = r.text(); auto dmac = r.text();
    auto mode = r.text(); auto mbps = std::stod(r.text());
    require((mode == "original" || mode == "mbps") && std::isfinite(mbps) && mbps >= 0,
            "invalid backend replay rate contract");
    metrics.actual_rate_mode = mode; metrics.actual_rate_mbps = mbps;
    metrics.replay_affinity = r.text(); r.finish();
    require(sources > 0, "backend source list is empty");
    check_interface(c.capture_interface, c.local_ip, smac, dmac);
    std::ofstream audit;
    if (!c.audit.empty()) { audit.open(c.audit, std::ios::binary|std::ios::trunc); require(bool(audit), "cannot open frontend audit"); }
    metrics.initialization_ns = now_ns()-metrics.started;
    for (uint32_t index = 0; index < sources; ++index) {
      auto begin = channel.receive(); expect(begin, Type::source_begin, run);
      Reader b(begin.body); auto source = b.u32(); auto expected = b.u64(); auto name = b.text(); b.finish();
      require(source == index && expected > 0, "invalid source index/count"); metrics.expected_packets += expected;
      SourcePipeline pipeline(c, schema.get(), channel, metrics, run, source, expected, smac, dmac, audit);
      pipeline.start(); Writer capture_ready; capture_ready.u32(source);
      if (!index) metrics.initialization_ns = now_ns()-metrics.started;
      channel.control(Type::capture_ready, run, capture_ready);
      std::cout << "Capture ready source=" << source << " PCAP=" << name << '\n';
      try {
        auto receive_control = [&](Type type) {
          for (;;) {
            auto message = channel.receive();
            if (message.type != Type::progress) { expect(message, type, run); return message; }
            expect(message, Type::progress, run); Reader progress(message.body);
            require(progress.u32() == source && progress.u64() == 0, "invalid backend heartbeat"); progress.finish();
          }
        };
        auto replay = receive_control(Type::replay_done);
        Reader d(replay.body); require(d.u32() == source, "REPLAY_DONE source mismatch");
        auto sent = d.u64(); d.finish(); pipeline.replay_done(sent); metrics.sent_packets += sent;
        auto done = receive_control(Type::source_done);
        Reader s(done.body); require(s.u32() == source, "SOURCE_DONE source mismatch"); s.finish(); pipeline.finish();
      } catch (const std::exception &e) {
        auto original = std::string(e.what()); pipeline.cancel(); channel.stop();
        try { pipeline.check_failure(); } catch (const std::exception &worker_error) {
          throw std::runtime_error(original+"; frontend worker: "+worker_error.what());
        }
        throw;
      }
    }
    require(metrics.records == metrics.extracted, "extracted/sent feature count mismatch");
    Writer end; end.u64(metrics.packets); end.u64(metrics.bytes); end.u64(metrics.records); end.u64(metrics.batches);
    channel.control(Type::end, run, end);
    auto done = channel.receive(); expect(done, Type::done, run);
    Reader d(done.body); auto received = d.u64(), submitted = d.u64(), filtered = d.u64(), predictions = d.u64(); d.finish();
    require(received == metrics.records && received == submitted && submitted == filtered+predictions,
            "final feature/inference counts do not close");
    metrics.submitted = submitted; metrics.filtered = filtered; metrics.predictions = predictions;
    metrics.completed = now_ns();
    if (audit.is_open()) { audit.flush(); require(bool(audit), "cannot flush frontend audit"); }
  } catch (const std::exception &e) {
    try { Writer w; w.text(e.what()); channel.control(Type::error, run, w); } catch (...) {}
    channel.stop(); throw;
  }
}
} // namespace tcp_bench
