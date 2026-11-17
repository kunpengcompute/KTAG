#include "common.h"
#include "offline_input.h"
#include "file_summary.h"
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
    Metrics &metrics, uint64_t run, uint32_t source, const std::string &path, std::ofstream &audit)
    : c_(c), schema_(schema), channel_(channel), metrics_(metrics), run_(run), source_(source),
      output_(c.feature_queue), audit_(audit), active_(c.feature_threads),
      registry_(ktag_feature_registry_create(), ktag_feature_registry_destroy),
      domain_(nullptr, ktag_domain_store_destroy) {
    load_feature_plugins(c, registry_.get(), plugins_);
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
    capture_ = std::make_unique<OfflineInput>(path, c.packet_pool);
    stats_.index = source; stats_.name = path; stats_.link_type = capture_->link_type();
    processed_before_ = metrics.processed; rejected_before_ = metrics.rejected;
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
    } catch (...) { cancel(); channel_.stop(); join(); throw; }
  }
  const SourceStats &stats() const { return stats_; }
  void finish() {
    check_failure(); require(source_ended_, "file ended without FILE_END");
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
    auto cpu = thread_cpu_ns(); uint64_t zero = 0;
    metrics_.first_read.compare_exchange_strong(zero, now_ns());
    while (!cancelled_) {
      require(!interrupted, "interrupted reading PCAP");
      Packet packet; auto started = now_ns(); auto status = capture_->next(packet);
      metrics_.read_ns += now_ns()-started;
      if (status == KTAG_STATUS_TIMEOUT) continue;
      if (status == KTAG_ERROR_CAPACITY) {
        ++stats_.pool_waits; ++metrics_.pool_waits;
        started = now_ns(); std::this_thread::yield(); metrics_.pool_wait_ns += now_ns()-started; continue;
      }
      if (status == KTAG_STATUS_END_OF_STREAM) break;
      if (status != KTAG_SUCCESS) {
        auto error = capture_->error();
        if (recoverable_truncation(status, stats_.packets, error)) {
          stats_.warning = error.substr(0, 4096); break;
        }
        throw std::runtime_error("capture read failed: "+error);
      }
      require(packet.value.captured_length <= packet.value.original_length, "captured length exceeds original length");
      ++stats_.packets; stats_.bytes += packet.value.captured_length; stats_.original_bytes += packet.value.original_length;
      ++metrics_.packets; metrics_.bytes += packet.value.captured_length; metrics_.original_bytes += packet.value.original_length;
      if (packet.value.captured_length < packet.value.original_length) { ++stats_.short_captures; ++metrics_.short_captures; }
      source_seen_ = stats_.packets;
      ktag_flow_key key{};
      if (ktag_packet_get_flow_key(&packet.value, &key) != KTAG_SUCCESS) { ++stats_.unsupported; ++metrics_.unsupported; continue; }
      auto worker = ktag_flow_select_worker(&key, c_.feature_threads, c_.flow_hash_seed);
      require(worker != KTAG_WORKER_INDEX_INVALID && worker < packets_.size(), "invalid feature worker selection");
      if (!packets_[worker]->push(std::move(packet), metrics_.queue_wait_ns)) break;
    }
    close_packets(); metrics_.read_cpu_ns += thread_cpu_ns()-cpu;
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
      send_batch(); stats_.records = source_records;
      stats_.processed = metrics_.processed.load()-processed_before_;
      stats_.rejected = metrics_.rejected.load()-rejected_before_;
      Writer w; write_source(w, stats_); channel_.control(Type::file_end, run_, w); source_ended_ = true;
    }
    metrics_.output_cpu_ns += thread_cpu_ns()-cpu;
  }
  const Config &c_; const ktag_feature_schema *schema_; Channel &channel_; Metrics &metrics_;
  uint64_t run_; uint32_t source_; SourceStats stats_; uint64_t processed_before_ = 0, rejected_before_ = 0;
  Queue<Record> output_; std::ofstream &audit_;
  std::atomic<uint32_t> active_;
  // Contexts borrow these objects; destroy contexts first, including on a
  // partially constructed pipeline.
  FeaturePlugins plugins_;
  Registry registry_; Domain domain_;
  std::vector<std::unique_ptr<Queue<Packet>>> packets_;
  std::unique_ptr<OfflineInput> capture_;
  std::vector<Feature> contexts_;
  std::thread capture_thread_, sender_; std::vector<std::thread> workers_;
  std::atomic<bool> cancelled_{false}, source_ended_{false};
  std::atomic<uint64_t> source_seen_{0};
  std::mutex error_mutex_; std::string error_;
};

void run_frontend(const Config &c, Metrics &metrics) {
  auto schema = make_schema(); metrics.schema_hash = ktag_feature_schema_get_hash(schema.get());
  std::ofstream audit;
  if (!c.audit.empty()) { audit.open(c.audit, std::ios::binary|std::ios::trunc); require(bool(audit), "cannot open frontend audit"); }
  Channel channel(connect_to(c), c.timeout_s, c.max_body, metrics); uint64_t run = 0;
  try {
    Writer hello; hello.u64(metrics.schema_hash); hello.u64(c.rules_hash());
    hello.u32(ktag_feature_schema_get_value_count(schema.get())); hello.u32(KTAG_DEMO_UB_FRAME_VERSION);
    hello.u32(uint32_t(c.level)); hello.u32(c.max_body); hello.u32(uint32_t(c.pcaps.size()));
    channel.control(Type::hello, 0, hello);
    auto ready = channel.receive();
    if (ready.type == Type::error) expect(ready, Type::ready, ready.run);
    require(ready.type == Type::ready && ready.run != 0, "invalid local-PCAP READY");
    run = ready.run; Reader ready_reader(ready.body); ready_reader.finish();
    metrics.initialization_ns = now_ns()-metrics.started;
    for (uint32_t index = 0; index < c.pcaps.size(); ++index) {
      SourcePipeline pipeline(c, schema.get(), channel, metrics, run, index, c.pcaps[index], audit);
      Writer begin; begin.u32(index); begin.text(c.pcaps[index]); channel.control(Type::file_begin, run, begin);
      auto file_ready = channel.receive(); expect(file_ready, Type::file_ready, run);
      Reader ready_body(file_ready.body); require(ready_body.u32() == index, "FILE_READY index mismatch"); ready_body.finish();
      if (!index) metrics.initialization_ns = now_ns()-metrics.started;
      auto started = now_ns(); pipeline.start();
      try {
        Message done;
        for (;;) {
          done = channel.receive();
          if (done.type != Type::progress) break;
          expect(done, Type::progress, run); Reader heartbeat(done.body);
          require(heartbeat.u32() == index && heartbeat.u64() == 0, "invalid backend heartbeat"); heartbeat.finish();
        }
        expect(done, Type::file_done, run); pipeline.finish();
        SourceStats stats = pipeline.stats(); Reader d(done.body);
        require(d.u32() == index, "FILE_DONE index mismatch");
        auto received = d.u64(); stats.submitted = d.u64(); stats.filtered = d.u64(); stats.predictions = d.u64();
        require(d.u32() == uint32_t(stats.warning.empty()), "FILE_DONE input completeness mismatch"); d.finish();
        require(received == stats.records && received == stats.submitted && stats.submitted == stats.filtered+stats.predictions,
                "file feature/inference counts do not close");
        stats.elapsed_ns = now_ns()-started; metrics.sources.push_back(stats);
        metrics.submitted += stats.submitted; metrics.filtered += stats.filtered; metrics.predictions += stats.predictions;
        if (!stats.warning.empty()) std::cerr << "WARNING: truncated PCAP; partial results retained: " << stats.name << " (" << stats.warning << ")\n";
      } catch (const std::exception &e) {
        auto original = std::string(e.what()); pipeline.cancel(); channel.stop();
        try { pipeline.check_failure(); } catch (const std::exception &worker) {
          throw std::runtime_error(original+"; frontend worker: "+worker.what());
        }
        throw;
      }
    }
    require(metrics.records == metrics.extracted && metrics.records == metrics.submitted, "final extracted/sent/submitted mismatch");
    Writer end; end.u64(metrics.packets); end.u64(metrics.bytes); end.u64(metrics.records); end.u64(metrics.batches);
    channel.control(Type::end, run, end); auto done = channel.receive(); expect(done, Type::done, run);
    Reader d(done.body); require(d.u64() == metrics.records && d.u64() == metrics.submitted &&
      d.u64() == metrics.filtered && d.u64() == metrics.predictions, "DONE total mismatch"); d.finish();
    metrics.completed = now_ns();
    if (audit.is_open()) { audit.flush(); require(bool(audit), "frontend audit flush failed"); }
  } catch (const std::exception &e) {
    try { Writer w; w.text(std::string(e.what()).substr(0, 4096)); channel.control(Type::error, run, w); } catch (...) {}
    channel.stop(); throw;
  }
}
} // namespace tcp_bench
