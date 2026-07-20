/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include "decode.h"
#include "feature_pipeline_internal.h"
#include "flow_hash.h"

namespace {

constexpr uint64_t FEATURE_FLOW_HASH_SEED = 0x4b544147464c4f57ULL;
constexpr uint64_t FEATURE_FLOW_HASH_GOLDEN_RATIO = 0x9e3779b97f4a7c15ULL;
constexpr uint32_t FEATURE_FLOW_INITIAL_CAPACITY = 10000U;
constexpr uint16_t ETHER_TYPE_IPV4 = 0x0800U;

uint64_t load_flow_address_word(
    const uint8_t *address,
    size_t length) noexcept
{
    uint64_t value = 0U;
    std::memcpy(&value, address, length);
    return value;
}

void combine_flow_hash(uint64_t *hash, uint64_t value) noexcept
{
    *hash ^= value + FEATURE_FLOW_HASH_GOLDEN_RATIO +
        (*hash << 6U) + (*hash >> 2U);
}

uint64_t finalize_flow_hash(uint64_t hash) noexcept
{
    hash ^= hash >> 30U;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27U;
    hash *= 0x94d049bb133111ebULL;
    return hash ^ (hash >> 31U);
}

enum class flow_finish_reason {
    protocol,
    tcp_expire,
    udp_expire,
    flush
};

struct feature_flow_key_hash {
    size_t operator()(const ktag_flow_key &key) const noexcept
    {
        const size_t address_length = key.address_a.version == 4U ?
            sizeof(uint32_t) : sizeof(key.address_a.bytes);
        const size_t first_word_length =
            std::min(address_length, sizeof(uint64_t));
        uint64_t hash = FEATURE_FLOW_HASH_SEED;
        combine_flow_hash(
            &hash,
            load_flow_address_word(
                key.address_a.bytes, first_word_length));
        if (address_length > sizeof(uint64_t)) {
            combine_flow_hash(
                &hash,
                load_flow_address_word(
                    key.address_a.bytes + sizeof(uint64_t), sizeof(uint64_t)));
        }
        combine_flow_hash(
            &hash,
            load_flow_address_word(
                key.address_b.bytes, first_word_length));
        if (address_length > sizeof(uint64_t)) {
            combine_flow_hash(
                &hash,
                load_flow_address_word(
                    key.address_b.bytes + sizeof(uint64_t), sizeof(uint64_t)));
        }
        const uint64_t metadata =
            static_cast<uint64_t>(key.port_a) |
            (static_cast<uint64_t>(key.port_b) << 16U) |
            (static_cast<uint64_t>(key.protocol) << 32U) |
            (static_cast<uint64_t>(key.address_a.version) << 40U);
        combine_flow_hash(&hash, metadata);
        return static_cast<size_t>(finalize_flow_hash(hash));
    }
};

struct feature_flow_key_equal {
    bool operator()(const ktag_flow_key &first, const ktag_flow_key &second) const noexcept
    {
        if (first.address_a.version != second.address_a.version ||
            first.address_b.version != second.address_b.version ||
            first.port_a != second.port_a || first.port_b != second.port_b ||
            first.protocol != second.protocol) {
            return false;
        }
        const size_t address_length = first.address_a.version == 4U ?
            sizeof(uint32_t) : sizeof(first.address_a.bytes);
        return std::memcmp(
                first.address_a.bytes,
                second.address_a.bytes,
                address_length) == 0 &&
            std::memcmp(
                first.address_b.bytes,
                second.address_b.bytes,
                address_length) == 0;
    }
};

struct feature_context_flow {
    ktag_flow_key key{};
    flow_hash_node node{};
    ktag_feature_flow_runtime runtime{};
    uint64_t flow_id = 0U;
    uint64_t first_packet_sequence = 0U;
    uint64_t last_packet_sequence = 0U;
    uint64_t last_seen_us = 0U;
    uint64_t domain_lookup_version = 0U;
    bool domain_lookup_initialized = false;

    ~feature_context_flow()
    {
        ktag_feature_flow_runtime_destroy_internal(&runtime);
    }
};

using feature_flow_table = std::unordered_map<
    ktag_flow_key,
    std::unique_ptr<feature_context_flow>,
    feature_flow_key_hash,
    feature_flow_key_equal>;

uint16_t read_be16(const uint8_t *data)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8U) |
        static_cast<uint16_t>(data[1]));
}

size_t current_thread_token()
{
    static std::atomic<size_t> next_token{1U};
    thread_local const size_t token =
        next_token.fetch_add(1U, std::memory_order_relaxed);
    return token == 0U ? std::numeric_limits<size_t>::max() : token;
}

ktag_status translate_decode_status(KDADKResult status)
{
    switch (status) {
        case KDADK_STS_NO_ERR:
            return KTAG_SUCCESS;
        case KDADK_STS_NOT_SUPPORT:
            return KTAG_ERROR_NOT_SUPPORTED;
        case KDADK_STS_MALLOC_FAILED:
            return KTAG_ERROR_OUT_OF_MEMORY;
        case KDADK_STS_VECTOR_INDEX_OVERFLOW:
            return KTAG_ERROR_CAPACITY;
        case KDADK_STS_NULL_PTR_ERR:
        case KDADK_STS_PARAMETER_ERR:
        case KDADK_STS_INVALID_PCAP_PACKET:
            return KTAG_ERROR_INVALID_ARGUMENT;
        default:
            return KTAG_ERROR_INTERNAL;
    }
}

KDADKResult translate_domain_status(ktag_status status)
{
    switch (status) {
        case KTAG_SUCCESS:
            return KDADK_STS_NO_ERR;
        case KTAG_ERROR_INVALID_ARGUMENT:
            return KDADK_STS_PARAMETER_ERR;
        case KTAG_ERROR_OUT_OF_MEMORY:
            return KDADK_STS_MALLOC_FAILED;
        case KTAG_ERROR_CAPACITY:
            return KDADK_STS_VECTOR_INDEX_OVERFLOW;
        default:
            return KDADK_STS_UNKNOWN_ERR;
    }
}

bool valid_feature_config(const ktag_feature_config *config)
{
    const size_t legacy_config_size =
        offsetof(ktag_feature_config, lifecycle_mode);
    if (config == nullptr || config->struct_size < legacy_config_size ||
        config->api_version != KTAG_API_VERSION || config->schema == nullptr ||
        config->registry == nullptr ||
        ktag_feature_schema_is_frozen(config->schema) == 0U ||
        ktag_feature_schema_get_value_count(config->schema) == 0U) {
        return false;
    }
    if (config->struct_size >= sizeof(*config) &&
        config->lifecycle_mode != KTAG_FEATURE_LIFECYCLE_MODERN &&
        config->lifecycle_mode != KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT) {
        return false;
    }
    return true;
}

ktag_status validate_packet_decoder_bounds(const ktag_packet *packet)
{
    if (packet == nullptr || packet->struct_size < sizeof(*packet) ||
        packet->api_version != KTAG_API_VERSION || packet->data == nullptr ||
        packet->captured_length == 0U || packet->timestamp.seconds < 0 ||
        packet->timestamp.microseconds < 0 ||
        packet->timestamp.microseconds > 999999) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }

    uint32_t network_offset = 0U;
    if (packet->link_type == KTAG_CAPTURE_LINKTYPE_ETHERNET) {
        if (packet->captured_length < 14U) {
            return KTAG_ERROR_INVALID_ARGUMENT;
        }
        if (read_be16(packet->data + 12U) != ETHER_TYPE_IPV4) {
            return KTAG_ERROR_NOT_SUPPORTED;
        }
        network_offset = 14U;
    } else if (packet->link_type == KTAG_CAPTURE_LINKTYPE_LINUX_SLL) {
        if (packet->captured_length < 16U) {
            return KTAG_ERROR_INVALID_ARGUMENT;
        }
        if (read_be16(packet->data + 14U) != ETHER_TYPE_IPV4) {
            return KTAG_ERROR_NOT_SUPPORTED;
        }
        network_offset = 16U;
    } else {
        return KTAG_ERROR_NOT_SUPPORTED;
    }

    if (packet->captured_length - network_offset < 20U) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const uint8_t *network = packet->data + network_offset;
    if ((network[0] >> 4U) != 4U) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }
    const uint32_t ip_header_length =
        static_cast<uint32_t>(network[0] & 0x0fU) * 4U;
    const uint32_t ip_total_length = read_be16(network + 2U);
    const uint32_t network_available = packet->captured_length - network_offset;
    if (ip_header_length < 20U || ip_header_length > network_available ||
        ip_total_length < ip_header_length || ip_total_length > network_available) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t transport_length = ip_total_length - ip_header_length;
    const uint8_t *transport = network + ip_header_length;
    if (network[9U] == IPPROTO_TCP) {
        if (transport_length < 20U) {
            return KTAG_ERROR_INVALID_ARGUMENT;
        }
        const uint32_t tcp_header_length =
            static_cast<uint32_t>(transport[12U] >> 4U) * 4U;
        return tcp_header_length >= 20U && tcp_header_length <= transport_length ?
            KTAG_SUCCESS : KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (network[9U] == IPPROTO_UDP) {
        return transport_length >= 8U ?
            KTAG_SUCCESS : KTAG_ERROR_INVALID_ARGUMENT;
    }
    return KTAG_ERROR_NOT_SUPPORTED;
}

uint64_t packet_timestamp_us(const ktag_packet &packet)
{
    const uint64_t seconds = static_cast<uint64_t>(packet.timestamp.seconds);
    const uint64_t microseconds =
        static_cast<uint64_t>(packet.timestamp.microseconds);
    if (seconds >
        (std::numeric_limits<uint64_t>::max() - microseconds) / 1000000U) {
        return std::numeric_limits<uint64_t>::max();
    }
    return seconds * 1000000U + microseconds;
}

void initialize_legacy_node_tuple(
    flow_hash_node *node,
    const packet_info &info)
{
    if (info.tuple.src_ip <= info.tuple.dst_ip) {
        node->tuple.src_ip = info.tuple.src_ip;
        node->tuple.src_port = info.tuple.src_port;
        node->tuple.dst_ip = info.tuple.dst_ip;
        node->tuple.dst_port = info.tuple.dst_port;
    } else {
        node->tuple.src_ip = info.tuple.dst_ip;
        node->tuple.src_port = info.tuple.dst_port;
        node->tuple.dst_ip = info.tuple.src_ip;
        node->tuple.dst_port = info.tuple.src_port;
    }
    node->tuple.protocol = info.tuple.protocol;
    node->flow_src_ip = info.tuple.src_ip;
}

ktag_ip_address make_ipv4_address(uint32_t network_order_address)
{
    ktag_ip_address address{};
    std::memcpy(
        address.bytes,
        &network_order_address,
        sizeof(network_order_address));
    address.version = 4U;
    return address;
}

} // namespace

struct ktag_feature_context {
    ktag_feature_pipeline *pipeline = nullptr;
    ktag_domain_store *domain_store = nullptr;
    ktag_domain_cache *domain_cache = nullptr;
    bool owns_domain_store = false;
    feature_flow_table flows;
    std::vector<float> output_values;
    uint32_t flow_capacity = KTAG_FEATURE_DEFAULT_FLOW_CAPACITY;
    uint64_t tcp_idle_timeout_us = KTAG_FEATURE_DEFAULT_TCP_IDLE_TIMEOUT_US;
    uint64_t udp_idle_timeout_us = KTAG_FEATURE_DEFAULT_UDP_IDLE_TIMEOUT_US;
    uint32_t udp_packet_limit = KTAG_FEATURE_DEFAULT_UDP_PACKET_LIMIT;
    ktag_feature_lifecycle_mode lifecycle_mode =
        KTAG_FEATURE_LIFECYCLE_MODERN;
    uint64_t next_flow_id = 1U;
    ktag_feature_statistics stats{};
    std::atomic<size_t> owner_thread{0U};
    std::atomic<uint64_t> thread_violations{0U};

    ~ktag_feature_context()
    {
        flows.clear();
        ktag_feature_pipeline_destroy_internal(pipeline);
        pipeline = nullptr;
        ktag_domain_cache_destroy(domain_cache);
        domain_cache = nullptr;
        if (owns_domain_store) {
            ktag_domain_store_destroy(domain_store);
        }
        domain_store = nullptr;
    }
};

namespace {

bool check_and_bind_owner(ktag_feature_context *context)
{
    const size_t token = current_thread_token();
    const size_t owner = context->owner_thread.load(std::memory_order_acquire);
    if (owner == token) {
        return true;
    }
    if (owner != 0U) {
        context->thread_violations.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    size_t expected = 0U;
    if (context->owner_thread.compare_exchange_strong(
            expected, token, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }
    if (expected == token) {
        return true;
    }
    context->thread_violations.fetch_add(1U, std::memory_order_relaxed);
    return false;
}

ktag_status prepare_output(
    ktag_feature_context *context,
    ktag_feature_batch *output)
{
    if (context == nullptr || output == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (!check_and_bind_owner(context)) {
        return KTAG_ERROR_INVALID_STATE;
    }
    if (ktag_feature_batch_get_schema_hash(output) !=
            context->pipeline->schema->schema_hash ||
        ktag_feature_batch_get_value_count(output) !=
            context->pipeline->schema->value_count) {
        return KTAG_ERROR_SCHEMA_MISMATCH;
    }
    ktag_feature_batch_clear(output);
    return KTAG_SUCCESS;
}

KDADKResult context_domain_update(
    void *user_data,
    uint32_t network_order_address,
    const std::string &domain,
    uint64_t expire_at_us)
{
    auto *context = static_cast<ktag_feature_context *>(user_data);
    if (context == nullptr || context->domain_store == nullptr ||
        context->domain_cache == nullptr) {
        return KDADK_STS_NULL_PTR_ERR;
    }
    const ktag_ip_address address = make_ipv4_address(network_order_address);
    ktag_status status = ktag_domain_store_put(
        context->domain_store,
        &address,
        domain.c_str(),
        expire_at_us,
        nullptr);
    if (status == KTAG_SUCCESS) {
        status = ktag_domain_cache_sync(context->domain_cache);
    }
    return translate_domain_status(status);
}

ktag_status lookup_domain(
    ktag_feature_context *context,
    uint32_t network_order_address,
    uint64_t now_us,
    std::string *domain,
    bool *found)
{
    *found = false;
    const ktag_ip_address address = make_ipv4_address(network_order_address);
    char value[KTAG_DOMAIN_NAME_MAX_LENGTH + 1U] = {};
    uint32_t required_capacity = 0U;
    const ktag_status status = ktag_domain_cache_lookup(
        context->domain_cache,
        &address,
        now_us,
        value,
        sizeof(value),
        &required_capacity);
    if (status == KTAG_ERROR_NOT_FOUND) {
        return KTAG_SUCCESS;    /* 解析失败跳过 */
    }
    if (status != KTAG_SUCCESS) {
        return status;
    }
    *domain = value;
    *found = true;
    return KTAG_SUCCESS;
}

ktag_status refresh_flow_domains(
    ktag_feature_context *context,
    feature_context_flow *flow,
    bool add_new_tokens)
{
    const bool already_had_domain =
        !flow->node.src_ip_domain_name.empty() ||
        !flow->node.dst_ip_domain_name.empty();
    flow->node.src_ip_domain_name.clear();
    flow->node.dst_ip_domain_name.clear();

    bool found_source = false;
    bool found_destination = false;
    ktag_status status = lookup_domain(
        context,
        flow->node.tuple.src_ip,
        flow->last_seen_us,
        &flow->node.src_ip_domain_name,
        &found_source);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    status = lookup_domain(
        context,
        flow->node.tuple.dst_ip,
        flow->last_seen_us,
        &flow->node.dst_ip_domain_name,
        &found_destination);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    flow->domain_lookup_version =
        ktag_domain_cache_get_applied_version(context->domain_cache);
    flow->domain_lookup_initialized = true;
    if (add_new_tokens && !already_had_domain &&
        (found_source || found_destination)) {
        add_rawbow_to_node(nullptr, &flow->node, flow->node.src_ip_domain_name);
        add_rawbow_to_node(nullptr, &flow->node, flow->node.dst_ip_domain_name);
    }
    return KTAG_SUCCESS;
}

ktag_status process_flow_rawbow(
    ktag_feature_context *context,
    feature_context_flow *flow,
    const packet_info &info)
{
    if (flow->node.rawbow_cnt >= MAX_RAWBOW_CNT) {
        return KTAG_SUCCESS;
    }
    if (flow->node.src_ip_domain_name.empty() &&
        flow->node.dst_ip_domain_name.empty()) {
        const uint64_t published_version =
            ktag_domain_store_get_version(context->domain_store);
        if (!flow->domain_lookup_initialized ||
            flow->domain_lookup_version != published_version) {
            const ktag_status status =
                refresh_flow_domains(context, flow, true);
            if (status != KTAG_SUCCESS) {
                return status;
            }
        }
    }
    if (!info.http_url.empty()) {
        add_rawbow_to_node(nullptr, &flow->node, info.http_url);
    }
    return KTAG_SUCCESS;
}

void record_finish_reason(
    ktag_feature_context *context,
    flow_finish_reason reason)
{
    ++context->stats.completed_flows;
    switch (reason) {
        case flow_finish_reason::tcp_expire:
            ++context->stats.expired_tcp_flows;
            break;
        case flow_finish_reason::udp_expire:
            ++context->stats.expired_udp_flows;
            break;
        case flow_finish_reason::flush:
            ++context->stats.flushed_flows;
            break;
        case flow_finish_reason::protocol:
            break;
    }
}

ktag_status finish_flow(
    ktag_feature_context *context,
    feature_context_flow *flow,
    flow_finish_reason reason,
    ktag_feature_batch *output)
{
    try {
        record_finish_reason(context, reason);
        ktag_status status = refresh_flow_domains(context, flow, true);
        if (status != KTAG_SUCCESS) {
            ++context->stats.dropped_flows;
            return status;
        }

        status = ktag_feature_flow_runtime_finish_internal(
            &flow->runtime,
            context->output_values.data(),
            static_cast<uint32_t>(context->output_values.size()));
        if (status != KTAG_SUCCESS) {
            ++context->stats.callback_failures;
            ++context->stats.dropped_flows;
            return status;
        }

        ktag_feature_record_metadata metadata{};
        metadata.struct_size = sizeof(metadata);
        metadata.api_version = KTAG_API_VERSION;
        metadata.flow_id = flow->flow_id;
        metadata.flow_key = flow->key;
        metadata.first_packet_sequence = flow->first_packet_sequence;
        metadata.last_packet_sequence = flow->last_packet_sequence;
        status = ktag_feature_batch_append(
            output,
            &metadata,
            context->output_values.data(),
            static_cast<uint32_t>(context->output_values.size()));
        if (status == KTAG_SUCCESS) {
            ++context->stats.emitted_flows;
        } else {
            ++context->stats.dropped_flows;
        }
        return status;
    } catch (const std::bad_alloc &) {
        ++context->stats.dropped_flows;
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        ++context->stats.dropped_flows;
        return KTAG_ERROR_INTERNAL;
    }
}

bool flow_is_expired(
    const ktag_feature_context *context,
    const feature_context_flow &flow,
    uint64_t current_time_us)
{
    if (current_time_us < flow.last_seen_us) {
        return false;
    }
    const uint64_t timeout = flow.key.protocol == IPPROTO_TCP ?
        context->tcp_idle_timeout_us : context->udp_idle_timeout_us;
    return timeout != 0U && current_time_us - flow.last_seen_us >= timeout;
}

ktag_status create_flow(
    ktag_feature_context *context,
    const ktag_flow_key &key,
    const packet_info &info,
    const ktag_packet &packet,
    uint64_t now_us,
    feature_flow_table::iterator *created)
{
    if (context->flows.size() >= context->flow_capacity) {
        ++context->stats.capacity_rejections;
        return KTAG_ERROR_CAPACITY;
    }
    if (context->next_flow_id == std::numeric_limits<uint64_t>::max()) {
        return KTAG_ERROR_CAPACITY;
    }

    std::unique_ptr<feature_context_flow> flow(
        new (std::nothrow) feature_context_flow());
    if (!flow) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    }
    flow->key = key;
    flow->flow_id = context->next_flow_id;
    flow->first_packet_sequence = packet.sequence;
    flow->last_packet_sequence = packet.sequence;
    flow->last_seen_us = now_us;
    initialize_legacy_node_tuple(&flow->node, info);
    add_packet_to_flow_node(&flow->node, &info);

    ktag_status status = process_flow_rawbow(context, flow.get(), info);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    status = ktag_feature_flow_runtime_create_internal(
        context->pipeline,
        &flow->node,
        flow->flow_id,
        &flow->runtime);
    if (status != KTAG_SUCCESS) {
        ++context->stats.callback_failures;
        ++context->stats.dropped_flows;
        return status;
    }

    const auto inserted = context->flows.emplace(key, std::move(flow));
    if (!inserted.second) {
        return KTAG_ERROR_ALREADY_EXISTS;
    }
    ++context->next_flow_id;
    ++context->stats.created_flows;
    context->stats.peak_active_flows = std::max(
        context->stats.peak_active_flows,
        static_cast<uint32_t>(context->flows.size()));
    *created = inserted.first;
    return KTAG_SUCCESS;
}

ktag_status process_packet_internal(
    ktag_feature_context *context,
    const ktag_packet *packet,
    ktag_feature_batch *output)
{
    ktag_status status = prepare_output(context, output);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    status = validate_packet_decoder_bounds(packet);
    if (status != KTAG_SUCCESS) {
        ++context->stats.rejected_packets;
        return status;
    }

    ++context->stats.received_packets;
    context->stats.received_bytes += packet->captured_length;
    ktag_flow_key key{};
    status = ktag_packet_get_flow_key(packet, &key);
    if (status != KTAG_SUCCESS) {
        ++context->stats.rejected_packets;
        if (status == KTAG_ERROR_NOT_FOUND) {
            return KTAG_SUCCESS;
        }
        return status;
    }

    packet_info info{};
    info.packet_size = packet->captured_length;
    if (static_cast<uint64_t>(packet->timestamp.seconds) >
        static_cast<uint64_t>(std::numeric_limits<time_t>::max())) {
        ++context->stats.rejected_packets;
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    info.ts.tv_sec = static_cast<time_t>(packet->timestamp.seconds);
    info.ts.tv_usec = packet->timestamp.microseconds;
    const KDADKResult decode_status = decode_link_layer_with_domain_callback(
        packet->data,
        &info,
        static_cast<int>(packet->link_type),
        context_domain_update,
        context);
    if (decode_status != KDADK_STS_NO_ERR) {
        ++context->stats.rejected_packets;
        return translate_decode_status(decode_status);
    }
    ++context->stats.decoded_packets;

    const uint64_t now_us = packet_timestamp_us(*packet);
    auto position = context->flows.find(key);
    const bool tcp_control_without_flow =
        key.protocol == IPPROTO_TCP &&
        (info.tcp_flag & (TH_RST | TH_FIN)) != 0U &&
        position == context->flows.end();
    if (tcp_control_without_flow) {
        return KTAG_SUCCESS;
    }

    bool packet_belongs_to_flow = true;
    if (position == context->flows.end()) {
        status = create_flow(
            context, key, info, *packet, now_us, &position);
        if (status != KTAG_SUCCESS) {
            return status;
        }
    } else {
        try {
            feature_context_flow *flow = position->second.get();
            packet_belongs_to_flow = flow->node.status == CONNECTED &&
                (key.protocol == IPPROTO_UDP ||
                 (info.tcp_flag & (TH_RST | TH_FIN)) == 0U);
            if (context->lifecycle_mode !=
                    KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT ||
                packet_belongs_to_flow) {
                flow->last_packet_sequence = packet->sequence;
                flow->last_seen_us = now_us;
            }
            if (key.protocol == IPPROTO_UDP ||
                (info.tcp_flag & (TH_RST | TH_FIN)) == 0U) {
                if (packet_belongs_to_flow) {
                    add_packet_to_flow_node(&flow->node, &info);
                }
            }
            status = process_flow_rawbow(context, flow, info);
            if (status != KTAG_SUCCESS) {
                context->flows.erase(position);
                ++context->stats.dropped_flows;
                return status;
            }
        } catch (const std::bad_alloc &) {
            context->flows.erase(position);
            ++context->stats.dropped_flows;
            return KTAG_ERROR_OUT_OF_MEMORY;
        } catch (...) {
            context->flows.erase(position);
            ++context->stats.dropped_flows;
            return KTAG_ERROR_INTERNAL;
        }
    }

    feature_context_flow *flow = position->second.get();
    bool should_finish = false;
    if (key.protocol == IPPROTO_TCP) {
        if ((info.tcp_flag & TH_RST) != 0U) {
            flow->node.status = CLOSED;
            should_finish = true;
        } else if ((info.tcp_flag & TH_FIN) != 0U) {
            if (flow->node.status == CONNECTED) {
                flow->node.status = CLOSING;
            } else if (flow->node.status == CLOSING) {
                flow->node.status = CLOSED;
            }
        } else if (flow->node.status == CLOSED &&
                   (info.tcp_flag & TH_ACK) != 0U) {
            should_finish = true;
        }
    } else if (flow->node.raw_data.packets_cnt +
                   flow->node.reverse_raw_data.packets_cnt >=
               context->udp_packet_limit) {
        flow->node.status = CLOSED;
        should_finish = context->lifecycle_mode !=
            KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
    }

    if (context->lifecycle_mode != KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT ||
        packet_belongs_to_flow) {
        status = ktag_feature_flow_runtime_on_packet_internal(
            &flow->runtime,
            packet->data,
            packet->captured_length,
            &packet->timestamp);
        if (status != KTAG_SUCCESS) {
            ++context->stats.callback_failures;
            ++context->stats.dropped_flows;
            context->flows.erase(position);
            return status;
        }
    }

    if (!should_finish) {
        return KTAG_SUCCESS;
    }
    status = finish_flow(
        context, flow, flow_finish_reason::protocol, output);
    context->flows.erase(position);
    return status;
}

ktag_status expire_internal(
    ktag_feature_context *context,
    uint64_t current_time_us,
    ktag_feature_batch *output)
{
    uint32_t deleted_domains = 0U;
    ktag_status status = ktag_domain_store_expire(
        context->domain_store, current_time_us, &deleted_domains);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    context->stats.domain_expired_entries += deleted_domains;
    status = ktag_domain_cache_sync(context->domain_cache);
    if (status != KTAG_SUCCESS) {
        return status;
    }

    if (context->lifecycle_mode ==
        KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT) {
        return KTAG_SUCCESS;
    }

    for (auto position = context->flows.begin();
         position != context->flows.end();) {
        if (!flow_is_expired(context, *position->second, current_time_us)) {
            ++position;
            continue;
        }
        auto expired = position++;
        feature_context_flow *flow = expired->second.get();
        const flow_finish_reason reason = flow->key.protocol == IPPROTO_TCP ?
            flow_finish_reason::tcp_expire : flow_finish_reason::udp_expire;
        status = finish_flow(context, flow, reason, output);
        context->flows.erase(expired);
        if (status != KTAG_SUCCESS) {
            return status;
        }
    }
    return KTAG_SUCCESS;
}

} // namespace

extern "C" {

KTAG_API ktag_feature_context *ktag_feature_create(
    const ktag_feature_config *config)
{
    if (!valid_feature_config(config)) {
        return nullptr;
    }
    try {
        std::unique_ptr<ktag_feature_context> context(
            new (std::nothrow) ktag_feature_context());
        if (!context) {
            return nullptr;
        }
        context->pipeline = ktag_feature_pipeline_create_internal(
            config->schema, config->registry);
        if (context->pipeline == nullptr) {
            return nullptr;
        }

        context->lifecycle_mode = config->struct_size >= sizeof(*config) ?
            config->lifecycle_mode : KTAG_FEATURE_LIFECYCLE_MODERN;
        context->flow_capacity = config->flow_capacity == 0U ?
            (context->lifecycle_mode ==
                    KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT ?
                std::numeric_limits<uint32_t>::max() :
                KTAG_FEATURE_DEFAULT_FLOW_CAPACITY) :
            config->flow_capacity;
        context->tcp_idle_timeout_us = config->tcp_idle_timeout_us == 0U ?
            KTAG_FEATURE_DEFAULT_TCP_IDLE_TIMEOUT_US :
            config->tcp_idle_timeout_us;
        context->udp_idle_timeout_us = config->udp_idle_timeout_us == 0U ?
            KTAG_FEATURE_DEFAULT_UDP_IDLE_TIMEOUT_US :
            config->udp_idle_timeout_us;
        context->udp_packet_limit = config->udp_packet_limit == 0U ?
            KTAG_FEATURE_DEFAULT_UDP_PACKET_LIMIT : config->udp_packet_limit;
        if (config->shared_domain_store != nullptr) {
            context->domain_store = config->shared_domain_store;
        } else {
            ktag_domain_store_config store_config{};
            store_config.struct_size = sizeof(store_config);
            store_config.api_version = KTAG_API_VERSION;
            store_config.max_entries = KTAG_DOMAIN_DEFAULT_MAX_ENTRIES;
            store_config.journal_capacity = KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY;
            context->domain_store = ktag_domain_store_create(&store_config);
            context->owns_domain_store = context->domain_store != nullptr;
        }
        if (context->domain_store == nullptr) {
            return nullptr;
        }
        ktag_domain_cache_config cache_config{};
        cache_config.struct_size = sizeof(cache_config);
        cache_config.api_version = KTAG_API_VERSION;
        cache_config.authority_lookup_on_miss =
            config->domain_authority_lookup_on_miss;
        context->domain_cache = ktag_domain_cache_create(
            context->domain_store, &cache_config);
        if (context->domain_cache == nullptr) {
            return nullptr;
        }

        context->flows.reserve(std::min(
            context->flow_capacity, FEATURE_FLOW_INITIAL_CAPACITY));
        context->output_values.resize(
            context->pipeline->schema->value_count, 0.0F);
        context->stats.struct_size = sizeof(context->stats);
        context->stats.api_version = KTAG_API_VERSION;
        return context.release();
    } catch (...) {
        return nullptr;
    }
}

KTAG_API ktag_status ktag_feature_process_packet(
    ktag_feature_context *context,
    const ktag_packet *packet,
    ktag_feature_batch *output)
{
    try {
        return process_packet_internal(context, packet, output);
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API ktag_status ktag_feature_expire(
    ktag_feature_context *context,
    uint64_t current_time_us,
    ktag_feature_batch *output)
{
    try {
        const ktag_status status = prepare_output(context, output);
        return status == KTAG_SUCCESS ?
            expire_internal(context, current_time_us, output) : status;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API ktag_status ktag_feature_flush(
    ktag_feature_context *context,
    ktag_feature_batch *output)
{
    try {
        ktag_status status = prepare_output(context, output);
        if (status != KTAG_SUCCESS) {
            return status;
        }
        for (auto position = context->flows.begin();
             position != context->flows.end();) {
            auto flushed = position++;
            status = finish_flow(
                context,
                flushed->second.get(),
                flow_finish_reason::flush,
                output);
            context->flows.erase(flushed);
            if (status != KTAG_SUCCESS) {
                return status;
            }
        }
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API ktag_status ktag_feature_get_statistics(
    const ktag_feature_context *context,
    ktag_feature_statistics *statistics)
{
    if (context == nullptr || statistics == nullptr ||
        statistics->struct_size < sizeof(*statistics) ||
        statistics->api_version != KTAG_API_VERSION) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    auto *mutable_context = const_cast<ktag_feature_context *>(context);
    if (!check_and_bind_owner(mutable_context)) {
        return KTAG_ERROR_INVALID_STATE;
    }
    *statistics = context->stats;
    statistics->thread_violations =
        context->thread_violations.load(std::memory_order_relaxed);
    statistics->active_flows = static_cast<uint32_t>(context->flows.size());
    return KTAG_SUCCESS;
}

KTAG_API void ktag_feature_destroy(ktag_feature_context *context)
{
    delete context;
}

} // extern "C"
