// Fixture for dpdk_rx_worker_check.py, not a standalone production target.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "ktag_capture_dpdk.h"

constexpr unsigned RTE_ETH_RETA_GROUP_SIZE = 64;
constexpr unsigned LCORE_ID_ANY = ~0U;
struct rte_mempool {};
struct rte_mbuf { std::array<uint8_t, 32> bytes{}; };
struct rte_eth_dev_info { uint16_t reta_size = 128; };
struct rte_eth_rss_reta_entry64 { uint64_t mask = 0; uint16_t reta[64]{}; };
struct rte_eth_stats { uint64_t imissed = 7, ierrors = 3; };
struct mock_rx {
  std::deque<rte_mbuf *> incoming;
  std::thread::id reader;
  std::mutex owner_mutex;
};
std::vector<std::unique_ptr<mock_rx>> rx;
std::vector<uint16_t> reta(128);
std::atomic<unsigned> active{0}, peak{0}, registered{0}, copied{0}, freed{0};
std::atomic<uint64_t> clock_us{1000};
bool no_reta = false, missing_queue = false, registration_fails = false;
thread_local unsigned mock_lcore = LCORE_ID_ANY;
thread_local std::string dpdk_error;
thread_local void *registered_capture = nullptr;
uint64_t ktag_dpdk_now_us_internal() { return clock_us.fetch_add(1); }
unsigned rte_lcore_id() { return mock_lcore; }
int rte_thread_register() {
  if (registration_fails) { return -1; }
  mock_lcore = registered.fetch_add(1); return 0;
}
void rte_thread_unregister() { assert(mock_lcore != LCORE_ID_ANY); mock_lcore = LCORE_ID_ANY; --registered; }
int rte_eth_dev_info_get(uint16_t, rte_eth_dev_info *) { return 0; }
int rte_eth_dev_rss_reta_update(uint16_t, rte_eth_rss_reta_entry64 *table, uint16_t size) {
  if (no_reta) { return -95; }
  for (unsigned i = 0; i < size; ++i) { reta[i] = table[i / 64].reta[i % 64]; }
  return 0;
}
int rte_eth_dev_rss_reta_query(uint16_t, rte_eth_rss_reta_entry64 *table, uint16_t size) {
  if (no_reta) { return -95; }
  for (unsigned i = 0; i < size; ++i) { table[i / 64].reta[i % 64] = missing_queue ? 0 : reta[i]; }
  return 0;
}
int rte_eth_stats_get(uint16_t, rte_eth_stats *) { return 0; }
void rte_delay_us(uint32_t) { std::this_thread::yield(); }
uint16_t rte_eth_rx_burst(uint16_t, uint16_t q, rte_mbuf **buffers, uint16_t capacity) {
  auto &queue = *rx.at(q);
  { std::lock_guard<std::mutex> lock(queue.owner_mutex);
    if (queue.reader == std::thread::id{}) { queue.reader = std::this_thread::get_id(); }
    assert(queue.reader == std::this_thread::get_id());
  }
  const auto current = active.fetch_add(1) + 1;
  auto maximum = peak.load();
  while (current > maximum && !peak.compare_exchange_weak(maximum, current)) {}
  std::this_thread::sleep_for(std::chrono::microseconds(1));
  const auto count = static_cast<uint16_t>(std::min<size_t>(capacity, queue.incoming.size()));
  for (unsigned i = 0; i < count; ++i) { buffers[i] = queue.incoming.front(); queue.incoming.pop_front(); }
  --active;
  return count;
}
uint32_t rte_pktmbuf_pkt_len(rte_mbuf *m) { return static_cast<uint32_t>(m->bytes.size()); }
void rte_pktmbuf_free(rte_mbuf *m) { delete m; ++freed; }
#define rte_pktmbuf_mtod(m, type) reinterpret_cast<type>((m)->bytes.data())
struct ktag_packet_buffer { std::vector<uint8_t> bytes; };
extern "C" void ktag_packet_init(ktag_packet *p) {
  *p = {}; p->struct_size = sizeof(*p); p->api_version = KTAG_API_VERSION;
}
extern "C" ktag_status ktag_packet_make_owned(ktag_packet *p) {
  auto *buffer = new ktag_packet_buffer;
  buffer->bytes.assign(p->data, p->data + p->captured_length);
  p->data = buffer->bytes.data(); p->owner = buffer; ++copied;
  return KTAG_SUCCESS;
}
extern "C" void ktag_packet_release(ktag_packet *p) { delete p->owner; ktag_packet_init(p); }

// INSERT_ACTUAL_READERS

void setup(ktag_dpdk_context &context, unsigned queues, unsigned records) {
  context.rx_queue_count = static_cast<uint16_t>(queues);
  context.mode = KTAG_DPDK_PACKET_OWNED;
  context.rss_enabled = true;
  context.pending.resize(queues); context.counters.resize(queues);
  context.last_rx_us = clock_us.load();
  rx.clear(); copied = 0; freed = 0; peak = 0;
  for (unsigned q = 0; q < queues; ++q) {
    rx.emplace_back(new mock_rx);
    for (unsigned n = 0; n < records; ++n) {
      auto *m = new rte_mbuf;
      std::memcpy(m->bytes.data(), &q, sizeof(q));
      std::memcpy(m->bytes.data() + sizeof(q), &n, sizeof(n));
      rx.back()->incoming.push_back(m);
    }
  }
}
void check_parallel(unsigned queues) {
  constexpr unsigned records = 513;
  ktag_dpdk_context context;
  setup(context, queues, records);
  assert(ktag_dpdk_capture_prepare_readers(&context, queues) == KTAG_SUCCESS);
  std::vector<std::thread> readers;
  std::mutex sequence_mutex;
  std::set<uint64_t> sequences;
  for (unsigned q = 0; q < queues; ++q) {
    readers.emplace_back([&, q]() {
      assert(ktag_dpdk_capture_reader_enter(&context) == KTAG_SUCCESS);
      ktag_packet packet; ktag_packet_init(&packet);
      for (unsigned n = 0; n < records; ++n) {
        assert(ktag_dpdk_capture_next_queue(&context, q, &packet) == KTAG_SUCCESS);
        unsigned actual_q, actual_n;
        std::memcpy(&actual_q, packet.data, sizeof(actual_q));
        std::memcpy(&actual_n, packet.data + sizeof(actual_q), sizeof(actual_n));
        assert(actual_q == q && actual_n == n); // Same queue stays ordered.
        { std::lock_guard<std::mutex> lock(sequence_mutex); assert(sequences.insert(packet.sequence).second); }
        ktag_packet_release(&packet);
      }
      ktag_dpdk_capture_reader_leave(&context);
    });
  }
  for (auto &reader : readers) { reader.join(); }
  assert(registered == 0 && copied == queues * records && freed == copied);
  assert(sequences.size() == queues * records && peak > 1);
  ktag_dpdk_capture_stats totals{};
  totals.struct_size = sizeof(totals); totals.api_version = KTAG_API_VERSION;
  assert(ktag_dpdk_capture_get_stats(&context, &totals) == KTAG_SUCCESS);
  assert(totals.received == queues * records && totals.delivered == totals.received && totals.pending == 0);
  assert(totals.dropped == 10); // Port-level count must not be multiplied by readers.
  std::printf("PASS actual RX readers=%u: packet ownership, per-queue order, unique sequences, counters, concurrent PMD calls\n", queues);
}

int main() {
  check_parallel(4);
  check_parallel(16);
  ktag_dpdk_context context;
  setup(context, 4, 0);
  assert(ktag_dpdk_capture_prepare_readers(nullptr, 4) == KTAG_ERROR_INVALID_ARGUMENT);
  assert(ktag_dpdk_capture_prepare_readers(&context, 3) == KTAG_ERROR_INVALID_ARGUMENT);
  context.rss_enabled = false;
  assert(ktag_dpdk_capture_prepare_readers(&context, 4) != KTAG_SUCCESS);
  context.rss_enabled = true; missing_queue = true;
  assert(ktag_dpdk_capture_prepare_readers(&context, 4) != KTAG_SUCCESS);
  missing_queue = false; no_reta = true;
  assert(ktag_dpdk_capture_prepare_readers(&context, 4) == KTAG_SUCCESS); // Explicit capability warning.
  no_reta = false; registration_fails = true;
  assert(ktag_dpdk_capture_reader_enter(&context) != KTAG_SUCCESS);
  registration_fails = false;
  assert(ktag_dpdk_capture_reader_enter(&context) == KTAG_SUCCESS);
  ktag_packet packet; ktag_packet_init(&packet);
  assert(ktag_dpdk_capture_next(&context, &packet) == KTAG_ERROR_INVALID_STATE);
  assert(ktag_dpdk_capture_next_queue(&context, 4, &packet) == KTAG_ERROR_INVALID_ARGUMENT);
  context.rx_idle_eof_us = 100;
  context.last_rx_us = clock_us.load();
  assert(ktag_dpdk_capture_next_queue(&context, 0, &packet) == KTAG_STATUS_TIMEOUT);
  // Activity from any queue prevents an otherwise idle queue from ending.
  clock_us += 99; context.last_rx_us = clock_us.load();
  assert(ktag_dpdk_capture_next_queue(&context, 0, &packet) == KTAG_STATUS_TIMEOUT);
  clock_us += 101;
  assert(ktag_dpdk_capture_next_queue(&context, 0, &packet) == KTAG_STATUS_END_OF_STREAM);
  context.stopped = true;
  assert(ktag_dpdk_capture_next_queue(&context, 1, &packet) == KTAG_STATUS_END_OF_STREAM);
  ktag_dpdk_capture_reader_leave(&context);
  std::puts("PASS RX invalid configuration, RSS coverage, registration failure, global idle and stop");

  ktag_dpdk_context single;
  setup(single, 4, 5);
  assert(ktag_dpdk_capture_prepare_readers(&single, 1) == KTAG_SUCCESS);
  for (unsigned n = 0; n < 20; ++n) {
    assert(ktag_dpdk_capture_next(&single, &packet) == KTAG_SUCCESS);
    assert(packet.sequence == n + 1);
    ktag_packet_release(&packet);
  }
  assert(copied == 20 && freed == 20);
  std::puts("PASS original single-reader round-robin API remains compatible");
}
