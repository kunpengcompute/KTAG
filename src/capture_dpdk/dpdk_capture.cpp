/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "dpdk_capture_internal.h"

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

namespace {

thread_local std::string dpdk_error;
thread_local void *registered_capture = nullptr;

constexpr uint32_t EAL_MAX_ARGV = 64U;
constexpr uint16_t MBUF_POOL_CACHE_SIZE = 512U;
/* rx_queue_count is used as a modulus over the per-queue burst cache and
 * rx_queue_depth narrows to uint16_t when handed to DPDK. */
constexpr uint32_t MAX_RX_QUEUES = 256U;
constexpr uint32_t MAX_RX_QUEUE_DEPTH = 65535U;

bool valid_config(const ktag_dpdk_capture_config *config) {
  if (config == nullptr || config->struct_size < sizeof(*config) ||
      config->api_version != KTAG_API_VERSION) {
    dpdk_error = "invalid DPDK capture configuration";
    return false;
  }
  if (config->packet_mode != KTAG_DPDK_PACKET_BORROWED &&
      config->packet_mode != KTAG_DPDK_PACKET_OWNED) {
    dpdk_error = "packet_mode must be BORROWED or OWNED";
    return false;
  }
  /*
   * burst_size indexes a fixed-size stack array in next(), and the two queue
   * fields feed a modulus and a uint16_t narrowing. Reject out-of-range
   * values instead of clamping them: a bad YAML value should be reported,
   * not silently rerate the capture.
   */
  if (config->burst_size > KTAG_DPDK_DEFAULT_BURST_SIZE) {
    dpdk_error =
        "burst_size must be 0.." + std::to_string(KTAG_DPDK_DEFAULT_BURST_SIZE);
    return false;
  }
  if (config->rx_queue_count > MAX_RX_QUEUES) {
    dpdk_error = "rx_queue_count must be 0.." + std::to_string(MAX_RX_QUEUES);
    return false;
  }
  if (config->rx_queue_depth > MAX_RX_QUEUE_DEPTH) {
    dpdk_error =
        "rx_queue_depth must be 0.." + std::to_string(MAX_RX_QUEUE_DEPTH);
    return false;
  }
  return true;
}

/*
 * Splits a space-separated EAL argument string into tokens, respecting single
 * and double quotes (so paths with spaces survive) and backslash escapes.
 * Commas are never split: they carry kvargs semantics
 * (e.g. "--vdev=net_pcap0,rx_pcap=/x.pcap" is one token).
 */
std::vector<std::string> tokenize_eal_args(const char *input) {
  std::vector<std::string> tokens;
  if (input == nullptr) {
    return tokens;
  }
  const std::string text(input);
  size_t index = 0U;
  while (index < text.size()) {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }
    std::string token;
    char quote = 0;
    while (index < text.size()) {
      const char c = text[index];
      if (quote != 0) {
        if (c == quote) {
          quote = 0;
          ++index;
        } else if (c == '\\' && index + 1U < text.size()) {
          token += text[index + 1U];
          index += 2U;
        } else {
          token += c;
          ++index;
        }
        continue;
      }
      if (c == '\'' || c == '"') {
        quote = c;
        ++index;
        continue;
      }
      if (c == '\\' && index + 1U < text.size()) {
        token += text[index + 1U];
        index += 2U;
        continue;
      }
      if (c == ' ' || c == '\t') {
        break;
      }
      token += c;
      ++index;
    }
    if (!token.empty()) {
      tokens.push_back(std::move(token));
    }
  }
  return tokens;
}

std::string program_name() {
  char buffer[256] = {};
  const ssize_t length =
      ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1U);
  if (length <= 0) {
    return std::string("ktag");
  }
  buffer[length] = '\0';
  std::string path(buffer);
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1U);
}

std::mutex eal_mutex;
std::atomic<bool> eal_initialized{false};

/*
 * The token/argv storage is static so the C strings stay alive for the whole
 * process; rte_eal_init may retain them.
 */
std::vector<std::string> eal_tokens;
std::vector<char *> eal_argv;

ktag_status initialize_eal(const char *args) {
  std::lock_guard<std::mutex> lock(eal_mutex);
  if (eal_initialized.load(std::memory_order_acquire)) {
    return KTAG_SUCCESS;
  }

  eal_tokens = tokenize_eal_args(args);
  if (eal_tokens.size() > EAL_MAX_ARGV) {
    dpdk_error = "too many EAL arguments";
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  eal_argv.clear();
  std::string name = program_name();
  eal_argv.push_back(name.data());
  for (auto &token : eal_tokens) {
    eal_argv.push_back(token.data());
  }

  const int status =
      rte_eal_init(static_cast<int>(eal_argv.size()), eal_argv.data());
  if (status < 0 && rte_errno == EALREADY) {
    eal_initialized.store(true, std::memory_order_release);
    return KTAG_SUCCESS;
  }
  if (status < 0) {
    dpdk_error = rte_strerror(rte_errno);
    return KTAG_ERROR_IO;
  }
  eal_initialized.store(true, std::memory_order_release);
  return KTAG_SUCCESS;
}

ktag_status resolve_port(const char *device_name, uint16_t *port) {
  if (device_name != nullptr && device_name[0] != '\0') {
    const int found = rte_eth_dev_get_port_by_name(device_name, port);
    if (found == 0 && rte_eth_dev_is_valid_port(*port) != 0) {
      return KTAG_SUCCESS;
    }
    dpdk_error = std::string("DPDK device not found: ") + device_name;
    return KTAG_ERROR_NOT_FOUND;
  }

  /* Auto-select: prefer a PCI port, fall back to the first available. */
  const uint16_t count = rte_eth_dev_count_avail();
  uint16_t fallback = RTE_MAX_ETHPORTS;
  for (uint16_t candidate = 0U; candidate < count; ++candidate) {
    if (rte_eth_dev_is_valid_port(candidate) == 0) {
      continue;
    }
    if (fallback == RTE_MAX_ETHPORTS) {
      fallback = candidate;
    }
    char name[RTE_ETH_NAME_MAX_LEN] = {};
    if (rte_eth_dev_get_name_by_port(candidate, name) == 0 &&
        std::strncmp(name, "0000:", 5U) == 0) {
      *port = candidate;
      return KTAG_SUCCESS;
    }
  }
  if (fallback == RTE_MAX_ETHPORTS) {
    dpdk_error = "no DPDK port is available";
    return KTAG_ERROR_NOT_FOUND;
  }
  *port = fallback;
  return KTAG_SUCCESS;
}

void release_pending(ktag_dpdk_context *context) {
  if (context->current != nullptr) {
    rte_pktmbuf_free(context->current);
    context->current = nullptr;
  }
  for (auto &queue : context->pending) {
    context->discarded_total += queue.size();
    for (rte_mbuf *mbuf : queue) {
      rte_pktmbuf_free(mbuf);
    }
    queue.clear();
  }
}

ktag_status configure_port(ktag_dpdk_context *context,
                           const ktag_dpdk_capture_config *config) {
  struct rte_eth_dev_info info;
  std::memset(&info, 0, sizeof(info));
  const int info_status = rte_eth_dev_info_get(context->port, &info);
  if (info_status != 0) {
    dpdk_error =
        std::string("rte_eth_dev_info_get failed: ") + rte_strerror(rte_errno);
    return KTAG_ERROR_INTERNAL;
  }

  const int socket = rte_eth_dev_socket_id(context->port);

  const uint32_t rx_queues =
      config->rx_queue_count == 0U ? 1U : config->rx_queue_count;
  if (rx_queues > info.max_rx_queues) {
    dpdk_error = "rx_queue_count=" + std::to_string(rx_queues) +
        " exceeds NIC max_rx_queues=" + std::to_string(info.max_rx_queues);
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  context->rx_queue_count = static_cast<uint16_t>(rx_queues);
  context->pending.resize(rx_queues);
  for (auto &pending : context->pending) { pending.reserve(context->burst_size); }
  context->counters.resize(rx_queues);

  uint16_t nb_rx = config->rx_queue_depth == 0U
                       ? KTAG_DPDK_DEFAULT_RX_QUEUE_DEPTH
                       : config->rx_queue_depth;
  uint16_t nb_tx = 0U;
  (void)rte_eth_dev_adjust_nb_rx_tx_desc(context->port, &nb_rx, &nb_tx);

  /*
   * Every RX queue is backed by nb_rx descriptors drawn from the pool, so the
   * default pool has to scale with the queue count. Sizing it from the depth
   * alone starves the later queues: with 1024-deep queues the third
   * rte_eth_rx_queue_setup fails with "Fail to alloc mbuf for Rx queue".
   * The multiplier leaves headroom for packets in flight.
   */
  const uint32_t pool_size = config->mbuf_pool_size == 0U
                                 ? static_cast<uint32_t>(nb_rx) * rx_queues *
                                       KTAG_DPDK_DEFAULT_MBUF_POOL_MULTIPLIER
                                 : config->mbuf_pool_size;
  context->pool =
      rte_pktmbuf_pool_create("ktag_mbuf_pool", pool_size, MBUF_POOL_CACHE_SIZE,
                              0U, RTE_MBUF_DEFAULT_BUF_SIZE, socket);
  if (context->pool == nullptr) {
    dpdk_error = std::string("rte_pktmbuf_pool_create failed: ") +
                 rte_strerror(rte_errno);
    return KTAG_ERROR_OUT_OF_MEMORY;
  }

  struct rte_eth_conf port_conf;
  std::memset(&port_conf, 0, sizeof(port_conf));
  if (rx_queues > 1U) {
    /*
     * Only ask for hash types the PMD actually advertises; passing an
     * unsupported rss_hf makes rte_eth_dev_configure fail outright. A PMD
     * with no RSS at all (net_pcap, for one) still gets its queues, just
     * without a hash function.
     */
    port_conf.rx_adv_conf.rss_conf.rss_hf =
        info.flow_type_rss_offloads &
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP);
    if (port_conf.rx_adv_conf.rss_conf.rss_hf != 0U) {
      port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
      context->rss_enabled = true;
    } else {
      port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    }
  } else {
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  }

  /* Try capture-only first; some PMDs require a minimal TX queue. */
  uint16_t tx_queues = 0U;
  int configure_status =
      rte_eth_dev_configure(context->port, rx_queues, tx_queues, &port_conf);
  if (configure_status != 0) {
    tx_queues = 1U;
    configure_status =
        rte_eth_dev_configure(context->port, rx_queues, tx_queues, &port_conf);
  }
  if (configure_status != 0) {
    dpdk_error =
        std::string("rte_eth_dev_configure failed: ") + rte_strerror(rte_errno);
    return KTAG_ERROR_INTERNAL;
  }

  struct rte_eth_rxconf rx_conf;
  std::memset(&rx_conf, 0, sizeof(rx_conf));
  rx_conf.rx_free_thresh = 32U;
  for (uint32_t queue = 0U; queue < rx_queues; ++queue) {
    const int setup_status = rte_eth_rx_queue_setup(
        context->port, queue, nb_rx, static_cast<unsigned int>(socket),
        &rx_conf, context->pool);
    if (setup_status != 0) {
      dpdk_error = std::string("rte_eth_rx_queue_setup failed: ") +
                   rte_strerror(rte_errno);
      return KTAG_ERROR_INTERNAL;
    }
  }
  if (tx_queues != 0U) {
    struct rte_eth_txconf tx_conf;
    std::memset(&tx_conf, 0, sizeof(tx_conf));
    (void)rte_eth_tx_queue_setup(context->port, 0U, nb_tx == 0U ? 512U : nb_tx,
                                 static_cast<unsigned int>(socket), &tx_conf);
  }

  const int start_status = rte_eth_dev_start(context->port);
  if (start_status != 0) {
    dpdk_error =
        std::string("rte_eth_dev_start failed: ") + rte_strerror(rte_errno);
    return KTAG_ERROR_INTERNAL;
  }
  rte_eth_promiscuous_enable(context->port);
  /*
   * Start the device counters from zero so the drop figures reported by
   * ktag_dpdk_capture_get_stats() describe this run, not the port's lifetime.
   */
  rte_eth_stats_reset(context->port);
  return KTAG_SUCCESS;
}

} // namespace

void ktag_dpdk_set_error_internal(const std::string &message) {
  dpdk_error = message;
}

uint64_t ktag_dpdk_now_us_internal(void) {
  struct timespec now;
  std::memset(&now, 0, sizeof(now));
  (void)clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000U);
}

extern "C" {

KTAG_API ktag_status ktag_dpdk_eal_init(const char *eal_args) {
  dpdk_error.clear();
  return initialize_eal(eal_args);
}

KTAG_API void *ktag_dpdk_capture_open(const ktag_dpdk_capture_config *config) {
  dpdk_error.clear();
  if (!valid_config(config)) {
    /* valid_config() already set the specific reason. */
    return nullptr;
  }
  ktag_status status = initialize_eal(nullptr);
  if (status != KTAG_SUCCESS) {
    return nullptr;
  }

  std::unique_ptr<ktag_dpdk_context> context(new (std::nothrow)
                                                 ktag_dpdk_context());
  if (!context) {
    dpdk_error = "out of memory creating DPDK capture context";
    return nullptr;
  }
  context->mode = config->packet_mode;
  context->burst_size = config->burst_size == 0U ? KTAG_DPDK_DEFAULT_BURST_SIZE
                                                 : config->burst_size;
  context->rx_idle_eof_us = config->rx_idle_eof_us;
  context->poll_interval_us = config->poll_interval_us;
  context->pending.reserve(context->burst_size);

  status = resolve_port(config->device_name, &context->port);
  if (status != KTAG_SUCCESS) {
    return nullptr;
  }
  status = configure_port(context.get(), config);
  if (status != KTAG_SUCCESS) {
    return nullptr;
  }
  context->last_rx_us.store(ktag_dpdk_now_us_internal(),
                            std::memory_order_relaxed);
  return context.release();
}

KTAG_API ktag_status ktag_dpdk_capture_next(void *context,
                                            ktag_packet *packet) {
  dpdk_error.clear();
  if (context == nullptr || packet == nullptr ||
      packet->struct_size < sizeof(*packet) ||
      packet->api_version != KTAG_API_VERSION) {
    dpdk_error = "invalid DPDK capture context or packet output";
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (packet->owner != nullptr) {
    dpdk_error = "owned packet must be released before ktag_dpdk_capture_next";
    return KTAG_ERROR_INVALID_STATE;
  }

  auto *capture = static_cast<ktag_dpdk_context *>(context);
  if (capture->reader_count != 1U) {
    dpdk_error = "multi-reader capture requires next_queue(), not next()";
    return KTAG_ERROR_INVALID_STATE;
  }
  ktag_packet_init(packet);
  if (capture->stopped.load(std::memory_order_acquire)) {
    return KTAG_STATUS_END_OF_STREAM;
  }
  if (capture->mode == KTAG_DPDK_PACKET_BORROWED &&
      capture->current != nullptr) {
    rte_pktmbuf_free(capture->current);
    capture->current = nullptr;
  }

  const uint64_t now_us = ktag_dpdk_now_us_internal();
  if (capture->pending[capture->active_queue].empty()) {
    rte_mbuf *burst[KTAG_DPDK_DEFAULT_BURST_SIZE];
    uint16_t received = 0U;
    /*
     * Round-robin refill: scan the queues from the cursor and take the
     * first one that yields packets. RSS pins every packet of a flow to a
     * single queue, so rotating between queues can reorder packets across
     * flows but never within one.
     */
    for (uint16_t offset = 0U; offset < capture->rx_queue_count; ++offset) {
      const uint16_t queue = static_cast<uint16_t>(
          (capture->next_rx_queue + offset) % capture->rx_queue_count);
      received =
          rte_eth_rx_burst(capture->port, queue, burst, capture->burst_size);
      if (received != 0U) {
        capture->pending[queue].assign(burst, burst + received);
        capture->received_total += received;
        capture->counters[queue].received += received;
        capture->active_queue = queue;
        capture->next_rx_queue =
            static_cast<uint16_t>((queue + 1U) % capture->rx_queue_count);
        break;
      }
    }
    if (received == 0U) {
      if (capture->rx_idle_eof_us != 0U &&
          now_us >= capture->last_rx_us.load(std::memory_order_relaxed) &&
          now_us - capture->last_rx_us.load(std::memory_order_relaxed) >=
              capture->rx_idle_eof_us) {
        return KTAG_STATUS_END_OF_STREAM;
      }
      if (capture->poll_interval_us != 0U) {
        rte_delay_us(capture->poll_interval_us);
      }
      return KTAG_STATUS_TIMEOUT;
    }
    capture->last_rx_us.store(now_us, std::memory_order_relaxed);
  }

  rte_mbuf *mbuf = capture->pending[capture->active_queue].front();
  capture->pending[capture->active_queue].erase(
      capture->pending[capture->active_queue].begin());
  packet->data = rte_pktmbuf_mtod(mbuf, const uint8_t *);
  packet->captured_length = rte_pktmbuf_pkt_len(mbuf);
  packet->original_length = rte_pktmbuf_pkt_len(mbuf);
  packet->link_type = capture->link_type;
  packet->sequence = capture->next_sequence++;
  packet->timestamp.seconds = static_cast<int64_t>(now_us / 1000000U);
  packet->timestamp.microseconds = static_cast<int32_t>(now_us % 1000000U);

  if (capture->mode == KTAG_DPDK_PACKET_OWNED) {
    const ktag_status owned_status = ktag_packet_make_owned(packet);
    rte_pktmbuf_free(mbuf);
    if (owned_status != KTAG_SUCCESS) {
      dpdk_error = "failed to make captured packet owned";
      return owned_status;
    }
    ++capture->delivered_total;
    ++capture->counters[capture->active_queue].delivered;
    return KTAG_SUCCESS;
  }
  capture->current = mbuf;
  ++capture->delivered_total;
  ++capture->counters[capture->active_queue].delivered;
  return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_dpdk_capture_prepare_readers(void *context, uint32_t readers) {
  dpdk_error.clear();
  auto *capture = static_cast<ktag_dpdk_context *>(context);
  if (!capture || readers == 0U || (readers > 1U &&
      (readers != capture->rx_queue_count || capture->mode != KTAG_DPDK_PACKET_OWNED))) {
    dpdk_error = "RX readers must be 1, or equal rx_queue_count with OWNED packets";
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (readers > 1U) {
    if (!capture->rss_enabled) {
      dpdk_error = "multi-reader RX requires NIC RSS support";
      return KTAG_ERROR_INVALID_STATE;
    }
    rte_eth_dev_info info{};
    if (rte_eth_dev_info_get(capture->port, &info) != 0 || info.reta_size < readers) {
      dpdk_error = "NIC RSS RETA cannot cover all requested RX queues";
      return KTAG_ERROR_INVALID_STATE;
    }
    std::vector<rte_eth_rss_reta_entry64> reta((info.reta_size + RTE_ETH_RETA_GROUP_SIZE - 1U) / RTE_ETH_RETA_GROUP_SIZE);
    for (uint32_t i = 0; i < info.reta_size; ++i) {
      auto &entry = reta[i / RTE_ETH_RETA_GROUP_SIZE];
      const auto bit = i % RTE_ETH_RETA_GROUP_SIZE;
      entry.mask |= uint64_t{1} << bit;
      entry.reta[bit] = static_cast<uint16_t>(i % readers);
    }
    const int updated = rte_eth_dev_rss_reta_update(capture->port, reta.data(), info.reta_size);
    const int queried = rte_eth_dev_rss_reta_query(capture->port, reta.data(), info.reta_size);
    if (queried == 0) {
      std::vector<bool> covered(readers, false);
      for (uint32_t i = 0; i < info.reta_size; ++i) {
        const auto q = reta[i / RTE_ETH_RETA_GROUP_SIZE].reta[i % RTE_ETH_RETA_GROUP_SIZE];
        if (q < readers) { covered[q] = true; }
      }
      if (std::find(covered.begin(), covered.end(), false) != covered.end()) {
        dpdk_error = "NIC RSS RETA does not cover every configured RX queue";
        return KTAG_ERROR_INVALID_STATE;
      }
    } else if (updated != 0) {
      // Some PMDs expose neither operation; be explicit about the limit.
      std::fprintf(stderr, "WARNING: RSS RETA update/query unavailable (%d/%d); verify per-RX queue packet distribution\n", updated, queried);
    }
    std::printf("DPDK readers: queues=%u, readers=%u, RSS=enabled, RETA=%u, query=%s\n",
                capture->rx_queue_count, readers, info.reta_size, queried == 0 ? "verified" : "unavailable");
  }
  capture->reader_count = readers;
  return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_dpdk_capture_reader_enter(void *context) {
  dpdk_error.clear();
  auto *capture = static_cast<ktag_dpdk_context *>(context);
  if (!capture) { dpdk_error = "invalid RX reader context"; return KTAG_ERROR_INVALID_ARGUMENT; }
  if (capture->reader_count == 1U) { return KTAG_SUCCESS; }
  if (registered_capture || rte_lcore_id() != LCORE_ID_ANY || rte_thread_register() != 0) {
    dpdk_error = "RX thread registration failed (lcore capacity or DPDK secondary process conflict)";
    return KTAG_ERROR_INVALID_STATE;
  }
  registered_capture = context;
  return KTAG_SUCCESS;
}

KTAG_API void ktag_dpdk_capture_reader_leave(void *context) {
  if (registered_capture == context && context != nullptr) {
    rte_thread_unregister();
    registered_capture = nullptr;
  }
}

KTAG_API ktag_status ktag_dpdk_capture_next_queue(
    void *context, uint32_t queue_id, ktag_packet *packet) {
  dpdk_error.clear();
  auto *capture = static_cast<ktag_dpdk_context *>(context);
  if (!capture || !packet || packet->struct_size < sizeof(*packet) ||
      packet->api_version != KTAG_API_VERSION || packet->owner ||
      queue_id >= capture->rx_queue_count) {
    dpdk_error = "invalid queue reader arguments or unreleased owned packet";
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (capture->reader_count <= 1U || capture->mode != KTAG_DPDK_PACKET_OWNED ||
      registered_capture != context) {
    dpdk_error = "next_queue requires prepared OWNED multi-reader capture and reader_enter()";
    return KTAG_ERROR_INVALID_STATE;
  }
  ktag_packet_init(packet);
  if (capture->stopped.load(std::memory_order_acquire)) { return KTAG_STATUS_END_OF_STREAM; }
  auto &pending = capture->pending[queue_id];
  auto &counters = capture->counters[queue_id];
  const auto now_us = ktag_dpdk_now_us_internal();
  if (pending.empty()) {
    rte_mbuf *burst[KTAG_DPDK_DEFAULT_BURST_SIZE];
    const auto count = rte_eth_rx_burst(capture->port, static_cast<uint16_t>(queue_id), burst, capture->burst_size);
    if (count == 0U) {
      const auto last = capture->last_rx_us.load(std::memory_order_relaxed);
      if (capture->rx_idle_eof_us && now_us >= last && now_us - last >= capture->rx_idle_eof_us) {
        capture->stopped.store(true, std::memory_order_release);
        return KTAG_STATUS_END_OF_STREAM;
      }
      if (capture->poll_interval_us) { rte_delay_us(capture->poll_interval_us); }
      return KTAG_STATUS_TIMEOUT;
    }
    pending.assign(burst, burst + count);
    counters.received += count;
    auto last = capture->last_rx_us.load(std::memory_order_relaxed);
    while (last < now_us && !capture->last_rx_us.compare_exchange_weak(last, now_us, std::memory_order_relaxed)) {}
  }
  rte_mbuf *mbuf = pending.front();
  pending.erase(pending.begin());
  packet->data = rte_pktmbuf_mtod(mbuf, const uint8_t *);
  packet->captured_length = rte_pktmbuf_pkt_len(mbuf);
  packet->original_length = packet->captured_length;
  packet->link_type = capture->link_type;
  packet->sequence = capture->parallel_sequence.fetch_add(1U, std::memory_order_relaxed);
  packet->timestamp.seconds = static_cast<int64_t>(now_us / 1000000U);
  packet->timestamp.microseconds = static_cast<int32_t>(now_us % 1000000U);
  const auto status = ktag_packet_make_owned(packet);
  rte_pktmbuf_free(mbuf);
  if (status != KTAG_SUCCESS) {
    dpdk_error = "failed to make queue-captured packet owned";
    return status;
  }
  ++counters.delivered;
  return KTAG_SUCCESS;
}

KTAG_API uint32_t ktag_dpdk_capture_get_link_type(const void *context) {
  const auto *capture = static_cast<const ktag_dpdk_context *>(context);
  return capture == nullptr ? 0U : capture->link_type;
}

KTAG_API void ktag_dpdk_capture_interrupt(void *context) {
  if (context != nullptr) {
    auto *capture = static_cast<ktag_dpdk_context *>(context);
    capture->stopped.store(true, std::memory_order_release);
  }
}

KTAG_API void ktag_dpdk_capture_close(void *context) {
  if (context == nullptr) {
    return;
  }
  auto *capture = static_cast<ktag_dpdk_context *>(context);
  capture->stopped.store(true, std::memory_order_release);
  release_pending(capture);
  if (rte_eth_dev_is_valid_port(capture->port) != 0) {
    rte_eth_dev_stop(capture->port);
    rte_eth_dev_close(capture->port);
  }
  if (capture->pool != nullptr) {
    rte_mempool_free(capture->pool);
    capture->pool = nullptr;
  }
  delete capture;
}

KTAG_API ktag_status
ktag_dpdk_capture_get_stats(const void *context,
                            ktag_dpdk_capture_stats *stats) {
  if (context == nullptr || stats == nullptr ||
      stats->struct_size < sizeof(*stats) ||
      stats->api_version != KTAG_API_VERSION) {
    dpdk_error = "invalid DPDK capture context or stats output";
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  const auto *capture = static_cast<const ktag_dpdk_context *>(context);

  uint64_t pending = 0U;
  for (const auto &queue : capture->pending) {
    pending += queue.size();
  }
  stats->received = 0U;
  stats->delivered = 0U;
  for (const auto &queue : capture->counters) {
    stats->received += queue.received;
    stats->delivered += queue.delivered;
  }
  stats->pending = pending;
  stats->discarded = capture->discarded_total;

  /* Drops the device itself reports: no descriptor available, or RX errors. */
  stats->dropped = 0U;
  struct rte_eth_stats eth{};
  if (rte_eth_stats_get(capture->port, &eth) == 0) {
    stats->dropped = eth.imissed + eth.ierrors;
  }
  return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_dpdk_capture_get_queue_stats(
    const void *context, uint32_t queue_id, ktag_dpdk_queue_stats *stats) {
  if (context == nullptr || stats == nullptr ||
      stats->struct_size < sizeof(*stats) ||
      stats->api_version != KTAG_API_VERSION) {
    dpdk_error = "invalid DPDK capture context or queue stats output";
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  const auto *capture = static_cast<const ktag_dpdk_context *>(context);
  if (queue_id >= capture->rx_queue_count) {
    dpdk_error = "queue_id exceeds the configured rx_queue_count";
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  stats->queue_id = queue_id;
  stats->received = capture->counters[queue_id].received;
  stats->delivered = capture->counters[queue_id].delivered;
  stats->pending = capture->pending[queue_id].size();
  return KTAG_SUCCESS;
}

KTAG_API const char *ktag_dpdk_capture_get_last_error(void) {
  return dpdk_error.c_str();
}

} // extern "C"
