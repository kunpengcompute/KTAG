// Fixture used by ub_queue_worker_check.py; not a separate production target.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include "pipeline_runner.h"
#include "ub_feature_wire.h"
#include "ub_ring_transport.h"
#include "prediction_batch_internal.h"

using pipeline_prediction = ktag_demo_pipeline_prediction;
using feature_record_owned = ktag_demo_schema_feature_record;
volatile sig_atomic_t pipeline_interrupted = 0;
struct feature_batch_deleter { void operator()(ktag_feature_batch *p) const { ktag_feature_batch_destroy(p); } };
struct inference_context_deleter { void operator()(ktag_inference_context *p) const { ktag_inference_destroy(p); } };
struct prediction_batch_deleter { void operator()(ktag_prediction_batch *p) const { ktag_prediction_batch_destroy(p); } };
using feature_batch_ptr = std::unique_ptr<ktag_feature_batch, feature_batch_deleter>;
using inference_context_ptr = std::unique_ptr<ktag_inference_context, inference_context_deleter>;
using prediction_batch_ptr = std::unique_ptr<ktag_prediction_batch, prediction_batch_deleter>;

struct mock_queue {
  std::mutex mutex;
  std::deque<void *> features, controls;
  std::thread::id reader, writer;
  std::atomic<int> sending_features{0}, sending_controls{0}, receiving{0};
};
struct mock_scenario {
  unsigned providers, queues;
  std::vector<std::unique_ptr<mock_queue>> channels;
  std::mutex buffers_mutex;
  std::set<void *> buffers;
  std::atomic<int> live_models{0}, created_models{0}, completed{0}, active{0}, peak{0};
  std::atomic<int> acks{0}, errors{0};
  std::atomic<bool> slow_released{false};
  int fail_cpu = -1, bind_fail_cpu = -1;
  bool fail_inference = false, corrupt = false, wrong_totals = false, filter_all = false, block_credit = false, slow_queue = false;
  mock_scenario(unsigned p, unsigned q) : providers(p), queues(q) {
    for (unsigned n = 0; n < p * q; ++n) { channels.emplace_back(new mock_queue); }
  }
  void release(void *buffer) {
    if (!buffer) { return; }
    std::lock_guard<std::mutex> lock(buffers_mutex);
    assert(buffers.erase(buffer) == 1U); // Detect double returns / lost ownership.
    delete[] static_cast<uint8_t *>(buffer);
  }
  void clear_queues() {
    for (auto &channel : channels) {
      for (void *buffer : channel->features) { release(buffer); }
      for (void *buffer : channel->controls) { release(buffer); }
      channel->features.clear(); channel->controls.clear();
    }
    assert(buffers.empty());
  }
};
mock_scenario *scenario;
thread_local uint32_t mock_cpu = 0U;
thread_local std::string last_error;
struct ktag_feature_schema {};
struct ktag_inference_context { std::thread::id owner; };

extern "C" {
uint64_t ktag_feature_schema_get_hash(const ktag_feature_schema *) { return 123; }
uint32_t ktag_feature_schema_get_value_count(const ktag_feature_schema *) { return 2; }
uint32_t ktag_feature_schema_is_frozen(const ktag_feature_schema *) { return 1; }
ktag_inference_context *ktag_inference_create(const ktag_inference_config *) {
  if (int(mock_cpu) == scenario->fail_cpu) { last_error = "injected model init failure"; return nullptr; }
  ++scenario->created_models; ++scenario->live_models;
  return new ktag_inference_context{std::this_thread::get_id()};
}
void ktag_inference_destroy(ktag_inference_context *context) { --scenario->live_models; delete context; }
const char *ktag_inference_get_last_error() { return last_error.c_str(); }
ktag_status ktag_inference_predict(ktag_inference_context *context, const ktag_feature_batch *input, ktag_prediction_batch *output) {
  assert(context->owner == std::this_thread::get_id());
  const int active = ++scenario->active;
  int peak = scenario->peak.load();
  while (active > peak && !scenario->peak.compare_exchange_weak(peak, active)) {}
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  --scenario->active;
  if (scenario->fail_inference) { last_error = "injected inference failure"; return KTAG_ERROR_INTERNAL; }
  output->records.clear();
  for (uint32_t n = 0; n < ktag_feature_batch_get_count(input); ++n) {
    ktag_feature_record record{};
    assert(ktag_feature_batch_get_record(input, n, &record) == KTAG_SUCCESS);
    ++scenario->completed;
    if (scenario->filter_all || record.metadata.flow_id % 5U == 0U) { continue; }
    ktag_prediction_record prediction{};
    prediction.struct_size = sizeof(prediction); prediction.api_version = KTAG_API_VERSION;
    prediction.source_record_index = n;
    prediction.metadata.flow_id = record.metadata.flow_id;
    prediction.metadata.flow_key = record.metadata.flow_key;
    prediction.metadata.first_packet_sequence = record.metadata.first_packet_sequence;
    prediction.metadata.last_packet_sequence = record.metadata.last_packet_sequence;
    prediction.label = record.metadata.flow_id % 7U;
    output->records.push_back(prediction);
  }
  return KTAG_SUCCESS;
}
}

bool ktag_demo_ub_allowed_cpus(std::vector<uint32_t> *cpus, std::string *) {
  for (unsigned n = 0; n < scenario->channels.size(); ++n) { cpus->push_back(n); }
  return true;
}
bool ktag_demo_ub_bind_cpu(uint32_t cpu, std::string *error) {
  mock_cpu = cpu;
  if (int(cpu) == scenario->bind_fail_cpu) { *error = "injected affinity failure"; return false; }
  return true;
}
uint32_t ktag_demo_ub_runtime_data_element_size(uint32_t payload) { return payload; }
struct ktag_demo_ub_ring_transport::implementation { ktag_demo_ub_transport_config config; };
ktag_demo_ub_ring_transport::ktag_demo_ub_ring_transport(std::unique_ptr<implementation> state) : state_(std::move(state)) {}
ktag_demo_ub_ring_transport::~ktag_demo_ub_ring_transport() = default;
std::unique_ptr<ktag_demo_ub_ring_transport> ktag_demo_ub_ring_transport::create(const ktag_demo_ub_transport_config &config, std::string *) {
  auto state = std::make_unique<implementation>(); state->config = config;
  return std::unique_ptr<ktag_demo_ub_ring_transport>(new ktag_demo_ub_ring_transport(std::move(state)));
}
bool ktag_demo_ub_ring_transport::queue_mapping(const std::string &host, uint32_t sub,
    ktag_demo_ub_queue_mapping *mapping, std::string *) const {
  const auto &nodes = state_->config.provider_nodes;
  auto it = std::find(nodes.begin(), nodes.end(), host);
  if (it == nodes.end() || sub >= scenario->queues) { return false; }
  mapping->provider_node_index = static_cast<uint32_t>(it - nodes.begin());
  mapping->rx_queue_id = mapping->tx_queue_id = mapping->provider_node_index * scenario->queues + sub;
  return true;
}
uint32_t ktag_demo_ub_ring_transport::data_element_size(uint32_t) const { return state_->config.data_element_size; }
void *ktag_demo_ub_ring_transport::allocate_buffer(uint32_t) {
  auto *buffer = new uint8_t[state_->config.data_element_size]{};
  std::lock_guard<std::mutex> lock(scenario->buffers_mutex);
  assert(scenario->buffers.insert(buffer).second);
  return buffer;
}
void ktag_demo_ub_ring_transport::free_buffer(void *buffer) { scenario->release(buffer); }
void ktag_demo_ub_ring_transport::return_buffer(void *buffer) { scenario->release(buffer); }
uint16_t ktag_demo_ub_ring_transport::send_burst(uint32_t id, void **buffers, uint16_t count) {
  auto &queue = *scenario->channels.at(id);
  const bool provider = state_->config.role == ktag_demo_ub_role::provider;
  auto &sending = provider ? queue.sending_features : queue.sending_controls;
  assert(sending.fetch_add(1) == 0); // One producer per direction, including startup handoff.
  struct guard { std::atomic<int> &n; ~guard() { --n; } } release{sending};
  assert(count == 1);
  ktag_demo_ub_frame_info info;
  std::string error;
  assert(ktag_demo_ub_inspect_frame(*buffers, state_->config.data_element_size, &info, &error));
  if (!provider && scenario->block_credit && info.message_type == ktag_demo_ub_message_type::credit && info.contract.sequence > 0) { return 0; }
  if (!provider && scenario->slow_queue && id == 0 && !scenario->slow_released &&
      info.message_type == ktag_demo_ub_message_type::credit && info.contract.sequence > 0) { return 0; }
  std::lock_guard<std::mutex> lock(queue.mutex);
  if (provider) {
    if (queue.writer == std::thread::id{}) { queue.writer = std::this_thread::get_id(); }
    assert(queue.writer == std::this_thread::get_id());
    if (scenario->corrupt && info.message_type == ktag_demo_ub_message_type::feature) {
      static_cast<uint8_t *>(*buffers)[KTAG_DEMO_UB_FRAME_HEADER_SIZE] ^= 1U;
    }
    if (scenario->wrong_totals && id % scenario->queues != 0 && info.message_type == ktag_demo_ub_message_type::drain) {
      ktag_demo_ub_stream_statistics totals;
      assert(ktag_demo_ub_decode_drain_frame(*buffers, state_->config.data_element_size, &totals, &error));
      totals.input_bytes = 999;
      size_t encoded = 0;
      assert(ktag_demo_ub_encode_drain_frame(*buffers, state_->config.data_element_size, info.contract, totals, &encoded, &error));
    }
    queue.features.push_back(*buffers);
  } else {
    if (info.message_type == ktag_demo_ub_message_type::drain_ack) { ++scenario->acks; }
    if (info.message_type == ktag_demo_ub_message_type::error) { ++scenario->errors; }
    if (info.message_type == ktag_demo_ub_message_type::drain_ack ||
        (info.message_type == ktag_demo_ub_message_type::credit && info.contract.sequence > 0)) {
      assert(queue.reader == std::this_thread::get_id());
    }
    queue.controls.push_back(*buffers);
  }
  return 1;
}
uint16_t ktag_demo_ub_ring_transport::receive_burst(uint32_t id, void **buffers, uint16_t count) {
  assert(count == 1);
  auto &queue = *scenario->channels.at(id);
  std::lock_guard<std::mutex> lock(queue.mutex);
  auto &incoming = state_->config.role == ktag_demo_ub_role::provider ? queue.controls : queue.features;
  if (incoming.empty()) { return 0; }
  *buffers = incoming.front(); incoming.pop_front();
  return 1;
}
bool ktag_demo_ub_ring_transport::receive_copy(uint32_t id, void *local, size_t capacity,
    size_t size, ktag_demo_ub_copy_receipt *receipt) {
  auto &queue = *scenario->channels.at(id);
  assert(queue.receiving.fetch_add(1) == 0);
  struct guard { std::atomic<int> &n; ~guard() { --n; } } release{queue.receiving};
  {
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.reader == std::thread::id{}) { queue.reader = std::this_thread::get_id(); }
    assert(queue.reader == std::this_thread::get_id());
  }
  void *buffer = nullptr;
  if (receive_burst(id, &buffer, 1) == 0) { return false; }
  assert(size <= capacity);
  std::memcpy(local, buffer, size);
  receipt->ub_buffer = buffer;
  receipt->received_at = std::chrono::steady_clock::now();
  return true;
}

// INSERT_ACTUAL_EXECUTORS

void run_case(unsigned providers, unsigned queues, unsigned records,
              const std::string &fault = "", uint32_t batch_timeout = 100000) {
  std::signal(SIGINT, [](int) { pipeline_interrupted = 1; });
  mock_scenario state(providers, queues);
  scenario = &state; pipeline_interrupted = 0;
  state.fail_inference = fault == "infer";
  state.fail_cpu = fault == "init" ? 0 : -1;
  state.bind_fail_cpu = fault == "bind" ? 0 : -1;
  state.corrupt = fault == "corrupt";
  state.wrong_totals = fault == "totals";
  state.filter_all = fault == "filter";
  state.block_credit = fault == "blocked";
  state.slow_queue = fault == "slow";
  ktag_demo_pipeline_config config;
  config.ub.user_nodes = {"controller"};
  for (unsigned p = 0; p < providers; ++p) { config.ub.provider_nodes.push_back("computer0" + std::to_string(p + 1)); }
  config.ub.queues_per_provider = queues; config.ub.credits_per_queue = 2;
  config.ub.data_element_size = 4096; config.ub.ruleset_hash = 456; config.ub.poll_us = 0;
  config.inference_threads = providers * queues; config.inference_batch_size = 8;
  config.inference_batch_timeout_us = batch_timeout;
  for (unsigned n = 0; n < providers * queues; ++n) { config.ub.backend_worker_cpus.push_back(n); }
  std::vector<std::thread> frontends;
  std::vector<uint8_t> frontend_ok(providers, 0);
  for (unsigned p = 0; p < providers; ++p) {
    frontends.emplace_back([&, p]() {
      auto frontend = config; frontend.ub.node_id = frontend.ub.provider_nodes[p];
      ub_frontend_record_sender sender;
      std::string error;
      bool ok = sender.initialize(frontend, 123, 2, &error);
      for (unsigned n = 1; ok && n <= records; ++n) {
        feature_record_owned record;
        record.source_index = p + 10;
        record.details.metadata.flow_id = p * 10000 + n;
        record.values = {float(n), float(p)};
        ok = sender.send(std::move(record), p == 0, &error);
      }
      if (ok) {
        pipeline_run_statistics statistics;
        statistics.packets = records * 2; statistics.bytes = records * 100;
        statistics.emitted_flows = records;
        if (records) { statistics.first_feature_at = std::chrono::steady_clock::now(); }
        ok = sender.drain(&statistics, &error);
        if (ok && records) { assert(statistics.distributed_core_nanoseconds > 0); }
      }
      if (!ok) { sender.abort_best_effort(); }
      frontend_ok[p] = ok;
    });
  }
  std::thread interrupt;
  std::thread unblock;
  if (fault == "slow") {
    unblock = std::thread([&]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (state.completed < 8 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
      assert(state.completed >= 8); // Other queue progressed while queue 0 could not return CREDIT.
      state.slow_released = true;
    });
  }
  if (fault == "blocked" || fault == "interrupt") {
    interrupt = std::thread([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      std::raise(SIGINT);
    });
  }
  std::string error;
  std::vector<pipeline_prediction> predictions;
  pipeline_run_statistics statistics;
  bool ok;
  ktag_feature_schema schema;
  {
    ub_backend_executor executor;
    assert(executor.initialize(config, &schema, &error));
    ok = executor.run(&predictions, &statistics, &error);
  }
  if (interrupt.joinable()) { interrupt.join(); }
  if (unblock.joinable()) { unblock.join(); }
  for (auto &frontend : frontends) { frontend.join(); }
  if (fault.empty() || fault == "filter" || fault == "slow") {
    if (!ok) { std::fprintf(stderr, "Unexpected failure: %s\n", error.c_str()); }
    assert(ok);
    assert(std::all_of(frontend_ok.begin(), frontend_ok.end(), [](uint8_t value) { return value; }));
    assert(state.acks == int(providers * queues));
    assert(state.created_models == int(providers * queues));
    assert(state.completed == int(records * providers));
    assert(statistics.inference_inputs == records * providers);
    assert(statistics.emitted_flows == records * providers);
    assert(statistics.bytes == records * providers * 100);
    assert(statistics.packets == records * providers * 2);
    assert(statistics.inference_nanoseconds == 0);
    assert((statistics.distributed_inference_wall_nanoseconds > 0) == (records > 0));
    std::set<uint64_t> ids;
    size_t expected = 0;
    for (unsigned p = 0; p < providers; ++p) {
      for (unsigned n = 1; n <= records; ++n) {
        if (!state.filter_all && (p * 10000 + n) % 5U) { ++expected; }
      }
    }
    assert(predictions.size() == expected);
    for (const auto &prediction : predictions) {
      const auto id = prediction.record.metadata.flow_id;
      const auto provider = id / 10000;
      assert(ids.insert(id).second);
      assert(prediction.details.metadata.flow_id == id);
      assert(prediction.source_index == provider + 10);
      assert(prediction.frontend_id_hash == ub_identity_hash(config.ub.provider_nodes.at(provider)));
      assert(prediction.ground_truth_valid == (provider == 0));
      assert(prediction.record.label == int64_t(id % 7));
    }
  } else {
    assert(!ok); assert(!error.empty());
    if (fault == "init" || fault == "bind" || fault == "infer" || fault == "corrupt") { assert(state.errors > 0); }
  }
  assert(state.live_models == 0);
  state.clear_queues();
  std::printf("PASS providers=%u queues/provider=%u records/provider=%u fault=%s timeout=%u peak=%d\n",
              providers, queues, records, fault.c_str(), batch_timeout, int(state.peak));
}

void check_graceful_frontend_stop() {
  mock_scenario state(1, 1);
  scenario = &state; pipeline_interrupted = 0;
  ktag_demo_pipeline_config config;
  config.ub.provider_nodes = {"computer01"}; config.ub.user_nodes = {"controller"};
  config.ub.node_id = "computer01"; config.ub.queues_per_provider = 1;
  config.ub.data_element_size = 4096; config.ub.ruleset_hash = 456; config.ub.poll_us = 0;
  std::string error;
  auto backend = ktag_demo_ub_ring_transport::create(make_ub_transport_config(config, ktag_demo_ub_role::user), &error);
  void *buffer = backend->allocate_buffer(0);
  ktag_demo_ub_frame_contract contract;
  contract.frontend_id_hash = ub_identity_hash("computer01"); contract.epoch = 77;
  contract.schema_hash = 123; contract.ruleset_hash = 456;
  size_t encoded = 0;
  assert(ktag_demo_ub_encode_control_frame(buffer, 4096, ktag_demo_ub_message_type::credit, contract, &encoded, &error));
  assert(backend->send_burst(0, &buffer, 1) == 1);
  {
    ub_frontend_record_sender sender;
    assert(sender.initialize(config, 123, 2, &error));
    sender.begin_input();
    // The remote peer has its own signal state in deployment. This echo peer
    // deliberately ignores the test process's frontend stop flag.
    std::thread peer([&]() {
      std::vector<uint8_t> frame(ktag_demo_ub_feature_frame_size(2));
      for (unsigned step = 0; step < 2;) {
        ktag_demo_ub_copy_receipt receipt;
        if (!backend->receive_copy(0, frame.data(), frame.size(), frame.size(), &receipt)) {
          std::this_thread::yield(); continue;
        }
        ktag_demo_ub_frame_info info;
        std::string local_error;
        assert(ktag_demo_ub_inspect_frame(frame.data(), frame.size(), &info, &local_error));
        const auto reply = step == 0 ? ktag_demo_ub_message_type::credit : ktag_demo_ub_message_type::drain_ack;
        size_t size = 0;
        assert(ktag_demo_ub_encode_control_frame(receipt.ub_buffer, 4096, reply, info.contract, &size, &local_error));
        assert(backend->send_burst(0, &receipt.ub_buffer, 1) == 1);
        ++step;
      }
    });
    feature_record_owned record;
    record.values = {1, 2}; record.details.metadata.flow_id = 1;
    pipeline_interrupted = 1;
    assert(sender.send(std::move(record), true, &error)); // Flush after first signal.
    pipeline_interrupted = 2;
    assert(!sender.send(std::move(record), true, &error)); // Second signal cancels waits.
    pipeline_interrupted = 1;
    pipeline_run_statistics totals;
    totals.emitted_flows = 1; totals.bytes = 100; totals.packets = 2;
    totals.first_feature_at = std::chrono::steady_clock::now();
    assert(sender.drain(&totals, &error));
    peer.join();
  }
  state.clear_queues();
  pipeline_interrupted = 0;
  std::puts("PASS first frontend signal drains pending features; second signal cancels");
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("Starting actual UB queue executor checks");
  std::signal(SIGINT, [](int) { pipeline_interrupted = 1; });
  run_case(1, 1, 37);
  run_case(1, 2, 37);
  run_case(1, 2, 37, "slow");
  run_case(2, 2, 37);
  run_case(2, 2, 0);
  run_case(2, 4, 1); // Most queues are empty but still must DRAIN/ACK.
  run_case(2, 2, 37, "filter");
  run_case(2, 2, 37, "", 1);
  run_case(2, 2, 37, "init");
  run_case(2, 2, 37, "bind");
  run_case(2, 2, 37, "infer");
  run_case(2, 2, 37, "corrupt");
  run_case(2, 2, 37, "totals");
  run_case(2, 2, 100000, "blocked");
  run_case(2, 2, 100000, "interrupt");
  run_case(2, 48, 7);
  check_graceful_frontend_stop();
  std::puts("PASS all UB queue worker checks");
}
