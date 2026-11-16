/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ub_ring_transport.h"

#include "ub_feature_wire.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <cstring>
#include <sched.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_memcpy.h>
#include <rte_ub_ring.h>

namespace {

void set_error(std::string *error, const std::string &message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string dpdk_error(const char *operation) {
    const int code = rte_errno;
    return std::string(operation) + " failed: " +
           (code == 0 ? "unknown DPDK error" : rte_strerror(code));
}

bool validate_config(const ktag_demo_ub_transport_config &config,
                     std::string *error) {
    if (config.provider_nodes.empty() || config.user_nodes.size() != 1U) {
        set_error(error,
                  "UB transport requires at least one Provider and one User");
        return false;
    }
    if (config.provider_nodes.size() + config.user_nodes.size() > 256U) {
        set_error(error, "UB transport node count exceeds the DPDK limit");
        return false;
    }
    std::unordered_set<std::string> nodes;
    for (const auto &node : config.provider_nodes) {
        if (node.empty() || !nodes.insert(node).second) {
            set_error(error,
                      "UB Provider hostnames must be non-empty and unique");
            return false;
        }
    }
    for (const auto &node : config.user_nodes) {
        if (node.empty() || !nodes.insert(node).second) {
            set_error(error, "UB User hostname must be non-empty and unique");
            return false;
        }
    }
    if (config.region.empty() || config.shm_prefix.empty()) {
        set_error(error, "UB region and shm prefix are required");
        return false;
    }
    if (config.data_element_size < ktag_demo_ub_feature_frame_size(0U)) {
        set_error(error, "UB data element cannot hold a KTAG Feature frame");
        return false;
    }
    if (config.queues_per_provider == 0U || config.queues_per_provider > 512U ||
        config.provider_nodes.size() > 512U / config.queues_per_provider) {
        set_error(error, "UB total queue count exceeds the DPDK limit");
        return false;
    }
    if (config.provider_shm_size == 0U) {
        set_error(error, "UB Provider shm size is required for both roles");
        return false;
    }
    if (config.queue_depth < 2U ||
        (config.queue_depth & (config.queue_depth - 1U)) != 0U) {
        set_error(
            error,
            "UB Provider shm/queue values are invalid or not power-of-two");
        return false;
    }
    const auto ring_size = rte_ring_get_memsize(config.queue_depth);
    const uint64_t metadata = (sizeof(ub_ring_metadata) +
                              2ULL * config.queues_per_provider * sizeof(uint64_t) + 63U) & ~63ULL;
    const uint32_t stride = ktag_demo_ub_runtime_data_element_size(config.data_element_size);
    // The current provider_init does not check layout bounds before writing
    // its rings. Reject undersized regions before entering the SDK.
    if (ring_size < 0 || stride == 0U || config.provider_shm_size <
        metadata + 2ULL * config.queues_per_provider * static_cast<uint64_t>(ring_size) +
        static_cast<uint64_t>(rte_ring_get_memsize(2U)) + 128U + 2ULL * stride) {
        set_error(error, "UB Provider shm is too small for queue layout and data buffers, or queue_depth is unsupported");
        return false;
    }
    return true;
}

std::vector<const char *> node_pointers(const std::vector<std::string> &nodes) {
    std::vector<const char *> result;
    result.reserve(nodes.size());
    for (const auto &node : nodes) {
        result.push_back(node.c_str());
    }
    return result;
}

bool initialize_eal(const ktag_demo_ub_transport_config &config, bool *owns_eal,
                    std::string *error) {
    if (owns_eal == nullptr) {
        set_error(error, "EAL ownership output is required");
        return false;
    }
    *owns_eal = false;

    std::vector<std::string> arguments = config.eal_arguments;
    if (arguments.empty()) {
        arguments = {"ktag_demo", "--no-huge", "-l", "0"};
    } else if (arguments.front().empty()) {
        set_error(error, "the first DPDK EAL argument must be a program name");
        return false;
    }
    if (arguments.size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
        set_error(error, "too many DPDK EAL arguments");
        return false;
    }

    std::vector<std::vector<char>> storage;
    std::vector<char *> argv;
    try {
        storage.reserve(arguments.size());
        argv.reserve(arguments.size());
        for (const auto &argument : arguments) {
            storage.emplace_back(argument.begin(), argument.end());
            storage.back().push_back('\0');
        }
        for (auto &argument : storage) {
            argv.push_back(argument.data());
        }
    } catch (...) {
        set_error(error, "out of memory preparing DPDK EAL arguments");
        return false;
    }
    const int argc = static_cast<int>(argv.size());
    argv.push_back(nullptr);
    const int rc = rte_eal_init(argc, argv.data());
    if (rc < 0 && rte_errno == EALREADY) {
        /* Another KTAG component (for example DPDK capture) owns EAL. */
        return true;
    }
    if (rc < 0) {
        set_error(error, dpdk_error("rte_eal_init"));
        return false;
    }
    *owns_eal = true;
    return true;
}

} // namespace

bool ktag_demo_ub_allowed_cpus(std::vector<uint32_t> *cpus, std::string *error) {
    for (size_t bits = 1024U; bits <= 1048576U; bits *= 2U) {
        std::vector<unsigned long> mask(bits / (8U * sizeof(unsigned long)), 0UL);
        if (sched_getaffinity(0, mask.size() * sizeof(unsigned long),
                              reinterpret_cast<cpu_set_t *>(mask.data())) == 0) {
            cpus->clear();
            for (size_t cpu = 0; cpu < bits; ++cpu) {
                if ((mask[cpu / (8U * sizeof(unsigned long))] >>
                     (cpu % (8U * sizeof(unsigned long)))) & 1UL) {
                    cpus->push_back(static_cast<uint32_t>(cpu));
                }
            }
            return !cpus->empty();
        }
        if (errno != EINVAL) { break; }
    }
    set_error(error, std::string("cannot read pre-EAL CPU affinity: ") + std::strerror(errno));
    return false;
}

bool ktag_demo_ub_bind_cpu(uint32_t cpu, std::string *error) {
    // Linux accepts a short setaffinity mask; the kernel zero-extends it.
    const size_t word_bits = 8U * sizeof(unsigned long);
    std::vector<unsigned long> mask(static_cast<size_t>(cpu) / word_bits + 1U, 0UL);
    mask[cpu / word_bits] = 1UL << (cpu % word_bits);
    if (sched_setaffinity(0, mask.size() * sizeof(unsigned long),
                          reinterpret_cast<cpu_set_t *>(mask.data())) == 0) {
        std::vector<uint32_t> actual;
        if (ktag_demo_ub_allowed_cpus(&actual, error) && actual.size() == 1U && actual.front() == cpu) {
            return true;
        }
    }
    set_error(error, "cannot bind worker to CPU " + std::to_string(cpu) + ": " + std::strerror(errno));
    return false;
}

uint32_t ktag_demo_ub_runtime_data_element_size(uint32_t payload_size) {
#if defined(UB_RING_BUF_HDR_SIZE)
    /* rte_ub_ring 24.11 aligns (payload + ownership header) to 128 bytes. */
    constexpr uint64_t kAlignment = 128U;
    const uint64_t runtime_size =
        static_cast<uint64_t>(payload_size) + UB_RING_BUF_HDR_SIZE;
    if (runtime_size > std::numeric_limits<uint32_t>::max()) {
        return 0U;
    }
    const uint64_t aligned_size =
        (runtime_size + (kAlignment - 1U)) & ~(kAlignment - 1U);
    if (aligned_size > std::numeric_limits<uint32_t>::max()) {
        return 0U;
    }
    return static_cast<uint32_t>(aligned_size);
#else
    /* DPDK/UB 24.1 and earlier had no ownership header. */
    return payload_size;
#endif
}

struct ktag_demo_ub_ring_transport::implementation {
    ~implementation() {
        if (handle != nullptr) {
            rte_ub_ring_fini(handle);
        }
        if (owns_eal) {
            rte_eal_cleanup();
        }
    }

    ktag_demo_ub_role role = ktag_demo_ub_role::provider;
    rte_ub_ring_handle *handle = nullptr;
    bool owns_eal = false;
    size_t provider_shm_size = 0U;
    uint32_t payload_capacity = 0U;
};

ktag_demo_ub_ring_transport::ktag_demo_ub_ring_transport(
    std::unique_ptr<implementation> state)
    : state_(std::move(state)) {}

ktag_demo_ub_ring_transport::~ktag_demo_ub_ring_transport() = default;

std::unique_ptr<ktag_demo_ub_ring_transport>
ktag_demo_ub_ring_transport::create(const ktag_demo_ub_transport_config &config,
                                    std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!validate_config(config, error)) {
        return nullptr;
    }
    std::unique_ptr<implementation> state(new (std::nothrow) implementation());
    if (!state) {
        set_error(error, "out of memory creating UB transport");
        return nullptr;
    }
    state->role = config.role;
    state->provider_shm_size = config.provider_shm_size;
    state->payload_capacity = config.data_element_size;
    bool owns_eal = false;
    if (!initialize_eal(config, &owns_eal, error)) {
        return nullptr;
    }
    state->owns_eal = owns_eal;

    auto provider_nodes = node_pointers(config.provider_nodes);
    auto user_nodes = node_pointers(config.user_nodes);
    if (config.role == ktag_demo_ub_role::provider) {
        state->handle = rte_ub_ring_provider_init(
            provider_nodes.data(), static_cast<uint32_t>(provider_nodes.size()),
            user_nodes.data(), static_cast<uint32_t>(user_nodes.size()),
            config.region.c_str(), config.shm_prefix.c_str(),
            config.provider_shm_size, config.queues_per_provider,
            config.queue_depth);
    } else {
        state->handle = rte_ub_ring_user_init(
            provider_nodes.data(), static_cast<uint32_t>(provider_nodes.size()),
            user_nodes.data(), static_cast<uint32_t>(user_nodes.size()),
            config.region.c_str(), config.shm_prefix.c_str(),
            config.data_element_size);
    }
    if (state->handle == nullptr) {
        set_error(error, dpdk_error(config.role == ktag_demo_ub_role::provider
                                        ? "rte_ub_ring_provider_init"
                                        : "rte_ub_ring_user_init"));
        return nullptr;
    }

    if (config.role == ktag_demo_ub_role::user) {
        std::unordered_set<uint32_t> tx_ids, rx_ids;
        for (const auto &hostname : config.provider_nodes) {
            uint32_t pi = 0U;
            if (rte_ub_ring_get_peer_index(state->handle, hostname.c_str(), &pi) < 0) {
                set_error(error, "cannot map UB Provider " + hostname);
                return nullptr;
            }
            uint32_t actual = 0U, tx = 0U, rx = 0U;
            while (actual <= 512U &&
                   rte_ub_ring_get_queue_mapping(state->handle, pi, actual, &tx, &rx) == 0) {
                if (!tx_ids.insert(tx).second || !rx_ids.insert(rx).second) {
                    set_error(error, "duplicate UB queue mapping for " + hostname);
                    return nullptr;
                }
                ++actual;
            }
            if (actual != config.queues_per_provider) {
                set_error(error, "UB queue layout mismatch: provider=" + hostname +
                    ", expected_queues=" + std::to_string(config.queues_per_provider) +
                    ", actual_queues=" + std::to_string(actual));
                return nullptr;
            }
        }
        if (rte_ub_ring_get_total_queues(state->handle) !=
            config.provider_nodes.size() * config.queues_per_provider) {
            set_error(error, "UB actual total queue count does not match YAML");
            return nullptr;
        }
    }

    auto *transport =
        new (std::nothrow) ktag_demo_ub_ring_transport(std::move(state));
    if (transport == nullptr) {
        set_error(error, "out of memory finalizing UB transport");
        return nullptr;
    }
    return std::unique_ptr<ktag_demo_ub_ring_transport>(transport);
}

ktag_demo_ub_role ktag_demo_ub_ring_transport::role() const {
    return state_->role;
}

bool ktag_demo_ub_ring_transport::node_index(uint32_t *index,
                                             std::string *error) const {
    if (index == nullptr) {
        set_error(error, "UB node-index output is required");
        return false;
    }
    if (rte_ub_ring_get_node_index(state_->handle, index) < 0) {
        set_error(error, "rte_ub_ring_get_node_index failed");
        return false;
    }
    return true;
}

bool ktag_demo_ub_ring_transport::queue_mapping(
    const std::string &provider_hostname, uint32_t sub_queue,
    ktag_demo_ub_queue_mapping *mapping, std::string *error) const {
    if (provider_hostname.empty() || mapping == nullptr) {
        set_error(error, "UB Provider hostname and queue output are required");
        return false;
    }
    ktag_demo_ub_queue_mapping result;
    if (rte_ub_ring_get_peer_index(state_->handle, provider_hostname.c_str(),
                                   &result.provider_node_index) < 0) {
        set_error(error,
                  "rte_ub_ring_get_peer_index failed for " + provider_hostname);
        return false;
    }
    if (rte_ub_ring_get_queue_mapping(
            state_->handle, result.provider_node_index, sub_queue,
            &result.tx_queue_id, &result.rx_queue_id) < 0) {
        set_error(error, "rte_ub_ring_get_queue_mapping failed for " +
                             provider_hostname);
        return false;
    }
    *mapping = result;
    return true;
}

uint32_t ktag_demo_ub_ring_transport::total_queues() const {
    return rte_ub_ring_get_total_queues(state_->handle);
}

uint32_t ktag_demo_ub_ring_transport::data_element_size(
    uint32_t provider_node_index) const {
    return rte_ub_ring_get_data_elt_size(state_->handle, provider_node_index);
}

void *
ktag_demo_ub_ring_transport::allocate_buffer(uint32_t provider_node_index) {
    if (state_->role != ktag_demo_ub_role::user) {
        return nullptr;
    }
    return rte_ub_ring_alloc_buf(state_->handle, provider_node_index);
}

uint16_t ktag_demo_ub_ring_transport::send_burst(uint32_t queue_id,
                                                 void **buffers,
                                                 uint16_t count) {
    if (buffers == nullptr || count == 0U) {
        return 0U;
    }
    // The application owns local payload pointers. DPDK queues carry packed
    // {provider, offset}, never a virtual address from either process.
    // Use bounded scratch space and preserve the caller's pointers on partial
    // sends: the caller may retry or free the unsent buffers.
    std::array<void *, 64> elements{};
    const uint16_t batch = std::min<uint16_t>(count, elements.size());
    for (uint16_t i = 0; i < batch; ++i) {
        if (buffers[i] == nullptr) {
            throw std::runtime_error("UB send buffer is null");
        }
        const auto *header = ub_ring_buf_hdr(buffers[i]);
        if (header->magic != UB_RING_BUF_MAGIC) {
            throw std::runtime_error("UB send buffer ownership header is invalid");
        }
        elements[i] = ub_ring_elem_pack(header->node_index, header->offset);
    }
    return rte_ub_ring_send_burst(state_->handle, queue_id, elements.data(), batch);
}

uint16_t ktag_demo_ub_ring_transport::receive_burst(uint32_t queue_id,
                                                    void **buffers,
                                                    uint16_t count) {
    if (buffers == nullptr || count == 0U) {
        return 0U;
    }
    const uint16_t received =
        rte_ub_ring_recv_burst(state_->handle, queue_id, buffers, count);
    for (uint16_t i = 0; i < received; ++i) {
        buffers[i] = resolve_received_element(buffers[i]);
    }
    return received;
}

void *ktag_demo_ub_ring_transport::resolve_received_element(void *element) const {
    const uint32_t provider = ub_ring_elem_get_node(element);
    const uint64_t offset = ub_ring_elem_get_offset(element);
    void *area = rte_ub_ring_get_data_buf_area(state_->handle, provider);
    const uint32_t stride =
        rte_ub_ring_get_data_elt_size(state_->handle, provider);
    // Keep the existing packed-element validation for both receive paths.
    // The shm size is an upper bound, not the exact buffer-area size.
    if (area == nullptr || stride <= UB_RING_BUF_HDR_SIZE ||
        offset == 0U || offset % stride != 0U ||
        state_->provider_shm_size < stride ||
        offset > state_->provider_shm_size - stride) {
        throw std::runtime_error("UB received invalid packed buffer element");
    }
    return ub_ring_buf_ptr(area, offset);
}

bool ktag_demo_ub_ring_transport::receive_copy(
    uint32_t queue_id, void *local_buffer, size_t local_capacity,
    size_t copy_size, ktag_demo_ub_copy_receipt *receipt) {
    if (receipt == nullptr) {
        throw std::invalid_argument("UB copy receipt is required");
    }
    *receipt = {};
    if (state_->role != ktag_demo_ub_role::user ||
        local_buffer == nullptr || copy_size == 0U ||
        queue_id >= total_queues()) {
        throw std::invalid_argument("invalid UB copy receive arguments");
    }
    const uint32_t provider =
        rte_ub_ring_queue_to_provider(state_->handle, queue_id);
    const uint32_t stride = data_element_size(provider);
    if (copy_size > local_capacity || copy_size > state_->payload_capacity ||
        stride <= UB_RING_BUF_HDR_SIZE ||
        copy_size > stride - UB_RING_BUF_HDR_SIZE) {
        // Reject capacity errors before consuming a queue element.
        throw std::length_error("UB copy exceeds local or UB payload capacity");
    }

    void *element = nullptr;
    if (rte_ub_ring_recv_burst(state_->handle, queue_id, &element, 1U) == 0U) {
        return false;
    }
    const auto received_at = std::chrono::steady_clock::now();
    if (ub_ring_elem_get_node(element) != provider) {
        throw std::runtime_error("UB received element from wrong Provider");
    }
    void *buffer = resolve_received_element(element);
    const auto destination = reinterpret_cast<uintptr_t>(local_buffer);
    const auto source = reinterpret_cast<uintptr_t>(buffer);
    if ((destination >= source && destination - source < copy_size) ||
        (source > destination && source - destination < copy_size)) {
        free_buffer(buffer);
        throw std::invalid_argument("UB copy source and destination overlap");
    }

    rte_memcpy(local_buffer, buffer, copy_size);
    receipt->ub_buffer = buffer;
    receipt->received_at = received_at;
    return true;
}

void ktag_demo_ub_ring_transport::free_buffer(void *buffer) {
    rte_ub_ring_free_buf(state_->handle, buffer);
}

void ktag_demo_ub_ring_transport::return_buffer(void *buffer) {
    if (state_->role == ktag_demo_ub_role::provider) {
        rte_ub_ring_return_buf(state_->handle, buffer);
    }
}

void ktag_demo_ub_ring_transport::collect_returned_buffers() {
    if (state_->role == ktag_demo_ub_role::user) {
        rte_ub_ring_collect(state_->handle);
    }
}
