/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_capture_dpdk.h"

#include <rte_eal.h>
#include <rte_ethdev.h>

#include <pthread.h>
#include <sched.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
volatile sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

struct options {
    std::string device;
    std::string eal_args;
    std::vector<uint32_t> cpus;
    uint32_t rxd = KTAG_DPDK_DEFAULT_RX_QUEUE_DEPTH;
    uint32_t mbufs = 65536U;
    uint32_t burst = KTAG_DPDK_DEFAULT_BURST_SIZE;
};

void usage() {
    std::puts("Usage: ktag_rx_bench --device PCI --rx-cpus CPU,CPU,... --eal-args 'ARGS'\n"
              "                     [--rxd 1024] [--mbufs 65536] [--burst 64]\n"
              "One RX thread per queue; existing KTAG MBUF capture, count and free.\n"
              "EAL args exclude the program name; enable only the main EAL lcore.\n"
              "Start pktgen after READY. Stop pktgen, drain, then Ctrl+C for totals.");
}

uint32_t number(const std::string &text, uint32_t maximum, bool allow_zero = false) {
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        (!allow_zero && value == 0U) || value > maximum) {
        throw std::runtime_error("invalid numeric value: " + text);
    }
    return value;
}

options parse(int argc, char **argv) {
    options out;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key != "--device" && key != "--rx-cpus" && key != "--eal-args" &&
            key != "--rxd" && key != "--mbufs" && key != "--burst") {
            throw std::runtime_error("unknown option: " + key);
        }
        if (++i == argc) { throw std::runtime_error("missing value for " + key); }
        const std::string value = argv[i];
        if (key == "--device") { out.device = value; }
        else if (key == "--eal-args") { out.eal_args = value; }
        else if (key == "--rxd") { out.rxd = number(value, 65535U); }
        else if (key == "--mbufs") { out.mbufs = number(value, UINT32_MAX); }
        else if (key == "--burst") { out.burst = number(value, KTAG_DPDK_DEFAULT_BURST_SIZE); }
        else {
            out.cpus.clear();
            size_t begin = 0;
            do {
                const size_t end = value.find(',', begin);
                const uint32_t cpu = number(value.substr(begin, end - begin), CPU_SETSIZE - 1U, true);
                if (std::find(out.cpus.begin(), out.cpus.end(), cpu) != out.cpus.end()) {
                    throw std::runtime_error("duplicate RX CPU: " + std::to_string(cpu));
                }
                out.cpus.push_back(cpu);
                if (end == std::string::npos) { break; }
                begin = end + 1U;
            } while (true);
        }
    }
    if (out.device.empty() || out.eal_args.empty() || out.cpus.empty() || out.cpus.size() > 256U) {
        throw std::runtime_error("--device, --eal-args and 1..256 distinct --rx-cpus are required");
    }
    // EAL may narrow the main thread's affinity; validate against the original set.
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        throw std::runtime_error("sched_getaffinity failed");
    }
    for (const auto cpu : out.cpus) {
        if (!CPU_ISSET(cpu, &allowed)) {
            throw std::runtime_error("RX CPU outside allowed CPU set: " + std::to_string(cpu));
        }
    }
    return out;
}

void check(ktag_status status, const char *operation) {
    if (status != KTAG_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " + ktag_dpdk_capture_get_last_error());
    }
}

struct reader_scope {
    void *capture;
    bool thread_entered = false;
    bool reader_entered = false;
    ~reader_scope() {
        if (reader_entered) { ktag_dpdk_capture_reader_leave(capture); }
        if (thread_entered) { ktag_dpdk_capture_thread_leave(capture); }
    }
    void enter() {
        check(ktag_dpdk_capture_thread_enter(), "thread_enter");
        thread_entered = true;
        check(ktag_dpdk_capture_reader_enter(capture), "reader_enter");
        reader_entered = true;
    }
};

struct alignas(64) rx_result {
    uint64_t packets = 0;
    uint64_t bytes = 0;
    std::string error;
};

struct mbuf_scope {
    void *value = nullptr;
    ~mbuf_scope() { if (value != nullptr) { ktag_dpdk_mbuf_release(value); } }
};

class rx_workers {
public:
    rx_workers(void *capture, const options &config)
        : results(config.cpus.size()), capture_(capture), config_(config) {
        threads_.reserve(config.cpus.size());
    }
    ~rx_workers() { stop_and_join(); }
    bool prepare() {
        for (size_t q = 0; q < results.size(); ++q) {
            threads_.emplace_back(&rx_workers::receive, this, static_cast<uint32_t>(q));
        }
        while (ready_.load(std::memory_order_acquire) != results.size() &&
               !stopped() && !interrupted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !stopped() && !interrupted;
    }
    void start() { started_.store(true, std::memory_order_release); }
    bool stopped() const { return stop_.load(std::memory_order_relaxed); }
    void stop_and_join() {
        stop_.store(true, std::memory_order_relaxed);
        ktag_dpdk_capture_interrupt(capture_);
        for (auto &thread : threads_) { if (thread.joinable()) { thread.join(); } }
    }
    std::vector<rx_result> results; // Read only after every thread has joined.

private:
    void receive(uint32_t queue) {
        try {
            cpu_set_t affinity;
            CPU_ZERO(&affinity);
            CPU_SET(config_.cpus[queue], &affinity);
            const int rc = pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
            if (rc != 0) { throw std::runtime_error(std::string("RX affinity: ") + std::strerror(rc)); }
            const auto name = "ktag_rx_" + std::to_string(queue);
            (void)pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
            reader_scope reader{capture_};
            reader.enter();
            mbuf_scope mbuf;
            ready_.fetch_add(1U, std::memory_order_release);
            while (!started_.load(std::memory_order_acquire) && !stopped()) { std::this_thread::yield(); }
            ktag_packet packet{};
            ktag_packet_init(&packet);
            auto &local = results[queue];
            while (!stopped()) {
                const auto status = results.size() == 1U
                    ? ktag_dpdk_capture_next_mbuf(capture_, &packet, &mbuf.value)
                    : ktag_dpdk_capture_next_queue_mbuf(capture_, queue, &packet, &mbuf.value);
                if (status == KTAG_STATUS_TIMEOUT) { continue; }
                if (status == KTAG_STATUS_END_OF_STREAM) { stop_.store(true); break; }
                check(status, "capture next_mbuf");
                if (mbuf.value == nullptr) { throw std::runtime_error("capture returned a null mbuf"); }
                ++local.packets;
                local.bytes += packet.captured_length;
                ktag_dpdk_mbuf_release(mbuf.value);
                mbuf.value = nullptr;
            }
        } catch (const std::exception &failure) {
            results[queue].error = failure.what();
            stop_.store(true, std::memory_order_relaxed);
        } catch (...) {
            results[queue].error = "unknown RX worker failure";
            stop_.store(true, std::memory_order_relaxed);
        }
    }
    void *capture_;
    const options &config_;
    std::vector<std::thread> threads_;
    std::atomic<uint32_t> ready_{0};
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_{false};
};

rte_eth_stats port_stats(uint16_t port) {
    rte_eth_stats stats{};
    const int rc = rte_eth_stats_get(port, &stats);
    if (rc != 0) { throw std::runtime_error("rte_eth_stats_get failed: " + std::to_string(rc)); }
    return stats;
}

uint64_t delta(uint64_t current, uint64_t previous) {
    if (current < previous) { throw std::runtime_error("DPDK port counter decreased during measurement"); }
    return current - previous;
}

void print_rate(const rte_eth_stats &current, const rte_eth_stats &previous,
                const rte_eth_stats &baseline, double seconds) {
    std::printf("RX: %.3f Gbps | %.3f Mpps | missed=%" PRIu64
                " | errors=%" PRIu64 " | nombuf=%" PRIu64 "\n",
                delta(current.ibytes, previous.ibytes) * 8.0 / seconds / 1e9,
                delta(current.ipackets, previous.ipackets) / seconds / 1e6,
                delta(current.imissed, baseline.imissed), delta(current.ierrors, baseline.ierrors),
                delta(current.rx_nombuf, baseline.rx_nombuf));
}

struct eal_cleanup {
    ~eal_cleanup() { (void)rte_eal_cleanup(); }
};
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        usage(); return 0;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        const auto config = parse(argc, argv);
        struct sigaction action{};
        action.sa_handler = on_signal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
            throw std::runtime_error("could not install signal handlers");
        }
        check(ktag_dpdk_eal_init(config.eal_args.c_str()), "EAL init");
        eal_cleanup cleanup;
        ktag_dpdk_capture_config capture_config{};
        capture_config.struct_size = sizeof(capture_config);
        capture_config.api_version = KTAG_API_VERSION;
        capture_config.device_name = config.device.c_str();
        capture_config.rx_queue_count = static_cast<uint32_t>(config.cpus.size());
        capture_config.rx_queue_depth = config.rxd;
        capture_config.mbuf_pool_size = config.mbufs;
        capture_config.burst_size = config.burst;
        capture_config.packet_mode = KTAG_DPDK_PACKET_MBUF;
        std::unique_ptr<void, decltype(&ktag_dpdk_capture_close)> capture(
            ktag_dpdk_capture_open(&capture_config), &ktag_dpdk_capture_close);
        if (!capture) { throw std::runtime_error(std::string("capture open: ") + ktag_dpdk_capture_get_last_error()); }
        check(ktag_dpdk_capture_prepare_readers(capture.get(), capture_config.rx_queue_count), "prepare_readers");
        uint16_t port = 0;
        if (rte_eth_dev_get_port_by_name(config.device.c_str(), &port) != 0) {
            throw std::runtime_error("cannot resolve DPDK port for " + config.device);
        }
        std::printf("ktag_rx_bench: device=%s port=%u NUMA=%d queues=%zu mode=mbuf\n"
                    "  rxd(requested)=%u mbufs=%u burst=%u; bytes exclude wire overhead\n",
                    config.device.c_str(), port, rte_eth_dev_socket_id(port), config.cpus.size(),
                    config.rxd, config.mbufs, config.burst);
        for (size_t q = 0; q < config.cpus.size(); ++q) {
            rte_eth_rxq_info info{};
            const int rc = rte_eth_rx_queue_info_get(port, static_cast<uint16_t>(q), &info);
            std::printf("  RX queue %zu -> CPU %u; rxd(actual)=%u%s\n", q, config.cpus[q],
                        rc == 0 ? info.nb_desc : 0U, rc == 0 ? "" : " (query unsupported)");
        }
        rx_workers workers(capture.get(), config);
        const bool ready = workers.prepare();
        const auto baseline = port_stats(port);
        auto previous = baseline;
        auto previous_time = clock_type::now();
        if (ready) {
            std::puts("READY: start pktgen; stop pktgen and drain before Ctrl+C. Rates use DPDK port counters.");
            workers.start();
        }
        while (ready && !interrupted && !workers.stopped()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = clock_type::now();
            if (now - previous_time >= std::chrono::seconds(1)) {
                const auto current = port_stats(port);
                const auto sampled = clock_type::now();
                print_rate(current, previous, baseline, std::chrono::duration<double>(sampled - previous_time).count());
                previous = current;
                previous_time = sampled;
            }
        }
        workers.stop_and_join();
        bool failed = false;
        uint64_t packets = 0, bytes = 0;
        std::puts("\nRX summary (whole run, including startup/drain; no automatic zero-loss verdict):");
        for (size_t q = 0; q < workers.results.size(); ++q) {
            const auto &result = workers.results[q];
            packets += result.packets; bytes += result.bytes;
            std::printf("  queue=%zu cpu=%u packets=%" PRIu64 " bytes=%" PRIu64 "\n",
                        q, config.cpus[q], result.packets, result.bytes);
            if (!result.error.empty()) {
                std::fprintf(stderr, "RX queue %zu failed: %s\n", q, result.error.c_str()); failed = true;
            }
        }
        ktag_dpdk_capture_stats stats{};
        stats.struct_size = sizeof(stats); stats.api_version = KTAG_API_VERSION;
        check(ktag_dpdk_capture_get_stats(capture.get(), &stats), "capture statistics after join");
        const auto final = port_stats(port);
        std::printf("Application received/freed: packets=%" PRIu64 " bytes=%" PRIu64 "\n"
                    "Capture: received=%" PRIu64 " delivered=%" PRIu64 " pending=%" PRIu64 "\n"
                    "DPDK delta: packets=%" PRIu64 " bytes=%" PRIu64 " missed=%" PRIu64
                    " errors=%" PRIu64 " nombuf=%" PRIu64 "\n",
                    packets, bytes, stats.received, stats.delivered, stats.pending,
                    delta(final.ipackets, baseline.ipackets), delta(final.ibytes, baseline.ibytes),
                    delta(final.imissed, baseline.imissed), delta(final.ierrors, baseline.ierrors),
                    delta(final.rx_nombuf, baseline.rx_nombuf));
        if (packets != stats.delivered || stats.received != stats.delivered + stats.pending) {
            throw std::runtime_error("application/Capture packet accounting mismatch");
        }
        return failed ? 1 : 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "ktag_rx_bench: %s\n", failure.what());
        return 1;
    }
}
