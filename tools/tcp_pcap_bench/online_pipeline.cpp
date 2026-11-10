#include "common.h"
#include "file_summary.h"
#include "tcp_transport.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <random>
#include <sched.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#ifdef KTAG_TCP_BENCH_HAVE_DPDK
#include "ktag_capture_dpdk.h"
#endif

namespace tcp_bench {
namespace {
constexpr uint32_t online_contract = 0x4f4e4c31; // ONL1, separate from local-PCAP handshakes.

cpu_set_t allowed_cpus() {
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  require(sched_getaffinity(0, sizeof(cpus), &cpus) == 0, "cannot read CPU affinity");
  return cpus;
}
void check_cpus(const std::vector<uint32_t> &cpus, const cpu_set_t &allowed) {
  for (auto cpu : cpus)
    require(cpu < CPU_SETSIZE && CPU_ISSET(cpu, &allowed),
            "worker CPU "+std::to_string(cpu)+" is outside the initial allowed CPU set");
}
void bind_worker(const char *stage, uint32_t index, const std::vector<uint32_t> &cpus,
                 const cpu_set_t &allowed) {
  auto mask = allowed;
  if (!cpus.empty()) { CPU_ZERO(&mask); CPU_SET(cpus.at(index), &mask); }
  auto error = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
  require(error == 0, "bind "+std::string(stage)+": "+std::strerror(error));
  auto name = std::string("tcp_")+stage+"_"+std::to_string(index);
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
  std::printf("TCP worker: stage=%s, worker=%u, tid=%ld, cpu=%s\n", stage, index,
              long(syscall(SYS_gettid)), cpus.empty() ? "allowed-set" : std::to_string(cpus[index]).c_str());
}
void mark_last(std::atomic<uint64_t> &field, uint64_t value) {
  auto old = field.load();
  while (old < value && !field.compare_exchange_weak(old, value)) {}
}

// All socket/queue owners outlive this object. Destruction joins even if
// thread creation, model loading or a handshake fails partway through.
struct OnlineRun {
  std::vector<std::unique_ptr<Channel>> channels;
  std::vector<std::thread> threads;
  std::function<void()> cancel_input;
  std::atomic<bool> failed{false};
  std::mutex mutex;
  std::condition_variable changed;
  std::string error;

  ~OnlineRun() { cancel(); join(); }
  void cancel() {
    failed = true;
    for (auto &channel : channels) if (channel) channel->stop();
    if (cancel_input) cancel_input();
    changed.notify_all();
  }
  void fail(const std::string &why) {
    { std::lock_guard<std::mutex> lock(mutex); if (error.empty()) error = why; }
    cancel();
  }
  template<class Fn> void guard(const std::string &stage, Fn fn) {
    try { fn(); }
    catch (const std::exception &e) { fail(stage+": "+e.what()); }
    catch (...) { fail(stage+": unexpected exception"); }
  }
  void join() { for (auto &thread : threads) if (thread.joinable()) thread.join(); }
  void check() {
    std::lock_guard<std::mutex> lock(mutex);
    require(error.empty(), error);
    require(!failed, "online run cancelled");
  }
};

Writer online_hello(const Config &c, uint64_t schema, uint64_t run, uint32_t lane) {
  Writer w;
  w.u32(online_contract); w.u64(schema); w.u64(c.rules_hash()); w.u64(run);
  w.u32(lane); w.u32(c.inference_threads); w.u32(c.sender_threads);
  w.u32(uint32_t(c.level)); w.u32(c.max_body); w.u32(c.compression_records);
  w.u32(c.inference_records); w.u32(c.inference_timeout_us);
  w.u32(KTAG_DEMO_UB_FRAME_VERSION); w.u32(KTAG_LEGACY_V1_VALUE_COUNT);
  return w;
}
uint64_t validate_hello(const Message &message, const Config &c, uint64_t schema, uint32_t &lane) {
  expect(message, Type::hello, 0);
  Reader r(message.body);
  require(r.u32() == online_contract && r.u64() == schema && r.u64() == c.rules_hash(),
          "online protocol/schema/feature settings mismatch");
  auto run = r.u64();
  lane = r.u32();
  auto inference_threads = r.u32();
  auto sender_threads = r.u32();
  require(run != 0, "online HELLO has an invalid zero run ID");
  require(lane < c.inference_threads, "online channel ID out of range: received="+
          std::to_string(lane)+", channel_count="+std::to_string(c.inference_threads));
  require(inference_threads == c.inference_threads, "online inference thread count mismatch: frontend="+
          std::to_string(inference_threads)+", backend="+std::to_string(c.inference_threads));
  require(sender_threads == c.sender_threads, "online sender thread count mismatch: frontend="+
          std::to_string(sender_threads)+", backend="+std::to_string(c.sender_threads));
  require(r.u32() == uint32_t(c.level) && r.u32() == c.max_body && r.u32() == c.compression_records &&
          r.u32() == c.inference_records && r.u32() == c.inference_timeout_us &&
          r.u32() == KTAG_DEMO_UB_FRAME_VERSION && r.u32() == KTAG_LEGACY_V1_VALUE_COUNT,
          "online compression/batch/wire settings mismatch");
  r.finish(); return run;
}

struct TxLane {
  Bytes raw;
  uint32_t count = 0;
  uint64_t records = 0, batches = 0;
  bool ended = false, done = false;
  Clock::time_point deadline{}, ack_deadline{}, heartbeat = Clock::now();
};

#ifdef KTAG_TCP_BENCH_HAVE_DPDK
struct RxCounters {
  uint64_t packets = 0, bytes = 0, unsupported = 0;
  std::atomic<uint64_t> queue_wait_ns{0};
};
struct FeatureCounters {
  uint64_t processed = 0, rejected = 0, extracted = 0, work_ns = 0;
  std::atomic<uint64_t> queue_wait_ns{0};
};

class OnlineFrontend {
 public:
  OnlineFrontend(const Config &c, Metrics &metrics)
      : c_(c), metrics_(metrics), allowed_(allowed_cpus()), schema_(make_schema()),
        registry_(ktag_feature_registry_create(), ktag_feature_registry_destroy),
        domain_(nullptr, ktag_domain_store_destroy), capture_(nullptr, ktag_dpdk_capture_close),
        active_rx_(c.rx_threads), active_features_(c.feature_threads), rules_hash_(c.rules_hash()), lanes_(c.inference_threads) {
    check_cpus(c.rx_cpus, allowed_); check_cpus(c.feature_cpus, allowed_); check_cpus(c.sender_cpus, allowed_);
    metrics_.schema_hash = ktag_feature_schema_get_hash(schema_.get());
    require(bool(registry_) && ktag_feature_registry_register_legacy_builtins(registry_.get()) == KTAG_SUCCESS,
            "cannot create builtin feature registry");
    ktag_domain_store_config dc{}; dc.struct_size = sizeof(dc); dc.api_version = KTAG_API_VERSION;
    dc.max_entries = KTAG_DOMAIN_DEFAULT_MAX_ENTRIES; dc.journal_capacity = KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY;
    domain_.reset(ktag_domain_store_create(&dc)); require(bool(domain_), "cannot create domain store");
    for (uint32_t i = 0; i < c.feature_threads; ++i) {
      packets_.push_back(std::make_unique<Queue<Packet>>(c.packet_queue));
      feature_stats_.push_back(std::make_unique<FeatureCounters>());
      ktag_feature_config fc{}; fc.struct_size = sizeof(fc); fc.api_version = KTAG_API_VERSION;
      fc.schema = schema_.get(); fc.registry = registry_.get(); fc.shared_domain_store = domain_.get();
      fc.flow_capacity = c.flow_capacity; fc.tcp_idle_timeout_us = c.tcp_idle_us;
      fc.udp_idle_timeout_us = c.udp_idle_us; fc.udp_packet_limit = c.udp_packet_limit;
      fc.lifecycle_mode = c.modern ? KTAG_FEATURE_LIFECYCLE_MODERN : KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
      contexts_.emplace_back(ktag_feature_create(&fc), ktag_feature_destroy);
      require(bool(contexts_.back()), "cannot create feature context");
    }
    for (uint32_t i = 0; i < c.sender_threads; ++i)
      outputs_.push_back(std::make_unique<Queue<Record>>(c.feature_queue/c.sender_threads+
                                                       (i < c.feature_queue%c.sender_threads ? 1 : 0)));
    for (uint32_t i = 0; i < c.rx_threads; ++i) rx_stats_.push_back(std::make_unique<RxCounters>());
    // EAL may narrow the main thread's mask. Preserve the original numactl /
    // cpuset mask for every business worker, including omitted CPU lists.
    auto status = ktag_dpdk_eal_init(c.eal_args.c_str());
    auto eal_error = std::string(ktag_dpdk_capture_get_last_error());
    require(sched_setaffinity(0, sizeof(allowed_), &allowed_) == 0, "cannot restore pre-EAL CPU affinity");
    require(status == KTAG_SUCCESS, "DPDK EAL init: "+eal_error);
    ktag_dpdk_capture_config cc{}; cc.struct_size = sizeof(cc); cc.api_version = KTAG_API_VERSION;
    cc.device_name = c.device.c_str(); cc.rx_queue_count = c.rx_queues; cc.rx_queue_depth = c.rx_depth;
    cc.mbuf_pool_size = c.mbuf_pool; cc.burst_size = c.burst; cc.packet_mode = KTAG_DPDK_PACKET_OWNED;
    cc.rx_idle_eof_us = c.rx_idle_eof_us; cc.poll_interval_us = c.rx_poll_us;
    capture_.reset(ktag_dpdk_capture_open(&cc));
    require(bool(capture_), "DPDK open: "+std::string(ktag_dpdk_capture_get_last_error()));
    require(ktag_dpdk_capture_prepare_readers(capture_.get(), c.rx_threads) == KTAG_SUCCESS,
            "DPDK prepare RX: "+std::string(ktag_dpdk_capture_get_last_error()));
    run_.cancel_input = [this] {
      ktag_dpdk_capture_interrupt(capture_.get());
      for (auto &queue : packets_) queue->close();
      for (auto &queue : outputs_) queue->close();
    };
  }
  ~OnlineFrontend() { run_.cancel(); run_.join(); }

  void execute() {
    std::random_device random;
    session_ = now_ns() ^ (uint64_t(random()) << 32) ^ random();
    if (!session_) session_ = 1;
    for (uint32_t lane = 0; lane < c_.inference_threads; ++lane) {
      run_.channels.push_back(std::make_unique<Channel>(connect_to(c_), c_.timeout_s, c_.max_body, metrics_, false));
      run_.channels.back()->control(Type::hello, 0, online_hello(c_, metrics_.schema_hash, session_, lane));
    }
    for (uint32_t lane = 0; lane < c_.inference_threads; ++lane) {
      auto message = run_.channels[lane]->receive(); expect(message, Type::ready, session_);
      Reader ready(message.body); require(ready.u32() == lane, "READY channel mismatch"); ready.finish();
      std::printf("TCP channel mapping: sender=%u, channel=%u, backend_worker=%u\n",
                  lane%c_.sender_threads, lane, lane);
    }
    for (uint32_t i = 0; i < c_.sender_threads; ++i)
      run_.threads.emplace_back([this, i] { run_.guard("sender="+std::to_string(i), [&] { sender(i); }); });
    for (uint32_t i = 0; i < c_.feature_threads; ++i)
      run_.threads.emplace_back([this, i] {
        run_.guard("feature="+std::to_string(i), [&] { feature(i); });
        if (active_features_.fetch_sub(1) == 1) for (auto &queue : outputs_) queue->close();
      });
    for (uint32_t i = 0; i < c_.rx_threads; ++i)
      run_.threads.emplace_back([this, i] {
        run_.guard("RX="+std::to_string(i), [&] { receive_packets(i); });
        if (active_rx_.fetch_sub(1) == 1) {
          merge_rx();
          for (auto &queue : packets_) queue->close();
        }
        run_.changed.notify_all();
      });
    {
      std::unique_lock<std::mutex> lock(run_.mutex);
      while (rx_ready_ != c_.rx_threads && !run_.failed)
        run_.changed.wait_for(lock, std::chrono::milliseconds(100));
    }
    run_.check();
    metrics_.initialization_ns = now_ns()-metrics_.started;
    metrics_.first_read = now_ns();
    auto started = Clock::now();
    std::printf("Online frontend ready: RX=%u, features=%u, senders=%u, channels=%u, duration_s=%u\n",
                c_.rx_threads, c_.feature_threads, c_.sender_threads, c_.inference_threads, c_.duration_s);
    std::fflush(stdout);
    go_ = true; run_.changed.notify_all();
    while (active_rx_ && !run_.failed) {
      if (interrupted || (c_.duration_s && Clock::now()-started >= std::chrono::seconds(c_.duration_s))) {
        stop_capture_ = true; ktag_dpdk_capture_interrupt(capture_.get());
      }
      std::unique_lock<std::mutex> lock(run_.mutex);
      run_.changed.wait_for(lock, std::chrono::milliseconds(50));
    }
    run_.join(); run_.check();
    require(metrics_.extracted == metrics_.records && metrics_.records == metrics_.submitted,
            "online extracted/sent/submitted totals mismatch");
    require(metrics_.completed != 0, "missing backend completion confirmations");
    auto source = source_stats(); source.submitted = metrics_.submitted;
    source.filtered = metrics_.filtered; source.predictions = metrics_.predictions;
    source.elapsed_ns = metrics_.completed-metrics_.first_read;
    metrics_.sources.push_back(source);
    print_capture_stats();
    for (uint32_t i = 0; i < c_.feature_threads; ++i)
      std::printf("Frontend packet queue: feature_worker=%u, peak=%zu, full_waits=%llu\n", i,
                  packets_[i]->peak(), (unsigned long long)packets_[i]->full_waits());
  }

 private:
  SourceStats source_stats() const {
    SourceStats s; s.name = c_.device; s.link_type = ktag_dpdk_capture_get_link_type(capture_.get());
    s.packets = metrics_.packets; s.bytes = metrics_.bytes; s.original_bytes = metrics_.original_bytes;
    s.unsupported = metrics_.unsupported; s.processed = metrics_.processed; s.rejected = metrics_.rejected;
    s.records = metrics_.extracted; return s;
  }
  void merge_rx() {
    for (const auto &s : rx_stats_) {
      metrics_.packets += s->packets; metrics_.bytes += s->bytes; metrics_.original_bytes += s->bytes;
      metrics_.unsupported += s->unsupported; metrics_.queue_wait_ns += s->queue_wait_ns.load();
    }
  }
  void receive_packets(uint32_t index) {
    bind_worker("rx", index, c_.rx_cpus, allowed_);
    require(ktag_dpdk_capture_reader_enter(capture_.get()) == KTAG_SUCCESS,
            "DPDK reader enter: "+std::string(ktag_dpdk_capture_get_last_error()));
    struct Leave {
      void *capture;
      ~Leave() { ktag_dpdk_capture_reader_leave(capture); }
    } leave{capture_.get()};
    ++rx_ready_; run_.changed.notify_all();
    {
      std::unique_lock<std::mutex> lock(run_.mutex);
      while (!go_ && !run_.failed) run_.changed.wait_for(lock, std::chrono::milliseconds(100));
    }
    auto cpu = thread_cpu_ns(); auto &s = *rx_stats_[index];
    while (!run_.failed && !stop_capture_) {
      Packet packet;
      auto status = c_.rx_threads == 1 ? ktag_dpdk_capture_next(capture_.get(), &packet.value)
          : ktag_dpdk_capture_next_queue(capture_.get(), index, &packet.value);
      if (status == KTAG_STATUS_TIMEOUT) continue;
      if (status == KTAG_STATUS_END_OF_STREAM) {
        stop_capture_ = true; ktag_dpdk_capture_interrupt(capture_.get()); break;
      }
      require(status == KTAG_SUCCESS, "DPDK receive: "+std::string(ktag_dpdk_capture_get_last_error()));
      ++s.packets; s.bytes += packet.value.captured_length;
      ktag_flow_key key{};
      if (ktag_packet_get_flow_key(&packet.value, &key) != KTAG_SUCCESS) { ++s.unsupported; continue; }
      auto worker = ktag_flow_select_worker(&key, c_.feature_threads, c_.flow_hash_seed);
      require(worker < packets_.size(), "invalid feature worker selection");
      if (!packets_[worker]->push(std::move(packet), s.queue_wait_ns)) break;
    }
    metrics_.read_cpu_ns += thread_cpu_ns()-cpu;
  }
  void emit(uint32_t index, ktag_feature_batch *batch, uint32_t &sender_cursor) {
    auto &s = *feature_stats_[index];
    for (uint32_t i = 0; i < ktag_feature_batch_get_count(batch); ++i) {
      ktag_feature_record view{}; Record r; r.source_index = 0;
      require(ktag_feature_batch_get_record(batch, i, &view) == KTAG_SUCCESS &&
              ktag_feature_batch_get_record_details(batch, i, &r.details) == KTAG_SUCCESS,
              "cannot read extracted record");
      r.values.assign(view.values, view.values+view.value_count);
      require(outputs_[sender_cursor]->push(std::move(r), s.queue_wait_ns), "feature output cancelled");
      sender_cursor = (sender_cursor+1)%c_.sender_threads; ++s.extracted;
    }
  }
  void feature(uint32_t index) {
    bind_worker("feature", index, c_.feature_cpus, allowed_);
    auto cpu = thread_cpu_ns(); auto &s = *feature_stats_[index];
    FeatureBatch batch(ktag_feature_batch_create(schema_.get(), 64), ktag_feature_batch_destroy);
    require(bool(batch), "cannot allocate extraction batch");
    Packet packet; uint64_t last_expire = 0; uint32_t sender_cursor = index%c_.sender_threads;
    while (packets_[index]->pop(packet) && !run_.failed) {
      auto timestamp = uint64_t(packet.value.timestamp.seconds)*1000000ULL+uint64_t(packet.value.timestamp.microseconds);
      auto start = now_ns();
      if (!s.processed) metrics_.mark_first(start);
      auto status = ktag_feature_process_packet(contexts_[index].get(), &packet.value, batch.get());
      s.work_ns += now_ns()-start; ++s.processed; packet = Packet{};
      if (status == KTAG_ERROR_NOT_SUPPORTED || status == KTAG_ERROR_INVALID_ARGUMENT) { ++s.rejected; continue; }
      require(status == KTAG_SUCCESS, "feature process status "+std::to_string(status));
      emit(index, batch.get(), sender_cursor);
      if (c_.expire_ms && (!last_expire || (timestamp >= last_expire && timestamp-last_expire >= uint64_t(c_.expire_ms)*1000))) {
        start = now_ns(); status = ktag_feature_expire(contexts_[index].get(), timestamp, batch.get());
        s.work_ns += now_ns()-start; require(status == KTAG_SUCCESS, "feature expire failed");
        emit(index, batch.get(), sender_cursor); last_expire = timestamp;
      }
    }
    if (!run_.failed) {
      auto start = now_ns(); auto status = ktag_feature_flush(contexts_[index].get(), batch.get());
      s.work_ns += now_ns()-start; require(status == KTAG_SUCCESS, "feature flush failed");
      emit(index, batch.get(), sender_cursor);
      ktag_feature_statistics stats{}; stats.struct_size = sizeof(stats); stats.api_version = KTAG_API_VERSION;
      require(ktag_feature_get_statistics(contexts_[index].get(), &stats) == KTAG_SUCCESS, "feature statistics failed");
      require(!stats.dropped_flows && !stats.capacity_rejections && !stats.callback_failures,
              "feature flow capacity exceeded or callbacks failed");
    }
    metrics_.processed += s.processed; metrics_.rejected += s.rejected; metrics_.extracted += s.extracted;
    metrics_.feature_ns += s.work_ns; metrics_.queue_wait_ns += s.queue_wait_ns.load();
    metrics_.feature_cpu_ns += thread_cpu_ns()-cpu;
  }
  void encode(uint32_t lane_index, Record &record) {
    auto &lane = lanes_[lane_index]; auto start = now_ns();
    if (!lane.count) lane.deadline = Clock::now()+std::chrono::microseconds(c_.compression_timeout_us);
    auto offset = lane.raw.size();
    lane.raw.resize(offset+ktag_demo_ub_feature_frame_size(uint32_t(record.values.size())));
    ktag_demo_ub_frame_contract contract{};
    contract.frontend_id_hash = 1; contract.epoch = session_; contract.sequence = lane.records+lane.count+1;
    contract.schema_hash = metrics_.schema_hash; contract.ruleset_hash = rules_hash_;
    size_t encoded = 0; std::string error;
    require(ktag_demo_ub_encode_feature_frame(lane.raw.data()+offset, lane.raw.size()-offset,
            contract, record, false, &encoded, &error), "feature encode: "+error);
    require(encoded == lane.raw.size()-offset, "feature frame size mismatch");
    ++lane.count; metrics_.encode_ns += now_ns()-start;
  }
  void flush_lane(uint32_t index, Codec &codec) {
    auto &lane = lanes_[index]; Message message;
    message.type = Type::features; message.run = session_; message.sequence = lane.batches+1;
    message.count = lane.count; message.raw_size = uint32_t(lane.raw.size());
    auto started = now_ns(); message.body = codec.compress(lane.raw); metrics_.compress_ns += now_ns()-started;
    metrics_.raw_bytes += lane.raw.size(); metrics_.compressed_bytes += message.body.size();
    run_.channels[index]->start_send(message);
    lane.records += lane.count; ++lane.batches; metrics_.records += lane.count; ++metrics_.batches;
    lane.count = 0; lane.raw.clear();
  }
  void sender(uint32_t index) {
    bind_worker("tx", index, c_.sender_cpus, allowed_);
    auto cpu = thread_cpu_ns(); Codec codec(c_.level);
    std::vector<uint32_t> owned;
    for (uint32_t lane = index; lane < c_.inference_threads; lane += c_.sender_threads) owned.push_back(lane);
    std::vector<pollfd> sockets(owned.size()); size_t cursor = 0;
    auto heartbeat_interval = std::chrono::microseconds(std::min<uint64_t>(1000000, uint64_t(c_.timeout_s)*1000000/3));
    for (;;) {
      if (run_.failed) break;
      bool progress = false, all_done = true;
      for (size_t i = 0; i < owned.size(); ++i) {
        auto &channel = *run_.channels[owned[i]];
        sockets[i] = {channel.fd(), short(POLLIN | (channel.pending() ? POLLOUT : 0)), 0};
      }
      auto polled = poll(sockets.data(), sockets.size(), 0);
      require(polled >= 0 || errno == EINTR, "sender poll failed");
      for (size_t i = 0; i < owned.size(); ++i) {
        auto id = owned[i]; auto &lane = lanes_[id]; auto &channel = *run_.channels[id];
        if (lane.done) continue;
        all_done = false;
        if (sockets[i].revents & POLLIN) {
          auto done = channel.receive(); expect(done, Type::done, session_);
          require(lane.ended, "backend DONE before channel END");
          Reader r(done.body);
          require(r.u64() == lane.records, "backend received record count mismatch");
          auto submitted = r.u64(), filtered = r.u64(), predictions = r.u64(); r.finish();
          require(submitted == lane.records && submitted == filtered+predictions, "backend inference counts mismatch");
          metrics_.submitted += submitted; metrics_.filtered += filtered; metrics_.predictions += predictions;
          mark_last(metrics_.completed, now_ns());
          lane.done = true; progress = true; continue;
        }
        require(!(sockets[i].revents & (POLLERR|POLLHUP|POLLNVAL)), "TCP channel closed before DONE");
        if (channel.pending()) {
          auto start = now_ns(); progress |= channel.advance_send(); metrics_.send_ns += now_ns()-start;
        }
        if (lane.ended) require(Clock::now() < lane.ack_deadline, "backend DONE timeout");
        if (channel.pending() || lane.ended) continue;
        bool drained = outputs_[index]->drained();
        if (lane.count && (drained || lane.count >= c_.compression_records || Clock::now() >= lane.deadline)) {
          flush_lane(id, codec); lane.heartbeat = Clock::now()+heartbeat_interval; progress = true;
        } else if (drained) {
          Writer end; end.u64(lane.records); end.u64(lane.batches); end.u32(id == 0 ? 1 : 0);
          if (id == 0) write_source(end, source_stats());
          Message message; message.type = Type::end; message.run = session_; message.body = std::move(end.data);
          channel.start_send(message); lane.ended = true; progress = true;
          lane.ack_deadline = Clock::now()+std::chrono::seconds(c_.timeout_s);
        } else if (Clock::now() >= lane.heartbeat) {
          Message heartbeat; heartbeat.type = Type::progress; heartbeat.run = session_;
          channel.start_send(heartbeat); lane.heartbeat = Clock::now()+heartbeat_interval; progress = true;
        }
      }
      if (all_done) break;
      // Choose a writable channel before removing an output record. A stalled
      // connection retains only its own batch; other channels keep advancing.
      for (size_t step = 0; step < owned.size(); ++step) {
        auto position = (cursor+step)%owned.size(); auto id = owned[position]; auto &lane = lanes_[id];
        if (lane.ended || run_.channels[id]->pending() || lane.count >= c_.compression_records) continue;
        Record record;
        if (outputs_[index]->try_pop(record)) {
          encode(id, record); cursor = (position+1)%owned.size(); progress = true;
        }
        break;
      }
      if (!progress) {
        // Poll has a bounded sleep so queue arrivals and shutdown remain prompt.
        for (size_t i = 0; i < owned.size(); ++i)
          sockets[i].events = short(POLLIN | (run_.channels[owned[i]]->pending() ? POLLOUT : 0));
        poll(sockets.data(), sockets.size(), 1);
      }
    }
    metrics_.output_cpu_ns += thread_cpu_ns()-cpu;
    if (!run_.failed) {
      std::printf("TCP sender drained: sender=%u, channels=%zu, output_peak=%zu, output_full_waits=%llu\n", index,
                  owned.size(), outputs_[index]->peak(), (unsigned long long)outputs_[index]->full_waits());
      for (auto id : owned)
        std::printf("TCP frontend drained: channel=%u, records=%llu, batches=%llu\n", id,
                    (unsigned long long)lanes_[id].records, (unsigned long long)lanes_[id].batches);
    }
  }
  void print_capture_stats() {
    ktag_dpdk_capture_stats stats{}; stats.struct_size = sizeof(stats); stats.api_version = KTAG_API_VERSION;
    require(ktag_dpdk_capture_get_stats(capture_.get(), &stats) == KTAG_SUCCESS, "DPDK statistics failed");
    metrics_.capture_received = stats.received; metrics_.capture_delivered = stats.delivered;
    metrics_.capture_pending = stats.pending; metrics_.capture_drops = stats.dropped;
    std::printf("DPDK capture: received=%llu, delivered=%llu, pending=%llu, missed/errors=%llu\n",
                (unsigned long long)stats.received, (unsigned long long)stats.delivered,
                (unsigned long long)stats.pending, (unsigned long long)stats.dropped);
    for (uint32_t i = 0; i < c_.rx_queues; ++i) {
      ktag_dpdk_queue_stats queue{}; queue.struct_size = sizeof(queue); queue.api_version = KTAG_API_VERSION;
      require(ktag_dpdk_capture_get_queue_stats(capture_.get(), i, &queue) == KTAG_SUCCESS, "DPDK queue statistics failed");
      std::printf("DPDK RX queue: queue=%u, received=%llu, delivered=%llu, pending=%llu\n", i,
                  (unsigned long long)queue.received, (unsigned long long)queue.delivered, (unsigned long long)queue.pending);
    }
  }

  const Config &c_; Metrics &metrics_; cpu_set_t allowed_;
  Schema schema_; Registry registry_; Domain domain_;
  std::vector<Feature> contexts_;
  std::unique_ptr<void, decltype(&ktag_dpdk_capture_close)> capture_;
  std::vector<std::unique_ptr<Queue<Packet>>> packets_;
  std::vector<std::unique_ptr<Queue<Record>>> outputs_;
  std::vector<std::unique_ptr<RxCounters>> rx_stats_;
  std::vector<std::unique_ptr<FeatureCounters>> feature_stats_;
  std::atomic<uint32_t> active_rx_, active_features_, rx_ready_{0};
  std::atomic<bool> go_{false}, stop_capture_{false};
  uint64_t session_ = 0;
  uint64_t rules_hash_;
  std::vector<TxLane> lanes_;
  OnlineRun run_;
};
#endif

struct BackendLane {
  std::vector<ktag_prediction_record> results;
  uint64_t records = 0, batches = 0, submitted = 0, filtered = 0, predictions = 0;
};

class OnlineBackend {
 public:
  OnlineBackend(const Config &c, Metrics &metrics)
      : c_(c), metrics_(metrics), allowed_(allowed_cpus()), schema_(make_schema()), lanes_(c.inference_threads), rules_hash_(c.rules_hash()) {
    check_cpus(c.inference_cpus, allowed_);
    metrics_.schema_hash = ktag_feature_schema_get_hash(schema_.get());
  }
  void execute() {
    // Open the result before accepting traffic, so an invalid path fails early.
    std::ofstream result(c_.result, std::ios::trunc);
    require(bool(result), "cannot open prediction output: "+c_.result);
    Listener listener(c_);
    std::cout << "Online backend listening " << c_.local_ip << ':' << c_.port
              << " channels=" << c_.inference_threads << std::endl;
    run_.channels.resize(c_.inference_threads);
    for (uint32_t i = 0; i < c_.inference_threads; ++i) {
      auto channel = std::make_unique<Channel>(listener.accept(c_.timeout_s), c_.timeout_s, c_.max_body, metrics_, false);
      auto message = channel->receive();
      uint32_t lane = 0;
      auto session = validate_hello(message, c_, metrics_.schema_hash, lane);
      if (!i) session_ = session;
      require(session == session_, "connections belong to different frontend runs");
      require(!run_.channels[lane], "duplicate online channel ID: "+std::to_string(lane));
      // Accept order is a TCP connection property, not the logical worker ID.
      // Use the explicit HELLO ID to retain frontend/backend ownership.
      run_.channels[lane] = std::move(channel);
      std::printf("TCP backend accepted: accept_index=%u, channel=%u, backend_worker=%u\n", i, lane, lane);
    }
    for (uint32_t i = 0; i < c_.inference_threads; ++i)
      run_.threads.emplace_back([this, i] {
        run_.guard("backend worker="+std::to_string(i), [&] { worker(i); });
        ++finished_; run_.changed.notify_all();
      });
    while (finished_ != c_.inference_threads && !run_.failed) {
      if (interrupted) run_.fail("backend interrupted");
      std::unique_lock<std::mutex> lock(run_.mutex);
      run_.changed.wait_for(lock, std::chrono::milliseconds(50));
    }
    run_.join(); run_.check(); metrics_.completed = now_ns();
    summary_.submitted = metrics_.submitted; summary_.filtered = metrics_.filtered;
    summary_.predictions = metrics_.predictions;
    summary_.elapsed_ns = metrics_.completed-metrics_.first_read;
    metrics_.sources.push_back(summary_);
    metrics_.packets = summary_.packets; metrics_.bytes = summary_.bytes; metrics_.original_bytes = summary_.original_bytes;
    metrics_.unsupported = summary_.unsupported; metrics_.processed = summary_.processed; metrics_.rejected = summary_.rejected;
    auto start = now_ns();
    result << "worker,flow_id,ip_version,src_ip,dst_ip,src_port,dst_port,protocol,first_packet_sequence,last_packet_sequence,label,confidence\n";
    result << std::setprecision(9);
    auto address = [](const ktag_ip_address &a) {
      char buffer[64]{};
      require(inet_ntop(a.version == 4 ? AF_INET : AF_INET6, a.bytes, buffer, sizeof(buffer)) != nullptr, "invalid prediction address");
      return std::string(buffer);
    };
    for (uint32_t i = 0; i < lanes_.size(); ++i) {
      auto &lane = lanes_[i];
      for (const auto &p : lane.results) {
        const auto &k = p.metadata.flow_key;
        result << i << ',' << p.metadata.flow_id << ',' << unsigned(k.address_a.version) << ',' << address(k.address_a)
               << ',' << address(k.address_b) << ',' << k.port_a << ',' << k.port_b << ',' << unsigned(k.protocol) << ','
               << p.metadata.first_packet_sequence << ',' << p.metadata.last_packet_sequence << ',' << p.label << ',' << p.confidence << '\n';
      }
      std::printf("TCP backend drained: worker=%u, records=%llu, batches=%llu, submitted=%llu, predictions=%llu, filtered=%llu\n", i,
                  (unsigned long long)lane.records, (unsigned long long)lane.batches, (unsigned long long)lane.submitted,
                  (unsigned long long)lane.predictions, (unsigned long long)lane.filtered);
    }
    result.flush(); require(bool(result), "prediction output write failed: "+c_.result);
    metrics_.output_ns = now_ns()-start;
  }

 private:
  void worker(uint32_t index) {
    bind_worker("infer", index, c_.inference_cpus, allowed_);
    ktag_inference_config ic{}; ic.struct_size = sizeof(ic); ic.api_version = KTAG_API_VERSION;
    ic.model_package_path = c_.model.c_str(); ic.feature_schema = schema_.get(); ic.intra_op_threads = 1; ic.inter_op_threads = 1;
    Inference inference(ktag_inference_create(&ic), ktag_inference_destroy);
    require(bool(inference), "model load: "+std::string(ktag_inference_get_last_error()));
    FeatureBatch input(ktag_feature_batch_create(schema_.get(), c_.inference_records), ktag_feature_batch_destroy);
    Predictions output(ktag_prediction_batch_create(c_.inference_records), ktag_prediction_batch_destroy);
    require(bool(input) && bool(output), "cannot allocate inference batches");
    auto &channel = *run_.channels[index]; auto &lane = lanes_[index]; Codec codec(c_.level);
    Writer ready; ready.u32(index); channel.control(Type::ready, session_, ready);
    if (ready_.fetch_add(1) == c_.inference_threads-1) {
      metrics_.initialization_ns = now_ns()-metrics_.started; metrics_.first_read = now_ns();
      std::cout << "Online backend ready: workers=" << c_.inference_threads << std::endl;
    }
    auto cpu = thread_cpu_ns(); Clock::time_point batch_started{};
    auto flush = [&] {
      auto count = ktag_feature_batch_get_count(input.get());
      if (!count) return;
      auto start = now_ns(); auto cpu_start = thread_cpu_ns();
      auto status = ktag_inference_predict(inference.get(), input.get(), output.get());
      metrics_.inference_ns += now_ns()-start; metrics_.inference_cpu_ns += thread_cpu_ns()-cpu_start;
      mark_last(metrics_.inference_completed, now_ns());
      require(status == KTAG_SUCCESS, "inference: "+std::string(ktag_inference_get_last_error()));
      auto count_out = ktag_prediction_batch_get_count(output.get());
      require(count_out <= count, "too many predictions");
      auto previous = retained_.fetch_add(count_out);
      require(previous+count_out <= c_.prediction_limit, "prediction buffer exceeds global prediction_limit");
      for (uint32_t i = 0; i < count_out; ++i) {
        ktag_prediction_record p{};
        require(ktag_prediction_batch_get_record(output.get(), i, &p) == KTAG_SUCCESS && p.source_record_index < count,
                "invalid prediction record");
        lane.results.push_back(p);
      }
      lane.submitted += count; lane.predictions += count_out; lane.filtered += count-count_out;
      ktag_feature_batch_clear(input.get()); batch_started = {};
    };
    const auto frame_size = ktag_demo_ub_feature_frame_size(KTAG_LEGACY_V1_VALUE_COUNT);
    auto last_progress = Clock::now(); bool ended = false;
    while (!run_.failed && !ended) {
      if (ktag_feature_batch_get_count(input.get()) && Clock::now()-batch_started >= std::chrono::microseconds(c_.inference_timeout_us)) flush();
      Message message;
      if (!channel.receive_for(message, c_.inference_timeout_us)) {
        require(Clock::now()-last_progress < std::chrono::seconds(c_.timeout_s), "online feature progress timeout");
        continue;
      }
      last_progress = Clock::now();
      if (message.type == Type::progress) {
        expect(message, Type::progress, session_); Reader r(message.body); r.finish();
      } else if (message.type == Type::features) {
        expect(message, Type::features, session_);
        require(message.sequence == lane.batches+1 && message.count <= c_.compression_records &&
                uint64_t(message.count)*frame_size == message.raw_size, "online feature batch sequence/size mismatch");
        metrics_.mark_first(now_ns());
        auto start = now_ns(); auto raw = codec.decompress(message, c_.max_body); metrics_.decompress_ns += now_ns()-start;
        for (uint32_t i = 0; i < message.count; ++i) {
          Record record; ktag_demo_ub_frame_info info{}; std::string error; start = now_ns();
          require(ktag_demo_ub_decode_feature_frame(raw.data()+i*frame_size, frame_size, &info, &record, &error),
                  "feature decode: "+error);
          require(info.frame_size == frame_size && info.record_count == 1 && info.value_count == KTAG_LEGACY_V1_VALUE_COUNT &&
                  info.flags == 0 && info.contract.frontend_id_hash == 1 && info.contract.epoch == session_ &&
                  info.contract.sequence == lane.records+1 && info.contract.schema_hash == metrics_.schema_hash &&
                  info.contract.ruleset_hash == rules_hash_ && record.source_index == 0, "online feature contract mismatch");
          metrics_.decode_ns += now_ns()-start;
          if (!ktag_feature_batch_get_count(input.get())) batch_started = Clock::now();
          require(ktag_feature_batch_append(input.get(), &record.details.metadata, record.values.data(), uint32_t(record.values.size())) == KTAG_SUCCESS,
                  "cannot append inference input");
          ++lane.records;
          if (ktag_feature_batch_get_count(input.get()) >= c_.inference_records ||
              Clock::now()-batch_started >= std::chrono::microseconds(c_.inference_timeout_us)) flush();
        }
        ++lane.batches; metrics_.raw_bytes += raw.size(); metrics_.compressed_bytes += message.body.size();
      } else {
        expect(message, Type::end, session_); Reader r(message.body);
        require(r.u64() == lane.records && r.u64() == lane.batches, "online END counters mismatch");
        auto summary = r.u32(); require(summary == (index == 0 ? 1U : 0U), "summary on wrong channel");
        if (summary) { summary_ = read_source(r); summary_.name = "DPDK online frontend"; }
        r.finish(); flush(); ended = true;
      }
    }
    if (run_.failed) return;
    require(lane.records == lane.submitted && lane.submitted == lane.filtered+lane.predictions, "online worker counts mismatch");
    metrics_.records += lane.records; metrics_.batches += lane.batches; metrics_.submitted += lane.submitted;
    metrics_.filtered += lane.filtered; metrics_.predictions += lane.predictions;
    metrics_.backend_data_cpu_ns += thread_cpu_ns()-cpu;
    // Every worker finishes inference before any worker acknowledges success.
    if (arrived_.fetch_add(1) == c_.inference_threads-1) {
      require(summary_.records == metrics_.records && metrics_.records == metrics_.submitted,
              "online global feature/inference totals mismatch");
      drained_ = true; run_.changed.notify_all();
    }
    {
      std::unique_lock<std::mutex> lock(run_.mutex);
      while (!drained_ && !run_.failed) run_.changed.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (run_.failed) return;
    Writer done; done.u64(lane.records); done.u64(lane.submitted); done.u64(lane.filtered); done.u64(lane.predictions);
    channel.control(Type::done, session_, done);
  }

  const Config &c_; Metrics &metrics_; cpu_set_t allowed_; Schema schema_;
  std::vector<BackendLane> lanes_; SourceStats summary_;
  uint64_t session_ = 0;
  uint64_t rules_hash_;
  std::atomic<uint32_t> ready_{0}, arrived_{0}, finished_{0};
  std::atomic<uint64_t> retained_{0};
  std::atomic<bool> drained_{false};
  OnlineRun run_;
};
} // namespace

void run_online_frontend(const Config &c, Metrics &metrics) {
#ifdef KTAG_TCP_BENCH_HAVE_DPDK
  OnlineFrontend pipeline(c, metrics); pipeline.execute();
#else
  (void)c; (void)metrics;
  throw std::runtime_error("online frontend requires a build with KTAG_ENABLE_DPDK=ON");
#endif
}
void run_online_backend(const Config &c, Metrics &metrics) {
  OnlineBackend pipeline(c, metrics); pipeline.execute();
}
} // namespace tcp_bench
