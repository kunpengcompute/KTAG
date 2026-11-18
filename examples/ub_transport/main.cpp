/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include "ub_ring_transport.h"

#include <csignal>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using transport = ktag_demo_ub_ring_transport;
using clock_type = std::chrono::steady_clock;
constexpr uint64_t magic = 0x4b54414755424259ULL;
constexpr uint32_t version = 1;
constexpr size_t capacity = 4096, header_size = 32;
enum class kind : uint32_t { credit = 1, data = 2, end = 3, ack = 4, error = 5 };
volatile sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

struct options {
    bool sender = false;
    std::string role, provider, user, input, output, eal;
    std::string prefix = "ktag_ub_bytes";
    uint32_t timeout_ms = 60000;
};

void usage() {
    std::puts("Usage: ktag_ub_bytes_example --role sender|receiver --provider HOST --user HOST\n"
              "       --eal-args 'ARGS' [--shm-prefix NAME] [--timeout-ms 60000]\n"
              "       sender: --input FILE; receiver: --output FILE\n"
              "1:1, one queue, arbitrary bytes. No features, model, NIC capture or UB ethdev.\n"
              "EAL args INCLUDE argv[0], e.g. 'ub_bytes --no-pci --no-huge --lcores=0@(0-3)'.");
}

options parse(int argc, char **argv) {
    options out;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key != "--role" && key != "--provider" && key != "--user" &&
            key != "--input" && key != "--output" && key != "--eal-args" &&
            key != "--shm-prefix" && key != "--timeout-ms") {
            throw std::runtime_error("unknown option: " + key);
        }
        if (++i == argc) { throw std::runtime_error("missing value for " + key); }
        const std::string value = argv[i];
        if (key == "--role") { out.role = value; }
        else if (key == "--provider") { out.provider = value; }
        else if (key == "--user") { out.user = value; }
        else if (key == "--input") { out.input = value; }
        else if (key == "--output") { out.output = value; }
        else if (key == "--eal-args") { out.eal = value; }
        else if (key == "--shm-prefix") { out.prefix = value; }
        else {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out.timeout_ms);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || !out.timeout_ms) {
                throw std::runtime_error("--timeout-ms must be a positive uint32");
            }
        }
    }
    if ((out.role != "sender" && out.role != "receiver") || out.provider.empty() ||
        out.user.empty() || out.prefix.empty() || out.eal.empty()) {
        throw std::runtime_error("role, provider, user, shm-prefix and eal-args must be configured");
    }
    out.sender = out.role == "sender";
    if (out.sender ? (out.input.empty() || !out.output.empty()) : (out.output.empty() || !out.input.empty())) {
        throw std::runtime_error("sender requires only --input; receiver requires only --output");
    }
    return out;
}

uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) { crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U))); }
    }
    return ~crc;
}
void put(uint8_t *p, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) { p[i] = static_cast<uint8_t>(value >> (8U * i)); }
}
uint64_t get(const uint8_t *p, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) { value |= static_cast<uint64_t>(p[i]) << (8U * i); }
    return value;
}
struct frame { kind type; uint64_t sequence; uint32_t size; };

void encode(void *buffer, kind type, uint64_t sequence, const void *data, size_t size) {
    if (!buffer || size > capacity - header_size || (size && !data)) {
        throw std::runtime_error("payload exceeds example frame capacity");
    }
    auto *p = static_cast<uint8_t *>(buffer);
    // Initialize the whole fixed-capacity frame; receive_copy copies this much.
    std::memset(p, 0, capacity);
    put(p, magic, 8); put(p + 8, version, 4); put(p + 12, static_cast<uint32_t>(type), 4);
    put(p + 16, sequence, 8); put(p + 24, size, 4);
    if (size) { std::memcpy(p + header_size, data, size); }
    put(p + 28, crc32(p + header_size, size), 4);
}
frame decode(const void *buffer) {
    const auto *p = static_cast<const uint8_t *>(buffer);
    if (get(p, 8) != magic || get(p + 8, 4) != version) {
        throw std::runtime_error("incompatible example frame magic/version");
    }
    const auto type = get(p + 12, 4);
    const auto size = get(p + 24, 4);
    if (type < 1 || type > 5 || size > capacity - header_size ||
        get(p + 28, 4) != crc32(p + header_size, static_cast<size_t>(size))) {
        throw std::runtime_error("invalid example frame type/length/CRC");
    }
    if (((type == 1 || type == 4 || type == 5) && size != 0) || (type == 3 && size != 16)) {
        throw std::runtime_error("invalid example control payload");
    }
    if (type == 5) { throw std::runtime_error("peer reported an error"); }
    return {static_cast<kind>(type), get(p + 16, 8), static_cast<uint32_t>(size)};
}

class channel {
public:
    explicit channel(const options &opts) : sender_(opts.sender), timeout_(opts.timeout_ms) {
        ktag_demo_ub_transport_config config;
        config.role = sender_ ? ktag_demo_ub_role::provider : ktag_demo_ub_role::user;
        config.provider_nodes = {opts.provider}; config.user_nodes = {opts.user};
        config.region = opts.prefix + "_region"; config.shm_prefix = opts.prefix;
        config.provider_shm_size = 268435456;
        config.queues_per_provider = 1; config.queue_depth = 128; config.data_element_size = capacity;
        // This adapter takes argv[0] too; its EAL parser needs a vector of tokens.
        config.eal_arguments = split_eal(opts.eal);
        std::string error;
        transport_ = transport::create(config, &error);
        if (!transport_) { throw std::runtime_error(error); }
        if (!transport_->queue_mapping(opts.provider, 0, &mapping_, &error)) { throw std::runtime_error(error); }
        const auto deadline = clock_type::now() + timeout_;
        uint32_t actual = 0;
        while (!(actual = transport_->data_element_size(mapping_.provider_node_index))) { pause(deadline, "waiting for peer data area"); }
        if (actual != ktag_demo_ub_runtime_data_element_size(capacity)) {
            throw std::runtime_error("peer payload capacity/SDK element size mismatch");
        }
        std::printf("UB example %s ready: tx=%u rx=%u payload_capacity=%zu\n",
                    sender_ ? "sender" : "receiver", mapping_.tx_queue_id, mapping_.rx_queue_id, capacity);
    }
    ~channel() {
        if (held_) {
            if (sender_) { transport_->return_buffer(held_); }
            else { transport_->free_buffer(held_); }
        }
    }
    void allocate_credit() {
        held_ = transport_->allocate_buffer(mapping_.provider_node_index);
        if (!held_) { throw std::runtime_error("could not allocate initial CREDIT"); }
        send(kind::credit, 0);
    }
    void credit(uint64_t expected) {
        const auto deadline = clock_type::now() + timeout_;
        while (!transport_->receive_burst(mapping_.rx_queue_id, &held_, 1)) { pause(deadline, "waiting for CREDIT/ACK"); }
        const auto message = decode(held_);
        if (message.type != kind::credit || message.sequence != expected) {
            throw std::runtime_error("unexpected CREDIT or sequence");
        }
    }
    frame receive(std::vector<uint8_t> &local) {
        ktag_demo_ub_copy_receipt receipt;
        const auto deadline = clock_type::now() + timeout_;
        while (!transport_->receive_copy(mapping_.rx_queue_id, local.data(), local.size(), capacity, &receipt)) {
            pause(deadline, "waiting for DATA/END");
        }
        held_ = receipt.ub_buffer;
        return decode(local.data());
    }
    void send(kind type, uint64_t sequence, const void *data = nullptr, size_t size = 0) {
        encode(held_, type, sequence, data, size);
        const auto deadline = clock_type::now() + timeout_;
        while (transport_->send_burst(mapping_.tx_queue_id, &held_, 1) != 1) { pause(deadline, "sending frame"); }
        held_ = nullptr; // Ownership moved to the peer; never write/free it now.
    }
    void ack(uint64_t sequence) {
        const auto deadline = clock_type::now() + timeout_;
        while (!transport_->receive_burst(mapping_.rx_queue_id, &held_, 1)) { pause(deadline, "waiting for final ACK"); }
        const auto message = decode(held_);
        if (message.type != kind::ack || message.sequence != sequence) { throw std::runtime_error("invalid final ACK"); }
        transport_->return_buffer(held_); held_ = nullptr;
    }
    void notify_error() noexcept {
        if (!held_) { return; }
        try {
            encode(held_, kind::error, 0, nullptr, 0);
            if (transport_->send_burst(mapping_.tx_queue_id, &held_, 1) == 1) { held_ = nullptr; }
        } catch (...) {}
    }

private:
    static std::vector<std::string> split_eal(const std::string &text) {
        std::vector<std::string> args;
        std::string token;
        char quote = 0;
        bool escape = false;
        for (const char c : text) {
            if (escape) { token += c; escape = false; }
            else if (c == '\\') { escape = true; }
            else if (quote) { if (c == quote) { quote = 0; } else { token += c; } }
            else if (c == '\'' || c == '"') { quote = c; }
            else if (c == ' ' || c == '\t') { if (!token.empty()) { args.push_back(token); token.clear(); } }
            else { token += c; }
        }
        if (quote || escape) { throw std::runtime_error("unclosed quote/escape in --eal-args"); }
        if (!token.empty()) { args.push_back(token); }
        if (args.empty() || args.front().front() == '-') { throw std::runtime_error("--eal-args must start with a program name"); }
        return args;
    }
    static void pause(clock_type::time_point deadline, const char *operation) {
        if (interrupted) { throw std::runtime_error("UB example interrupted"); }
        if (clock_type::now() >= deadline) { throw std::runtime_error(std::string(operation) + " timed out"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool sender_;
    std::chrono::milliseconds timeout_;
    std::unique_ptr<transport> transport_;
    ktag_demo_ub_queue_mapping mapping_{};
    void *held_ = nullptr;
};

void run_sender(const options &opts) {
    std::ifstream input(opts.input, std::ios::binary);
    if (!input) { throw std::runtime_error("cannot open input: " + opts.input); }
    channel link(opts);
    uint64_t messages = 0, bytes = 0;
    std::array<uint8_t, capacity - header_size> payload{};
    try {
        while (true) {
            link.credit(messages);
            // Customer seam: replace file input with any serialized bytes.
            input.read(reinterpret_cast<char *>(payload.data()), payload.size());
            const auto count = input.gcount();
            if (input.bad() || (input.fail() && !input.eof())) { throw std::runtime_error("input read failed"); }
            if (count == 0) { break; }
            link.send(kind::data, messages + 1, payload.data(), static_cast<size_t>(count));
            ++messages; bytes += static_cast<uint64_t>(count);
        }
        std::array<uint8_t, 16> totals{};
        put(totals.data(), messages, 8); put(totals.data() + 8, bytes, 8);
        link.send(kind::end, messages + 1, totals.data(), totals.size());
        link.ack(messages + 1);
        std::printf("Sender completed: messages=%llu bytes=%llu, final ACK verified\n",
                    static_cast<unsigned long long>(messages), static_cast<unsigned long long>(bytes));
    } catch (...) { link.notify_error(); throw; }
}

// Customer seam: consume ONLY the local copy. Copy again if handing data to an
// asynchronous consumer, since the example reuses the local frame on next receive.
void on_payload(std::ofstream &output, const uint8_t *data, size_t size) {
    output.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!output) { throw std::runtime_error("local payload processing/output failed"); }
}

void run_receiver(const options &opts) {
    std::ofstream output(opts.output, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("cannot open output: " + opts.output); }
    channel link(opts);
    uint64_t messages = 0, bytes = 0;
    std::vector<uint8_t> local(capacity);
    try {
        link.allocate_credit();
        while (true) {
            const auto message = link.receive(local);
            if (message.sequence != messages + 1) { throw std::runtime_error("DATA/END sequence mismatch"); }
            if (message.type == kind::end) {
                if (get(local.data() + header_size, 8) != messages ||
                    get(local.data() + header_size + 8, 8) != bytes) {
                    throw std::runtime_error("END message/byte totals mismatch");
                }
                output.flush();
                if (!output) { throw std::runtime_error("output flush failed"); }
                link.send(kind::ack, message.sequence);
                break; // Do not access the shared buffer after publishing ACK.
            }
            if (message.type != kind::data || !message.size) { throw std::runtime_error("expected non-empty DATA"); }
            on_payload(output, local.data() + header_size, message.size);
            ++messages; bytes += message.size;
            link.send(kind::credit, messages);
        }
        std::printf("Receiver completed: messages=%llu bytes=%llu, local copy CRC/totals verified\n",
                    static_cast<unsigned long long>(messages), static_cast<unsigned long long>(bytes));
    } catch (...) { link.notify_error(); throw; }
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        usage(); return 0;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        const auto opts = parse(argc, argv);
        std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
        if (opts.sender) { run_sender(opts); } else { run_receiver(opts); }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "UB bytes example: %s\n", error.what());
        return 1;
    }
}
