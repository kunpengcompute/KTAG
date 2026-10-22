/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include "latency_benchmark.h"
#include "latency_benchmark_internal.h"
#include "bounded_queue.h"
#include "demo_capture_source.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using namespace ktag_demo_latency;
template<class T, void (*Destroy)(T *)> using owner = std::unique_ptr<T, decltype(Destroy)>;
using feature_ptr = owner<ktag_feature_context, ktag_feature_destroy>;
using input_ptr = owner<ktag_feature_batch, ktag_feature_batch_destroy>;
using inference_ptr = owner<ktag_inference_context, ktag_inference_destroy>;
using output_ptr = owner<ktag_prediction_batch, ktag_prediction_batch_destroy>;
void check(ktag_status status, const char *operation) {
  if (status != KTAG_SUCCESS) throw std::runtime_error(std::string(operation) +
      " failed (status=" + std::to_string(status) + "): " + ktag_inference_get_last_error());
}
struct packet {
  ktag_packet value{};
  uint64_t arrival_ns = 0;
  std::string key;
  packet() { ktag_packet_init(&value); }
  ~packet() { ktag_packet_release(&value); }
  packet(const packet &) = delete;
  packet &operator=(const packet &) = delete;
  packet(packet &&other) noexcept : value(other.value), arrival_ns(other.arrival_ns),
      key(std::move(other.key)) { ktag_packet_init(&other.value); }
  packet &operator=(packet &&other) noexcept {
    if (this != &other) { ktag_packet_release(&value); value = other.value;
      arrival_ns = other.arrival_ns; key = std::move(other.key); ktag_packet_init(&other.value); }
    return *this;
  }
};
class feature_worker {
  feature_ptr context_;
  input_ptr output_;
  packet_history history_;
  uint64_t last_expire_us_ = 0;
  uint32_t source_, worker_;
  const ktag_demo_pipeline_config &config_;
  bool pure_;
public:
  uint64_t emitted = 0, matched_packets = 0;
  std::vector<flow> corpus;
  feature_worker(const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
                 uint32_t source, uint32_t worker, ktag_feature_context *context)
      : context_(context, ktag_feature_destroy),
        output_(ktag_feature_batch_create(schema, 64), ktag_feature_batch_destroy),
        source_(source), worker_(worker), config_(config), pure_(config.latency.mode == "inference") {
    if (!context_ || !output_) throw std::runtime_error("cannot create latency Feature Context/Batch");
  }
  using sink = std::function<void(flow &&)>;
  void collect(bool eof, uint64_t ready, const sink &emit) {
    for (uint32_t i = 0; i < ktag_feature_batch_get_count(output_.get()); ++i) {
      ktag_feature_record record{};
      flow item; item.source = source_; item.feature_worker = worker_;
      check(ktag_feature_batch_get_record(output_.get(), i, &record), "read feature");
      check(ktag_feature_batch_get_record_details(output_.get(), i, &item.details), "read details");
      item.values.assign(record.values, record.values + record.value_count);
      item.packets = history_.take(record.metadata);
      item.packet_count = item.packets.size(); item.ready_ns = ready; item.eof_feature = eof;
      ++emitted; matched_packets += item.packet_count;
      if (pure_) { item.packets.clear(); corpus.push_back(std::move(item)); }
      else emit(std::move(item));
    }
  }
  void process(const packet &item, const sink &emit) {
    history_.add(item.key, {item.value.sequence, item.arrival_ns});
    const auto status = ktag_feature_process_packet(context_.get(), &item.value, output_.get());
    if (status == KTAG_ERROR_NOT_SUPPORTED || status == KTAG_ERROR_INVALID_ARGUMENT) return;
    check(status, "feature process"); collect(false, now_ns(), emit);
    const uint64_t timestamp = static_cast<uint64_t>(item.value.timestamp.seconds) * 1000000ULL +
        static_cast<uint32_t>(item.value.timestamp.microseconds);
    if (config_.expire_interval_ms && timestamp >= last_expire_us_ &&
        timestamp - last_expire_us_ >= static_cast<uint64_t>(config_.expire_interval_ms) * 1000ULL) {
      check(ktag_feature_expire(context_.get(), timestamp, output_.get()), "feature expire");
      last_expire_us_ = timestamp; collect(false, now_ns(), emit);
    }
  }
  void flush(const sink &emit) {
    check(ktag_feature_flush(context_.get(), output_.get()), "feature flush");
    collect(true, now_ns(), emit);
  }
};
class infer_worker {
  inference_ptr context_;
  input_ptr input_;
  output_ptr output_;
  uint32_t worker_;
  uint64_t next_batch_ = 0;
  bool model_called_ = false;
public:
  std::vector<batch_result> results;
  infer_worker(const ktag_feature_schema *schema, uint32_t worker, ktag_inference_context *context,
               uint32_t capacity) : context_(context, ktag_inference_destroy),
      input_(ktag_feature_batch_create(schema, capacity), ktag_feature_batch_destroy),
      output_(ktag_prediction_batch_create(capacity), ktag_prediction_batch_destroy), worker_(worker) {
    if (!context_) throw std::runtime_error(std::string("cannot create latency Inference Context: ") +
        ktag_inference_get_last_error());
    if (!input_ || !output_) throw std::runtime_error("cannot allocate latency inference batches");
  }
  void build(const std::vector<flow> &items) {
    ktag_feature_batch_clear(input_.get());
    for (const auto &item : items) check(ktag_feature_batch_append(input_.get(), &item.details.metadata,
        item.values.data(), static_cast<uint32_t>(item.values.size())), "append inference input");
  }
  void predict(std::vector<flow> items, const std::string &reason, bool measured = true,
               bool retain_flows = true) {
    build(items);
    batch_result result; result.worker = worker_; result.reason = reason;
    result.batch = next_batch_;
    result.source = items.empty() ? 0 : items.front().source;
    result.timing.struct_size = sizeof(result.timing); result.timing.api_version = KTAG_API_VERSION;
    result.start_ns = now_ns();
    check(ktag_inference_predict_with_timing(context_.get(), input_.get(), output_.get(),
        &result.timing), "timed inference");
    result.finish_ns = now_ns(); // Prediction Batch is now ready; report work is outside this span.
    ++next_batch_;
    if (result.timing.model_record_count) {
      result.first_call = !model_called_;
      model_called_ = true;
    }
    for (uint32_t i = 0; i < ktag_prediction_batch_get_count(output_.get()); ++i) {
      ktag_prediction_record record{};
      check(ktag_prediction_batch_get_record(output_.get(), i, &record), "read prediction");
      if (record.source_record_index >= items.size()) throw std::runtime_error("invalid prediction source index");
      auto &item = items[record.source_record_index];
      if (item.predicted) throw std::runtime_error("duplicate prediction source index");
      item.predicted = true; item.label = record.label; item.confidence = record.confidence;
      result.packet_count += item.packet_count;
    }
    if (!measured) return;
    if (!retain_flows && result.timing.model_record_count != items.size())
      throw std::runtime_error("pure benchmark unexpectedly filtered an eligible record");
    if (retain_flows) {
      for (auto &item : items) std::vector<float>().swap(item.values);
      result.flows = std::move(items);
    }
    results.push_back(std::move(result));
  }
  std::vector<flow> eligible(std::vector<flow> items) {
    build(items); check(ktag_inference_predict(context_.get(), input_.get(), output_.get()), "sample eligibility");
    std::vector<flow> result;
    for (uint32_t i = 0; i < ktag_prediction_batch_get_count(output_.get()); ++i) {
      ktag_prediction_record record{}; check(ktag_prediction_batch_get_record(output_.get(), i, &record), "eligibility record");
      if (record.source_record_index >= items.size()) throw std::runtime_error("invalid eligibility source index");
      result.push_back(std::move(items[record.source_record_index]));
    }
    return result;
  }
};
struct thread_joiner {
  std::vector<std::thread> threads;
  void join() { for (auto &t : threads) if (t.joinable()) t.join(); }
  ~thread_joiner() { join(); }
};
using packet_queue = ktag_demo_bounded_queue<packet>;
using flow_queue = ktag_demo_bounded_queue<flow>;
const auto blocking = ktag_demo_backpressure_policy::block;

source_result run_source(const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
    const char *path, uint32_t source, const ktag_demo_latency_feature_factory &make_feature,
    const ktag_demo_latency_inference_factory &make_inference, const volatile sig_atomic_t *stop,
    std::vector<flow> *corpus, std::vector<batch_result> *results) {
  const bool serial = config.mode == ktag_demo_pipeline_mode::single_thread;
  const bool pure = config.latency.mode == "inference";
  const uint32_t nf = serial ? 1 : config.feature_threads;
  const uint32_t ni = pure ? 0 : (serial ? 1 : config.inference_threads);
  std::string error;
  auto capture = create_demo_capture_source(config, path, false, !serial, &error);
  if (!capture) throw std::runtime_error("capture open: " + error);
  ktag_domain_store_config dc{}; dc.struct_size = sizeof(dc); dc.api_version = KTAG_API_VERSION;
  owner<ktag_domain_store, ktag_domain_store_destroy> domain(ktag_domain_store_create(&dc), ktag_domain_store_destroy);
  if (!domain) throw std::runtime_error("cannot create latency Domain Store");
  std::vector<std::unique_ptr<feature_worker>> features;
  std::vector<std::unique_ptr<infer_worker>> infer;
  for (uint32_t i = 0; i < nf; ++i) features.emplace_back(new feature_worker(config, schema, source, i, make_feature(domain.get())));
  for (uint32_t i = 0; i < ni; ++i) infer.emplace_back(new infer_worker(schema, i, make_inference(), config.latency.batch_size));
  source_result stats;
  auto read = [&](packet *item) {
    for (;;) {
      if (*stop) throw std::runtime_error("latency benchmark interrupted; incomplete run discarded");
      const auto status = capture->next(&item->value);
      item->arrival_ns = now_ns(); // Timestamp immediately after Capture returns, before decode/queueing.
      if (status == KTAG_STATUS_END_OF_STREAM) { stats.eof_ns = item->arrival_ns; return false; }
      if (status == KTAG_STATUS_TIMEOUT || status == KTAG_ERROR_CAPACITY) { std::this_thread::yield(); continue; }
      if (status != KTAG_SUCCESS) throw std::runtime_error(std::string("capture read: ") + capture->last_error());
      ++stats.captured; stats.bytes += item->value.captured_length; stats.last_packet_ns = item->arrival_ns;
      ktag_flow_key key{};
      if (ktag_packet_get_flow_key(&item->value, &key) != KTAG_SUCCESS) { ktag_packet_release(&item->value); continue; }
      item->key = key_text(key); return true;
    }
  };
  stats.start_ns = now_ns();
  if (serial) {
    std::vector<flow> pending;
    pending.reserve(config.latency.batch_size);
    uint64_t pending_started = 0;
    auto flush = [&](const char *reason) {
      if (!pending.empty()) { infer[0]->predict(std::move(pending), reason); pending.clear(); pending.reserve(config.latency.batch_size); }
    };
    auto emit = [&](flow &&item) {
      if (!pending.empty() && now_ns() - pending_started >=
          static_cast<uint64_t>(config.inference_batch_timeout_us) * 1000ULL) flush("timeout");
      if (pending.empty()) pending_started = now_ns();
      pending.push_back(std::move(item));
      if (pending.size() == config.latency.batch_size) flush("full");
    };
    packet item;
    while (read(&item)) {
      features[0]->process(item, emit);
      if (!pure && !pending.empty() && now_ns() - pending_started >=
          static_cast<uint64_t>(config.inference_batch_timeout_us) * 1000ULL) flush("timeout");
      ktag_packet_release(&item.value);
    }
    features[0]->flush(emit);
    if (!pure) flush("eof");
  } else {
    std::vector<std::unique_ptr<packet_queue>> queues;
    for (uint32_t i = 0; i < nf; ++i) queues.emplace_back(new packet_queue(config.packet_queue_capacity));
    flow_queue outputs(config.feature_queue_capacity);
    std::atomic<uint32_t> active(nf);
    std::atomic<bool> failed(false);
    std::mutex error_mutex;
    std::string failure;
    auto fail = [&](const std::string &message) {
      { std::lock_guard<std::mutex> lock(error_mutex); if (failure.empty()) failure = message; }
      failed = true; for (auto &q : queues) q->close(); outputs.close();
    };
    // Declared last: all threads join before their queues, state or contexts are destroyed.
    thread_joiner group;
    try {
      for (uint32_t i = 0; i < ni; ++i) group.threads.emplace_back([&, i] {
        try {
          flow first;
          while (!failed && outputs.pop(&first)) {
            std::vector<flow> pending; pending.reserve(config.latency.batch_size);
            const auto deadline = clock::now() +
                std::chrono::microseconds(config.inference_batch_timeout_us);
            pending.push_back(std::move(first));
            const char *reason = "full";
            while (pending.size() < config.latency.batch_size && !failed) {
              if (clock::now() >= deadline) { reason = "timeout"; break; }
              flow next;
              if (!outputs.pop_until(&next, deadline)) {
                reason = outputs.statistics().closed ? "eof" : "timeout"; break;
              }
              pending.push_back(std::move(next));
            }
            if (!failed) infer[i]->predict(std::move(pending), reason);
          }
        } catch (const std::exception &e) { fail(e.what()); }
      });
      for (uint32_t i = 0; i < nf; ++i) group.threads.emplace_back([&, i] {
        try {
          auto emit = [&](flow &&item) {
            if (outputs.push(std::move(item), blocking) != ktag_demo_queue_push_result::pushed && !failed)
              throw std::runtime_error("latency feature queue closed unexpectedly");
          };
          packet item;
          while (!failed && queues[i]->pop(&item)) { features[i]->process(item, emit); ktag_packet_release(&item.value); }
          if (!failed) features[i]->flush(emit);
        } catch (const std::exception &e) { fail(e.what()); }
        if (active.fetch_sub(1) == 1) outputs.close();
      });
      packet item;
      while (!failed && read(&item)) {
        ktag_flow_key key{}; check(ktag_packet_get_flow_key(&item.value, &key), "dispatch key");
        const uint32_t i = ktag_flow_select_worker(&key, nf, config.flow_hash_seed);
        if (i >= nf) throw std::runtime_error("invalid latency worker selection");
        if (queues[i]->push(std::move(item), blocking) != ktag_demo_queue_push_result::pushed && !failed)
          throw std::runtime_error("latency packet queue closed unexpectedly");
      }
      for (auto &q : queues) q->close();
    } catch (const std::exception &e) { fail(e.what()); }
    group.join();
    if (failed) throw std::runtime_error(failure);
  }
  stats.finish_ns = now_ns();
  for (auto &worker : features) {
    stats.emitted += worker->emitted; stats.matched_packets += worker->matched_packets;
    for (auto &item : worker->corpus) corpus->push_back(std::move(item));
  }
  for (auto &worker : infer) for (auto &item : worker->results) results->push_back(std::move(item));
  return stats;
}

uint64_t benchmark_inference(const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
    std::vector<flow> corpus, const ktag_demo_latency_inference_factory &make_inference,
    const volatile sig_atomic_t *stop, std::vector<batch_result> *results) {
  const uint32_t count = config.mode == ktag_demo_pipeline_mode::single_thread ? 1 : config.inference_threads;
  // Keep the pure-inference corpus order independent of Feature worker sharding.
  std::sort(corpus.begin(), corpus.end(), [](const flow &a, const flow &b) {
    if (a.source != b.source) return a.source < b.source;
    if (a.details.metadata.first_packet_sequence != b.details.metadata.first_packet_sequence)
      return a.details.metadata.first_packet_sequence < b.details.metadata.first_packet_sequence;
    return a.details.metadata.last_packet_sequence < b.details.metadata.last_packet_sequence;
  });
  // Reject/filter records before timing, using the exact package preprocessing rules.
  std::vector<flow> eligible;
  {
    infer_worker validation(schema, 0, make_inference(), config.latency.batch_size);
    for (size_t offset = 0; offset < corpus.size(); offset += config.latency.batch_size) {
      const auto end = std::min(corpus.size(), offset + config.latency.batch_size);
      std::vector<flow> items;
      for (size_t i = offset; i < end; ++i) items.push_back(std::move(corpus[i]));
      auto accepted = validation.eligible(std::move(items));
      for (auto &item : accepted) eligible.push_back(std::move(item));
      if (*stop) throw std::runtime_error("latency benchmark interrupted during preparation");
    }
  }
  if (eligible.empty()) throw std::runtime_error("no eligible flows: check PCAP, lifecycle and model minimum_total_packets");
  std::printf("Pure inference: eligible_flows=%zu, warmup=%u, iterations/worker=%u, workers=%u\n",
      eligible.size(), config.latency.warmup, config.latency.iterations, count);
  std::mutex gate_mutex;
  std::condition_variable gate;
  uint32_t ready = 0;
  bool go = false;
  std::atomic<bool> failed(false);
  std::string failure;
  std::vector<std::vector<batch_result>> local(count);
  std::vector<uint64_t> worker_finished(count, 0);
  thread_joiner group;
  uint64_t start = 0, finish = 0;
  auto fail = [&](const std::string &message) {
    std::lock_guard<std::mutex> lock(gate_mutex);
    if (failure.empty()) failure = message;
    failed = true; gate.notify_all();
  };
  auto work = [&](uint32_t worker, bool synchronize) {
    try {
      infer_worker engine(schema, worker, make_inference(), config.latency.batch_size);
      auto make_items = [&](uint64_t step) {
        std::vector<flow> items; items.reserve(config.latency.batch_size);
        for (uint32_t j = 0; j < config.latency.batch_size; ++j)
          items.push_back(eligible[(static_cast<uint64_t>(worker) * config.latency.batch_size +
              step * config.latency.batch_size + j) % eligible.size()]);
        return items;
      };
      for (uint32_t i = 0; i < config.latency.warmup; ++i) {
        if (*stop || failed) throw std::runtime_error("warmup interrupted");
        engine.predict(make_items(i), "warmup", false, false);
      }
      if (synchronize) {
        std::unique_lock<std::mutex> lock(gate_mutex); ++ready; gate.notify_all();
        gate.wait(lock, [&] { return go || failed; });
      } else start = now_ns();
      for (uint32_t i = 0; i < config.latency.iterations && !failed; ++i) {
        if (*stop) throw std::runtime_error("measurement interrupted");
        engine.predict(make_items(static_cast<uint64_t>(config.latency.warmup) + i), "benchmark", true, false);
      }
      worker_finished[worker] = now_ns();
      local[worker] = std::move(engine.results);
    } catch (const std::exception &e) { fail(e.what()); }
  };
  if (count == 1 && config.mode == ktag_demo_pipeline_mode::single_thread) work(0, false);
  else {
    try {
      for (uint32_t i = 0; i < count; ++i) group.threads.emplace_back([&, i] { work(i, true); });
      { std::unique_lock<std::mutex> lock(gate_mutex);
        gate.wait(lock, [&] { return ready == count || failed; }); start = now_ns(); go = true; gate.notify_all(); }
    } catch (const std::exception &e) { fail(e.what()); }
    group.join();
  }
  if (failed) throw std::runtime_error(failure);
  finish = *std::max_element(worker_finished.begin(), worker_finished.end());
  for (auto &worker : local) for (auto &item : worker) results->push_back(std::move(item));
  return finish - start;
}

// Report implementation follows; no CSV writes occur in measured worker loops.

std::string quote_csv(const std::string &value) {
  std::string result = "\"";
  for (char c : value) { if (c == '"') result += '"'; result += c; }
  return result + '"';
}
std::ofstream csv(const std::string &path) {
  std::ofstream stream(path, std::ios::out | std::ios::trunc);
  if (!stream) throw std::runtime_error("cannot open latency report: " + path);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream << std::setprecision(12);
  return stream;
}
void write_reports(const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
    const std::vector<source_result> &sources, const std::vector<batch_result> &batches,
    uint64_t benchmark_wall_ns, const char **pcap_files) {
  const std::string prefix = config.latency.output_prefix;
  const bool pure = config.latency.mode == "inference";
  auto batch_csv = csv(prefix + ".batches.csv");
  batch_csv << "source_index,inference_worker,batch_id,input_flows,model_flows,associated_packets,"
      "model_ns,api_ns,submit_reason,first_model_call\n";
  std::ofstream flows_csv, packets_csv;
  if (!pure) {
    flows_csv = csv(prefix + ".flows.csv");
    flows_csv << "source_index,feature_worker,flow_id,inference_worker,batch_id,status,label,confidence,"
        "packet_count,first_sequence,last_sequence,first_arrival_ns,last_arrival_ns,feature_ready_ns,"
        "prediction_ready_ns,first_packet_to_prediction_ns,last_packet_to_prediction_ns,"
        "feature_ready_to_prediction_ns,eof_feature\n";
    if (config.latency.packet_details) {
      packets_csv = csv(prefix + ".packets.csv");
      packets_csv << "source_index,feature_worker,flow_id,packet_sequence,arrival_ns,prediction_ready_ns,latency_ns\n";
    }
  }
  std::map<std::string, distribution> metrics;
  uint64_t predicted = 0, filtered = 0, predicted_packets = 0, missing_history = 0;
  std::vector<uint64_t> last_prediction(sources.size(), 0), last_ready(sources.size(), 0);
  for (const auto &b : batches) {
    batch_csv << (pure ? -1LL : static_cast<long long>(b.source)) << ',' << b.worker << ',' << b.batch << ',' << b.timing.input_record_count << ','
        << b.timing.model_record_count << ',' << b.packet_count << ',' << b.timing.model_nanoseconds << ','
        << b.finish_ns - b.start_ns << ',' << b.reason << ',' << b.first_call << '\n';
    metrics["api_batch"].add(static_cast<double>(b.finish_ns - b.start_ns));
    if (b.timing.model_record_count) {
      const double ns = static_cast<double>(b.timing.model_nanoseconds);
      const uint64_t n = b.timing.model_record_count;
      metrics["model_batch"].add(ns);
      metrics[b.first_call ? "model_batch_first_call" : "model_batch_after_first_call"].add(ns);
      metrics["model_flow_batch_response"].add(ns, n);
      metrics["model_amortized_per_flow"].add(ns / n, n);
      if (b.packet_count) metrics["model_amortized_per_packet"].add(ns / b.packet_count, b.packet_count);
    }
    predicted += b.timing.model_record_count;
    filtered += b.timing.input_record_count - b.timing.model_record_count;
    predicted_packets += b.packet_count;
    if (pure) continue;
    for (const auto &f : b.flows) {
      const uint64_t id = f.details.metadata.flow_id;
      flows_csv << f.source << ',' << f.feature_worker << ',' << id << ',' << b.worker << ',' << b.batch << ','
          << (f.predicted ? "predicted" : "filtered") << ',';
      if (f.predicted) flows_csv << f.label << ',' << f.confidence;
      else flows_csv << ',';
      flows_csv << ',' << f.packet_count << ',' << f.details.metadata.first_packet_sequence << ','
          << f.details.metadata.last_packet_sequence << ',';
      if (f.packets.empty()) flows_csv << ',';
      else flows_csv << f.packets.front().arrival_ns << ',' << f.packets.back().arrival_ns;
      flows_csv << ',' << f.ready_ns << ',';
      if (!f.predicted) { flows_csv << ",,,," << f.eof_feature << '\n'; continue; }
      const uint64_t finished = b.finish_ns;
      flows_csv << finished << ',';
      if (f.packets.empty()) { ++missing_history; flows_csv << ','; }
      else {
        const auto first = finished - f.packets.front().arrival_ns;
        const auto last = finished - f.packets.back().arrival_ns;
        flows_csv << first << ',' << last;
        metrics["flow_first_packet_to_prediction"].add(static_cast<double>(first));
        metrics["flow_last_packet_to_prediction"].add(static_cast<double>(last));
      }
      flows_csv << ',' << finished - f.ready_ns << ',' << f.eof_feature << '\n';
      metrics["feature_ready_to_prediction"].add(static_cast<double>(finished - f.ready_ns));
      last_prediction[f.source] = std::max(last_prediction[f.source], finished);
      last_ready[f.source] = std::max(last_ready[f.source], f.ready_ns);
      for (const auto &packet : f.packets) {
        metrics["packet_to_prediction"].add(static_cast<double>(finished - packet.arrival_ns));
        if (config.latency.packet_details) packets_csv << f.source << ',' << f.feature_worker << ',' << id << ','
            << packet.sequence << ',' << packet.arrival_ns << ',' << finished << ',' << finished - packet.arrival_ns << '\n';
      }
    }
  }
  auto source_csv = csv(prefix + ".sources.csv");
  source_csv << "source_index,captured_packets,emitted_flows,associated_packets,unassociated_packets,"
      "processing_ns,capture_eof_ns,last_packet_ns,last_feature_ready_ns,last_prediction_ns,"
      "tail_after_eof_ns,tail_after_last_packet_ns,tail_after_last_feature_ns\n";
  uint64_t captured = 0, associated = 0, bytes = 0, processing = 0;
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto &s = sources[i];
    if (s.matched_packets > s.captured) throw std::runtime_error("packet/flow association double counted packets");
    captured += s.captured; associated += s.matched_packets; bytes += s.bytes; processing += s.finish_ns - s.start_ns;
    source_csv << i << ',' << s.captured << ',' << s.emitted << ',' << s.matched_packets << ','
        << s.captured - s.matched_packets << ',' << s.finish_ns - s.start_ns << ',' << s.eof_ns << ',' << s.last_packet_ns << ',';
    if (pure || !last_prediction[i]) { source_csv << ",,,,\n"; continue; }
    const uint64_t after_eof = last_prediction[i] > s.eof_ns ? last_prediction[i] - s.eof_ns : 0;
    const uint64_t after_packet = last_prediction[i] > s.last_packet_ns ? last_prediction[i] - s.last_packet_ns : 0;
    const uint64_t after_feature = last_prediction[i] - last_ready[i];
    source_csv << last_ready[i] << ',' << last_prediction[i] << ',' << after_eof << ',' << after_packet << ',' << after_feature << '\n';
    metrics["tail_after_capture_eof"].add(static_cast<double>(after_eof));
    metrics["tail_after_last_captured_packet"].add(static_cast<double>(after_packet));
    metrics["tail_after_last_predicted_feature_ready"].add(static_cast<double>(after_feature));
  }
  auto summary = csv(prefix + ".summary.csv");
  summary << "metric,unit,count,mean,p50,p95,p99,max\n";
  std::printf("\nLatency summary (microseconds; nearest-rank exact percentiles):\n");
  for (auto &entry : metrics) {
    const auto r = entry.second.summarize();
    summary << entry.first << ",us," << r.count << ',' << r.mean / 1000.0 << ',' << r.p50 / 1000.0 << ','
        << r.p95 / 1000.0 << ',' << r.p99 / 1000.0 << ',' << r.maximum / 1000.0 << '\n';
    std::printf("  %s: n=%llu mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f us\n", entry.first.c_str(),
        static_cast<unsigned long long>(r.count), r.mean / 1000, r.p50 / 1000, r.p95 / 1000, r.p99 / 1000, r.maximum / 1000);
  }
  auto metadata = csv(prefix + ".run.csv");
  metadata << "key,value\nmode," << config.latency.mode << "\npipeline_mode,"
      << ktag_demo_pipeline_mode_name(config.mode) << "\nschema," << ktag_feature_schema_get_name(schema)
      << "\nschema_hash," << ktag_feature_schema_get_hash(schema) << "\nmodel_package," << quote_csv(config.model_package_path)
      << "\nfeature_workers," << config.feature_threads << "\ninference_workers," << config.inference_threads
      << "\nintra_op_threads," << config.inference_intra_op_threads << "\ninter_op_threads," << config.inference_inter_op_threads
      << "\nbatch_size_limit," << config.latency.batch_size << "\nbatch_wait_us," << config.inference_batch_timeout_us
      << "\nwarmup_calls_per_worker," << (pure ? config.latency.warmup : 0) << "\nmeasured_calls_per_worker,"
      << (pure ? config.latency.iterations : 0) << "\nbenchmark_wall_ns," << (pure ? benchmark_wall_ns : processing)
      << "\ncaptured_packets," << captured << "\nassociated_packets," << associated
      << "\nunassociated_packets," << captured - associated << "\nmodel_output_flows," << predicted
      << "\nfiltered_input_flows," << filtered << "\npredicted_packet_observations," << predicted_packets
      << "\nflows_missing_packet_history," << missing_history << "\npacket_details," << config.latency.packet_details
      << "\nclock,steady_clock\ninput,offline_unpaced_pcap\ninitialization,excluded\nresult_file_io,excluded\n";
  for (size_t i = 0; i < sources.size(); ++i) metadata << "pcap_" << i << ',' << quote_csv(pcap_files[i]) << '\n';
  const double seconds = static_cast<double>(pure ? benchmark_wall_ns : processing) / 1e9;
  std::printf("Counters: predicted_flows=%llu filtered_flows=%llu capture_packets=%llu unassociated_packets=%llu\n",
      static_cast<unsigned long long>(predicted), static_cast<unsigned long long>(filtered),
      static_cast<unsigned long long>(captured), static_cast<unsigned long long>(captured - associated));
  if (seconds > 0) {
    std::printf("Measured throughput: %.3f predicted flows/s", predicted / seconds);
    if (!pure) std::printf(", %.3f MB/s (%.6f Gbps)", bytes / (1024.0 * 1024.0) / seconds,
        bytes * 8.0 / (1024.0 * 1024.0 * 1024.0) / seconds);
    std::printf("\n");
  }
  // Explicit close propagates write/flush errors, including a full output disk.
  batch_csv.close(); source_csv.close(); summary.close(); metadata.close();
  if (!pure) flows_csv.close();
  if (config.latency.packet_details && !pure) packets_csv.close();
  std::printf("Reports: %s.{summary,batches,sources,run}.csv%s\n", prefix.c_str(),
      pure ? "" : " plus .flows.csv (and .packets.csv when requested)");
}
} // namespace

int ktag_demo_run_latency_benchmark(const ktag_demo_pipeline_config &config,
    const ktag_feature_schema *schema, const char **pcap_files, int pcap_file_count,
    const ktag_demo_latency_feature_factory &feature_factory,
    const ktag_demo_latency_inference_factory &inference_factory,
    const volatile sig_atomic_t *interrupted) {
  try {
    if (!schema || !pcap_files || pcap_file_count <= 0 || !interrupted ||
        (config.latency.mode != "inference" && config.latency.mode != "e2e") ||
        config.latency.output_prefix.empty() || config.latency.batch_size == 0 ||
        config.latency.batch_size > 65536 || config.latency.iterations == 0 ||
        config.inference_batch_timeout_us == 0 || config.feature_threads == 0 || config.inference_threads == 0 ||
        (config.mode != ktag_demo_pipeline_mode::single_thread && config.mode != ktag_demo_pipeline_mode::full_parallel) ||
        config.ub.enabled || config.capture_backend != ktag_demo_capture_backend::pcap || config.feature_only ||
        config.backpressure != ktag_demo_backpressure_policy::block ||
        (config.mode == ktag_demo_pipeline_mode::single_thread &&
         (config.feature_threads != 1 || config.inference_threads != 1)))
      throw std::runtime_error("latency requires offline PCAP, single_thread/full_parallel, block backpressure, batch size 1..65536");
    const auto parent = std::filesystem::path(config.latency.output_prefix).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    // Refuse existing reports before an expensive run; do not mix stale and new results.
    for (const char *suffix : {".summary.csv", ".batches.csv", ".sources.csv", ".run.csv", ".flows.csv", ".packets.csv"}) {
      const auto path = config.latency.output_prefix + suffix;
      if (std::filesystem::exists(path)) throw std::runtime_error("report already exists; choose a new --latency-output prefix: " + path);
    }
    std::printf("========== Single-machine latency benchmark: %s =========="
        "\nTiming starts after Capture returns; model initialization and report file I/O are excluded.\n"
        "Batch limit=%u, wait=%u us; e2e includes first model calls (reported separately).\n",
        config.latency.mode.c_str(), config.latency.batch_size, config.inference_batch_timeout_us);
    std::vector<flow> corpus;
    std::vector<batch_result> batches;
    std::vector<source_result> sources;
    for (int i = 0; i < pcap_file_count; ++i) sources.push_back(run_source(config, schema, pcap_files[i], i,
        feature_factory, inference_factory, interrupted, &corpus, &batches));
    uint64_t wall_ns = 0;
    if (config.latency.mode == "inference") wall_ns = benchmark_inference(config, schema,
        std::move(corpus), inference_factory, interrupted, &batches);
    if (*interrupted) throw std::runtime_error("latency run interrupted");
    if (std::none_of(batches.begin(), batches.end(), [](const batch_result &batch) {
          return batch.timing.model_record_count != 0;
        })) throw std::runtime_error("no model predictions; all features were filtered or no flows were emitted");
    write_reports(config, schema, sources, batches, wall_ns, pcap_files);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "latency benchmark: %s\n", e.what());
    return -1;
  }
}
