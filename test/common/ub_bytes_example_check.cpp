// Application-level check: real example, in-memory replacement for UB transport.
// This does not emulate real SDK setup, remote memory or process teardown.
#define main ub_bytes_example_main
#include "../../examples/ub_transport/main.cpp"
#undef main
#include <cassert>
#include <iterator>
#include <mutex>

namespace {
struct mock_state {
    std::mutex mutex;
    std::array<uint8_t, capacity> buffer{};
    // -1: unallocated, 0: Provider, 1: User, 2: queued to Provider,
    // 3: queued to User, 4: released.
    int owner = -1, allocations = 0, releases = 0, acks = 0, copies = 0;
    std::string fault;
    bool injected = false, retried = false;
};
mock_state *mock;
}

struct ktag_demo_ub_ring_transport::implementation { ktag_demo_ub_role role; };
ktag_demo_ub_ring_transport::ktag_demo_ub_ring_transport(std::unique_ptr<implementation> state)
    : state_(std::move(state)) {}
ktag_demo_ub_ring_transport::~ktag_demo_ub_ring_transport() = default;
std::unique_ptr<ktag_demo_ub_ring_transport> ktag_demo_ub_ring_transport::create(
    const ktag_demo_ub_transport_config &config, std::string *) {
    assert(config.data_element_size == capacity && config.queues_per_provider == 1);
    auto state = std::make_unique<implementation>(); state->role = config.role;
    return std::unique_ptr<ktag_demo_ub_ring_transport>(new ktag_demo_ub_ring_transport(std::move(state)));
}
uint32_t ktag_demo_ub_runtime_data_element_size(uint32_t size) { return (size + 255U) & ~127U; }
uint32_t ktag_demo_ub_ring_transport::data_element_size(uint32_t) const {
    return ktag_demo_ub_runtime_data_element_size(capacity);
}
bool ktag_demo_ub_ring_transport::queue_mapping(const std::string &, uint32_t sub,
    ktag_demo_ub_queue_mapping *out, std::string *) const { assert(sub == 0); *out = {}; return true; }
void *ktag_demo_ub_ring_transport::allocate_buffer(uint32_t) {
    std::lock_guard<std::mutex> lock(mock->mutex);
    assert(state_->role == ktag_demo_ub_role::user && mock->owner == -1);
    mock->owner = 1; ++mock->allocations; return mock->buffer.data();
}
void ktag_demo_ub_ring_transport::free_buffer(void *buffer) {
    std::lock_guard<std::mutex> lock(mock->mutex);
    assert(state_->role == ktag_demo_ub_role::user && mock->owner == 1 && buffer == mock->buffer.data());
    mock->owner = 4; ++mock->releases;
}
void ktag_demo_ub_ring_transport::return_buffer(void *buffer) {
    std::lock_guard<std::mutex> lock(mock->mutex);
    assert(state_->role == ktag_demo_ub_role::provider && mock->owner == 0 && buffer == mock->buffer.data());
    mock->owner = 4; ++mock->releases;
}
uint16_t ktag_demo_ub_ring_transport::send_burst(uint32_t queue, void **buffers, uint16_t count) {
    std::lock_guard<std::mutex> lock(mock->mutex);
    const bool provider = state_->role == ktag_demo_ub_role::provider;
    assert(queue == 0 && count == 1 && buffers[0] == mock->buffer.data());
    assert(mock->owner == (provider ? 0 : 1));
    auto *p = mock->buffer.data();
    const auto type = static_cast<kind>(get(p + 12, 4));
    if (type == kind::ack) { ++mock->acks; }
    if (!mock->retried) { mock->retried = true; return 0; } // Full queue; pointer must stay owned.
    if (!mock->injected && type == kind::data && mock->fault == "crc") {
        p[header_size] ^= 1; mock->injected = true;
    } else if (!mock->injected && type == kind::data && mock->fault == "sequence") {
        p[16] ^= 1; mock->injected = true;
    } else if (type == kind::end && mock->fault == "totals") {
        p[header_size] ^= 1; put(p + 28, crc32(p + header_size, 16), 4);
    }
    mock->owner = provider ? 3 : 2;
    return 1;
}
uint16_t ktag_demo_ub_ring_transport::receive_burst(uint32_t queue, void **buffers, uint16_t count) {
    std::lock_guard<std::mutex> lock(mock->mutex);
    const bool provider = state_->role == ktag_demo_ub_role::provider;
    assert(queue == 0 && count == 1);
    if (mock->owner != (provider ? 2 : 3)) { return 0; }
    if (provider && mock->fault == "lost_ack" && get(mock->buffer.data() + 12, 4) == 4) { return 0; }
    mock->owner = provider ? 0 : 1; buffers[0] = mock->buffer.data(); return 1;
}
bool ktag_demo_ub_ring_transport::receive_copy(uint32_t queue, void *local, size_t local_capacity,
    size_t size, ktag_demo_ub_copy_receipt *receipt) {
    assert(state_->role == ktag_demo_ub_role::user && local_capacity >= size && size == capacity);
    void *buffer = nullptr;
    if (!receive_burst(queue, &buffer, 1)) { return false; }
    assert(local != buffer);
    std::memcpy(local, buffer, size); receipt->ub_buffer = buffer;
    ++mock->copies; return true;
}

namespace {
void transfer(size_t size, const std::string &fault = "") {
    mock_state state; state.fault = fault; mock = &state;
    std::string original(size, '\0');
    for (size_t i = 0; i < size; ++i) { original[i] = static_cast<char>((i * 37U) % 256U); }
    { std::ofstream input("input.bin", std::ios::binary); input.write(original.data(), original.size()); }
    options sender, receiver;
    sender.sender = true; sender.input = "input.bin"; sender.provider = "a"; sender.user = "b";
    sender.eal = "ub_bytes --no-pci --lcores='0@(0-3)'"; sender.timeout_ms = 1000;
    receiver = sender; receiver.sender = false; receiver.input.clear(); receiver.output = "output.bin";
    std::exception_ptr send_error, receive_error;
    std::thread user([&] { try { run_receiver(receiver); } catch (...) { receive_error = std::current_exception(); } });
    std::thread provider([&] { try { run_sender(sender); } catch (...) { send_error = std::current_exception(); } });
    provider.join(); user.join();
    if (fault.empty()) {
        if (send_error) { std::rethrow_exception(send_error); }
        if (receive_error) { std::rethrow_exception(receive_error); }
        std::ifstream output("output.bin", std::ios::binary);
        const std::string actual{std::istreambuf_iterator<char>(output), std::istreambuf_iterator<char>()};
        assert(actual == original);
        assert(state.allocations == 1 && state.releases == 1 && state.owner == 4);
        assert(state.acks == 1 && state.copies == int((size + capacity - header_size - 1) / (capacity - header_size) + 1));
    } else {
        assert(send_error);
        assert(fault == "lost_ack" ? !receive_error : bool(receive_error));
        assert(state.owner >= 2); // ERROR/ACK may be left for SDK teardown; neither app holds it.
    }
    std::printf("PASS binary transfer size=%zu fault=%s\n", size, fault.empty() ? "none" : fault.c_str());
}
void protocol_checks() {
    std::array<uint8_t, capacity> frame{};
    assert(crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926U);
    const auto rejected = [](auto operation) {
        bool threw = false; try { operation(); } catch (const std::exception &) { threw = true; }
        assert(threw);
    };
    encode(frame.data(), kind::data, 1, "\0", 1);
    assert(decode(frame.data()).size == 1);
    frame[0] ^= 1; rejected([&] { decode(frame.data()); });
    encode(frame.data(), kind::data, 1, "\0", 1);
    put(frame.data() + 24, capacity, 4); rejected([&] { decode(frame.data()); });
    rejected([&] { encode(frame.data(), kind::data, 1, frame.data(), capacity); });
    encode(frame.data(), kind::credit, 0, "x", 1); rejected([&] { decode(frame.data()); });
    std::puts("PASS protocol CRC vector, magic, length, overflow and control validation");
    mock_state state; mock = &state;
    options opts; opts.sender = true; opts.provider = "a"; opts.user = "b";
    opts.eal = "--no-pci"; rejected([&] { channel link(opts); });
    opts.eal = "ub_bytes 'unterminated"; rejected([&] { channel link(opts); });
    opts.eal = "ub_bytes --no-pci"; opts.timeout_ms = 20;
    channel link(opts); rejected([&] { link.credit(0); });
    interrupted = 1; rejected([&] { link.credit(0); }); interrupted = 0;
    std::puts("PASS EAL argument validation, missing peer timeout and interruption");
}
}

int main() {
    protocol_checks();
    for (const size_t size : {size_t(0), size_t(1), size_t(4063), size_t(4064), size_t(4065),
                              size_t(8128), size_t(8129), size_t(2 * 1024 * 1024)}) { transfer(size); }
    for (const auto *fault : {"crc", "sequence", "totals", "lost_ack"}) { transfer(8129, fault); }
    std::remove("input.bin"); std::remove("output.bin");
}
