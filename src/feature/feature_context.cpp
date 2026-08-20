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
#include "feature_batch_internal.h"
#include "feature_pipeline_internal.h"
#include "flow_hash.h"
#include "src/packet_decode_internal.h"

namespace {

constexpr uint64_t FEATURE_FLOW_HASH_SEED = 0x4b544147464c4f57ULL;
constexpr uint64_t FEATURE_FLOW_HASH_GOLDEN_RATIO = 0x9e3779b97f4a7c15ULL;
constexpr uint32_t FEATURE_FLOW_INITIAL_CAPACITY = 10000U;

uint64_t load_flow_address_word(const uint8_t *address,
                                size_t length) noexcept {
  uint64_t value = 0U;
  std::memcpy(&value, address, length);
  return value;
}

void combine_flow_hash(uint64_t *hash, uint64_t value) noexcept {
  *hash ^=
      value + FEATURE_FLOW_HASH_GOLDEN_RATIO + (*hash << 6U) + (*hash >> 2U);
}

uint64_t finalize_flow_hash(uint64_t hash) noexcept {
  hash ^= hash >> 30U;
  hash *= 0xbf58476d1ce4e5b9ULL;
  hash ^= hash >> 27U;
  hash *= 0x94d049bb133111ebULL;
  return hash ^ (hash >> 31U);
}

enum class flow_finish_reason { protocol, tcp_expire, udp_expire, flush };

struct feature_flow_key_hash {
  size_t operator()(const ktag_flow_key &key) const noexcept {
    const size_t address_length = key.address_a.version == 4U
                                      ? sizeof(uint32_t)
                                      : sizeof(key.address_a.bytes);
    const size_t first_word_length = std::min(address_length, sizeof(uint64_t));
    uint64_t hash = FEATURE_FLOW_HASH_SEED;
    combine_flow_hash(
        &hash, load_flow_address_word(key.address_a.bytes, first_word_length));
    if (address_length > sizeof(uint64_t)) {
      combine_flow_hash(
          &hash, load_flow_address_word(key.address_a.bytes + sizeof(uint64_t),
                                        sizeof(uint64_t)));
    }
    combine_flow_hash(
        &hash, load_flow_address_word(key.address_b.bytes, first_word_length));
    if (address_length > sizeof(uint64_t)) {
      combine_flow_hash(
          &hash, load_flow_address_word(key.address_b.bytes + sizeof(uint64_t),
                                        sizeof(uint64_t)));
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
  bool operator()(const ktag_flow_key &first,
                  const ktag_flow_key &second) const noexcept {
    if (first.address_a.version != second.address_a.version ||
        first.address_b.version != second.address_b.version ||
        first.port_a != second.port_a || first.port_b != second.port_b ||
        first.protocol != second.protocol) {
      return false;
    }
    const size_t address_length = first.address_a.version == 4U
                                      ? sizeof(uint32_t)
                                      : sizeof(first.address_a.bytes);
    return std::memcmp(first.address_a.bytes, second.address_a.bytes,
                       address_length) == 0 &&
           std::memcmp(first.address_b.bytes, second.address_b.bytes,
                       address_length) == 0;
  }
};

struct feature_context_flow {
  ktag_flow_key key{};
  ktag_ip_address node_source_address{};
  ktag_ip_address node_destination_address{};
  ktag_ip_address forward_address{};
  ktag_ip_address reverse_address{};
  flow_hash_node node{};
  ktag_feature_flow_runtime runtime{};
  uint16_t forward_port = 0U;
  uint16_t reverse_port = 0U;
  uint64_t flow_id = 0U;
  uint64_t first_packet_sequence = 0U;
  uint64_t last_packet_sequence = 0U;
  uint64_t last_seen_us = 0U;
  uint64_t domain_lookup_version = 0U;
  uint32_t ipv6_extension_packet_count = 0U;
  uint32_t ip_fragment_first_packet_count = 0U;
  bool domain_lookup_initialized = false;
  bool forward_is_node_source = true;

  ~feature_context_flow() {
    ktag_feature_flow_runtime_destroy_internal(&runtime);
  }
};

using feature_flow_table =
    std::unordered_map<ktag_flow_key, std::unique_ptr<feature_context_flow>,
                       feature_flow_key_hash, feature_flow_key_equal>;

size_t current_thread_token() {
  static std::atomic<size_t> next_token{1U};
  thread_local const size_t token =
      next_token.fetch_add(1U, std::memory_order_relaxed);
  return token == 0U ? std::numeric_limits<size_t>::max() : token;
}

ktag_status translate_decode_status(KDADKResult status) {
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

KDADKResult translate_domain_status(ktag_status status) {
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

bool valid_feature_config(const ktag_feature_config *config) {
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

uint64_t packet_timestamp_us(const ktag_packet &packet) {
  const uint64_t seconds = static_cast<uint64_t>(packet.timestamp.seconds);
  const uint64_t microseconds =
      static_cast<uint64_t>(packet.timestamp.microseconds);
  if (seconds >
      (std::numeric_limits<uint64_t>::max() - microseconds) / 1000000U) {
    return std::numeric_limits<uint64_t>::max();
  }
  return seconds * 1000000U + microseconds;
}

void initialize_legacy_node_tuple(flow_hash_node *node, const packet_info &info,
                                  const ktag_decoded_transport_packet &decoded,
                                  ktag_ip_address *node_source_address,
                                  ktag_ip_address *node_destination_address,
                                  bool *forward_is_node_source) {
  if (decoded.network_version == 4U && info.tuple.src_ip <= info.tuple.dst_ip) {
    node->tuple.src_ip = info.tuple.src_ip;
    node->tuple.src_port = info.tuple.src_port;
    node->tuple.dst_ip = info.tuple.dst_ip;
    node->tuple.dst_port = info.tuple.dst_port;
  } else if (decoded.network_version == 4U) {
    node->tuple.src_ip = info.tuple.dst_ip;
    node->tuple.src_port = info.tuple.dst_port;
    node->tuple.dst_ip = info.tuple.src_ip;
    node->tuple.dst_port = info.tuple.src_port;
  } else {
    node->tuple.src_ip = 0U;
    node->tuple.src_port = htons(decoded.flow_key.port_a);
    node->tuple.dst_ip = 0U;
    node->tuple.dst_port = htons(decoded.flow_key.port_b);
  }
  node->tuple.protocol = info.tuple.protocol;
  node->flow_src_ip = decoded.network_version == 4U ? info.tuple.src_ip : 0U;

  if (decoded.network_version == 4U) {
    std::memset(node_source_address, 0, sizeof(*node_source_address));
    std::memset(node_destination_address, 0, sizeof(*node_destination_address));
    std::memcpy(node_source_address->bytes, &node->tuple.src_ip,
                sizeof(node->tuple.src_ip));
    std::memcpy(node_destination_address->bytes, &node->tuple.dst_ip,
                sizeof(node->tuple.dst_ip));
    node_source_address->version = 4U;
    node_destination_address->version = 4U;
    *forward_is_node_source = info.tuple.src_ip == node->tuple.src_ip &&
                              info.tuple.src_port == node->tuple.src_port;
  } else {
    *node_source_address = decoded.flow_key.address_a;
    *node_destination_address = decoded.flow_key.address_b;
    *forward_is_node_source = decoded.source_port == decoded.flow_key.port_a &&
                              std::memcmp(decoded.source_address.bytes,
                                          decoded.flow_key.address_a.bytes,
                                          KTAG_IPV6_ADDRESS_SIZE) == 0;
  }
}

void append_packet_sample(flow_raw_data *data, const packet_info &info) {
  data->header_len.push_back(info.header_len);
  data->http_uris.push_back(info.http_uri);
  data->http_hosts.push_back(info.http_host);
  data->http_urls.push_back(info.http_url);
  data->payload_len.push_back(info.payload_len);
  data->ts.push_back(info.ts);
  if (info.tls_info.client_version != 0U) {
    data->tls_infos.push_back(info.tls_info);
  }
  data->total_size += info.packet_size;
  ++data->packets_cnt;
}

void add_packet_to_flow_direction(flow_hash_node *node, const packet_info &info,
                                  ktag_direction direction) {
  append_packet_sample(direction == KTAG_DIRECTION_FORWARD
                           ? &node->raw_data
                           : &node->reverse_raw_data,
                       info);
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
  ktag_feature_lifecycle_mode lifecycle_mode = KTAG_FEATURE_LIFECYCLE_MODERN;
  uint64_t next_flow_id = 1U;
  uint64_t next_legacy_emission_id = 1U;
  ktag_feature_statistics stats{};
  std::atomic<size_t> owner_thread{0U};
  std::atomic<uint64_t> thread_violations{0U};

  ~ktag_feature_context() {
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

bool check_and_bind_owner(ktag_feature_context *context) {
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
          expected, token, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return true;
  }
  if (expected == token) {
    return true;
  }
  context->thread_violations.fetch_add(1U, std::memory_order_relaxed);
  return false;
}

ktag_status prepare_output(ktag_feature_context *context,
                           ktag_feature_batch *output) {
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

KDADKResult context_domain_update(void *user_data,
                                  const ktag_ip_address *address,
                                  const std::string &domain,
                                  uint64_t expire_at_us) {
  auto *context = static_cast<ktag_feature_context *>(user_data);
  if (context == nullptr || context->domain_store == nullptr ||
      context->domain_cache == nullptr || address == nullptr) {
    return KDADK_STS_NULL_PTR_ERR;
  }
  ktag_status status = ktag_domain_store_put(
      context->domain_store, address, domain.c_str(), expire_at_us, nullptr);
  if (status == KTAG_SUCCESS) {
    status = ktag_domain_cache_sync(context->domain_cache);
  }
  return translate_domain_status(status);
}

ktag_status lookup_domain(ktag_feature_context *context,
                          const ktag_ip_address &address, uint64_t now_us,
                          std::string *domain, bool *found) {
  *found = false;
  char value[KTAG_DOMAIN_NAME_MAX_LENGTH + 1U] = {};
  uint32_t required_capacity = 0U;
  const ktag_status status =
      ktag_domain_cache_lookup(context->domain_cache, &address, now_us, value,
                               sizeof(value), &required_capacity);
  if (status == KTAG_ERROR_NOT_FOUND) {
    return KTAG_SUCCESS; /* 解析失败跳过 */
  }
  if (status != KTAG_SUCCESS) {
    return status;
  }
  *domain = value;
  *found = true;
  return KTAG_SUCCESS;
}

ktag_status refresh_flow_domains(ktag_feature_context *context,
                                 feature_context_flow *flow,
                                 bool add_new_tokens) {
  const bool already_had_domain = !flow->node.src_ip_domain_name.empty() ||
                                  !flow->node.dst_ip_domain_name.empty();
  flow->node.src_ip_domain_name.clear();
  flow->node.dst_ip_domain_name.clear();

  bool found_source = false;
  bool found_destination = false;
  ktag_status status =
      lookup_domain(context, flow->node_source_address, flow->last_seen_us,
                    &flow->node.src_ip_domain_name, &found_source);
  if (status != KTAG_SUCCESS) {
    return status;
  }
  status =
      lookup_domain(context, flow->node_destination_address, flow->last_seen_us,
                    &flow->node.dst_ip_domain_name, &found_destination);
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

ktag_status process_flow_rawbow(ktag_feature_context *context,
                                feature_context_flow *flow,
                                const packet_info &info) {
  if (flow->node.rawbow_cnt >= MAX_RAWBOW_CNT) {
    return KTAG_SUCCESS;
  }
  if (flow->node.src_ip_domain_name.empty() &&
      flow->node.dst_ip_domain_name.empty()) {
    const uint64_t published_version =
        ktag_domain_store_get_version(context->domain_store);
    if (!flow->domain_lookup_initialized ||
        flow->domain_lookup_version != published_version) {
      const ktag_status status = refresh_flow_domains(context, flow, true);
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

void record_finish_reason(ktag_feature_context *context,
                          flow_finish_reason reason) {
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

ktag_status finish_flow(ktag_feature_context *context,
                        feature_context_flow *flow, flow_finish_reason reason,
                        ktag_feature_batch *output) {
  try {
    const bool legacy_identity =
        context->lifecycle_mode == KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
    if (legacy_identity) {
      if (context->next_legacy_emission_id ==
          std::numeric_limits<uint64_t>::max()) {
        ++context->stats.dropped_flows;
        return KTAG_ERROR_CAPACITY;
      }
      /* The fixed-vector extractor assigned flow_id when a row was emitted,
       * not when the flow was first observed. Preserve that CSV contract while
       * modern lifecycle mode keeps stable creation-time identities. */
      flow->flow_id = context->next_legacy_emission_id;
      flow->runtime.flow.flow_id = flow->flow_id;
    }
    record_finish_reason(context, reason);
    ktag_status status = refresh_flow_domains(context, flow, true);
    if (status != KTAG_SUCCESS) {
      ++context->stats.dropped_flows;
      return status;
    }

    status = ktag_feature_flow_runtime_finish_internal(
        &flow->runtime, context->output_values.data(),
        static_cast<uint32_t>(context->output_values.size()));
    if (status != KTAG_SUCCESS) {
      ++context->stats.callback_failures;
      ++context->stats.dropped_flows;
      return status;
    }

    ktag_feature_record_details details{};
    details.struct_size = sizeof(details);
    details.api_version = KTAG_API_VERSION;
    details.metadata.struct_size = sizeof(details.metadata);
    details.metadata.api_version = KTAG_API_VERSION;
    details.metadata.flow_id = flow->flow_id;
    details.metadata.flow_key = flow->key;
    details.metadata.first_packet_sequence = flow->first_packet_sequence;
    details.metadata.last_packet_sequence = flow->last_packet_sequence;
    details.source_address = flow->forward_address;
    details.destination_address = flow->reverse_address;
    details.source_port = flow->forward_port;
    details.destination_port = flow->reverse_port;
    details.ipv6_extension_packet_count = flow->ipv6_extension_packet_count;
    details.ip_fragment_first_packet_count =
        flow->ip_fragment_first_packet_count;
    status = ktag_feature_batch_append_with_details_internal(
        output, &details, context->output_values.data(),
        static_cast<uint32_t>(context->output_values.size()));
    if (status == KTAG_SUCCESS) {
      ++context->stats.emitted_flows;
      if (legacy_identity) {
        ++context->next_legacy_emission_id;
      }
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

bool flow_is_expired(const ktag_feature_context *context,
                     const feature_context_flow &flow,
                     uint64_t current_time_us) {
  if (current_time_us < flow.last_seen_us) {
    return false;
  }
  const uint64_t timeout = flow.key.protocol == IPPROTO_TCP
                               ? context->tcp_idle_timeout_us
                               : context->udp_idle_timeout_us;
  return timeout != 0U && current_time_us - flow.last_seen_us >= timeout;
}

bool same_ip_address(const ktag_ip_address &first,
                     const ktag_ip_address &second) {
  if (first.version != second.version ||
      (first.version != 4U && first.version != 6U)) {
    return false;
  }
  const uint32_t length = first.version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
  return std::memcmp(first.bytes, second.bytes, length) == 0;
}

ktag_status get_packet_direction(const feature_context_flow &flow,
                                 const ktag_decoded_transport_packet &decoded,
                                 ktag_direction *direction) {
  if (direction == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (same_ip_address(decoded.source_address, flow.forward_address) &&
      decoded.source_port == flow.forward_port) {
    *direction = KTAG_DIRECTION_FORWARD;
    return KTAG_SUCCESS;
  }
  if (same_ip_address(decoded.destination_address, flow.forward_address) &&
      decoded.destination_port == flow.forward_port) {
    *direction = KTAG_DIRECTION_REVERSE;
    return KTAG_SUCCESS;
  }
  return KTAG_ERROR_INVALID_ARGUMENT;
}

ktag_packet_transport_view
make_transport_view(const ktag_decoded_transport_packet &decoded,
                    ktag_direction direction) {
  ktag_packet_transport_view view{};
  view.struct_size = sizeof(view);
  view.api_version = KTAG_API_VERSION;
  view.direction = direction;
  view.network_version = decoded.network_version;
  view.transport_protocol = decoded.transport_protocol;
  view.tcp_flags = decoded.tcp_flags;
  view.fragment_state = static_cast<uint8_t>(decoded.fragment_state);
  view.network_header_length = decoded.network_header_length;
  view.transport_header_length = decoded.transport_header_length;
  view.ipv6_extension_header_count = decoded.ipv6_extension_header_count;
  view.ipv6_extension_header_bytes = decoded.ipv6_extension_header_bytes;
  view.tcp_sequence = decoded.tcp_sequence;
  view.tcp_acknowledgment = decoded.tcp_acknowledgment;
  return view;
}

ktag_status build_packet_info(ktag_feature_context *context,
                              const ktag_packet &packet,
                              const ktag_decoded_transport_packet &decoded,
                              packet_info *info) {
  if (context == nullptr || info == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *info = {};
  /*
   * Preserve the Legacy Provider's captured-byte statistics for IPv4.
   * The shared decoder separately supplies the bounded L4 Payload View.
   */
  info->packet_size = packet.captured_length;
  info->header_len = decoded.link_header_length + decoded.network_header_length;
  info->tuple.protocol = decoded.transport_protocol;
  if (decoded.network_version == 4U) {
    std::memcpy(&info->tuple.src_ip, decoded.source_address.bytes,
                sizeof(info->tuple.src_ip));
    std::memcpy(&info->tuple.dst_ip, decoded.destination_address.bytes,
                sizeof(info->tuple.dst_ip));
  }
  if (static_cast<uint64_t>(packet.timestamp.seconds) >
      static_cast<uint64_t>(std::numeric_limits<time_t>::max())) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  info->ts.tv_sec = static_cast<time_t>(packet.timestamp.seconds);
  info->ts.tv_usec = packet.timestamp.microseconds;
  const KDADKResult decode_status =
      decode_transport_payload_with_domain_callback(
          decoded.transport, decoded.transport_packet_length,
          decoded.transport_protocol, info, context_domain_update, context);
  return translate_decode_status(decode_status);
}

ktag_status create_flow(ktag_feature_context *context,
                        const ktag_decoded_transport_packet &decoded,
                        const packet_info &info, const ktag_packet &packet,
                        uint64_t now_us,
                        feature_flow_table::iterator *created) {
  if (context->flows.size() >= context->flow_capacity) {
    ++context->stats.capacity_rejections;
    return KTAG_ERROR_CAPACITY;
  }
  if (context->next_flow_id == std::numeric_limits<uint64_t>::max()) {
    return KTAG_ERROR_CAPACITY;
  }

  std::unique_ptr<feature_context_flow> flow(new (std::nothrow)
                                                 feature_context_flow());
  if (!flow) {
    return KTAG_ERROR_OUT_OF_MEMORY;
  }
  flow->key = decoded.flow_key;
  flow->forward_address = decoded.source_address;
  flow->reverse_address = decoded.destination_address;
  flow->forward_port = decoded.source_port;
  flow->reverse_port = decoded.destination_port;
  flow->flow_id = context->next_flow_id;
  flow->first_packet_sequence = packet.sequence;
  flow->last_packet_sequence = packet.sequence;
  flow->last_seen_us = now_us;
  initialize_legacy_node_tuple(
      &flow->node, info, decoded, &flow->node_source_address,
      &flow->node_destination_address, &flow->forward_is_node_source);
  add_packet_to_flow_direction(&flow->node, info, KTAG_DIRECTION_FORWARD);

  ktag_status status = process_flow_rawbow(context, flow.get(), info);
  if (status != KTAG_SUCCESS) {
    return status;
  }
  status = ktag_feature_flow_runtime_create_with_key_internal(
      context->pipeline, &flow->node, flow->flow_id, &flow->key,
      &flow->forward_address, flow->forward_port, flow->forward_is_node_source,
      &flow->runtime);
  if (status != KTAG_SUCCESS) {
    ++context->stats.callback_failures;
    ++context->stats.dropped_flows;
    return status;
  }

  const auto inserted =
      context->flows.emplace(decoded.flow_key, std::move(flow));
  if (!inserted.second) {
    return KTAG_ERROR_ALREADY_EXISTS;
  }
  ++context->next_flow_id;
  ++context->stats.created_flows;
  context->stats.peak_active_flows =
      std::max(context->stats.peak_active_flows,
               static_cast<uint32_t>(context->flows.size()));
  *created = inserted.first;
  return KTAG_SUCCESS;
}

ktag_status process_packet_internal(ktag_feature_context *context,
                                    const ktag_packet *packet,
                                    ktag_feature_batch *output) {
  ktag_status status = prepare_output(context, output);
  if (status != KTAG_SUCCESS) {
    return status;
  }
  if (packet == nullptr || packet->struct_size < sizeof(*packet) ||
      packet->api_version != KTAG_API_VERSION || packet->data == nullptr ||
      packet->captured_length == 0U || packet->timestamp.seconds < 0 ||
      packet->timestamp.microseconds < 0 ||
      packet->timestamp.microseconds > 999999) {
    ++context->stats.rejected_packets;
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  ++context->stats.received_packets;
  context->stats.received_bytes += packet->captured_length;
  ktag_decoded_transport_packet decoded{};
  status = ktag_decode_transport_packet_internal(packet, &decoded);
  if (status != KTAG_SUCCESS) {
    ++context->stats.rejected_packets;
    if (status == KTAG_ERROR_NOT_FOUND || status == KTAG_ERROR_NOT_SUPPORTED ||
        status == KTAG_ERROR_INVALID_ARGUMENT ||
        status == KTAG_ERROR_CAPACITY) {
      return KTAG_SUCCESS;
    }
    return status;
  }
  if (!decoded.transport_header_complete) {
    ++context->stats.rejected_packets;
    return KTAG_SUCCESS;
  }

  packet_info info{};
  status = build_packet_info(context, *packet, decoded, &info);
  if (status != KTAG_SUCCESS) {
    ++context->stats.rejected_packets;
    return status;
  }
  ++context->stats.decoded_packets;

  const uint64_t now_us = packet_timestamp_us(*packet);
  auto position = context->flows.find(decoded.flow_key);
  const bool tcp_control_without_flow =
      decoded.flow_key.protocol == IPPROTO_TCP &&
      (info.tcp_flag & (TH_RST | TH_FIN)) != 0U &&
      position == context->flows.end();
  if (tcp_control_without_flow) {
    return KTAG_SUCCESS;
  }

  bool packet_belongs_to_flow = true;
  ktag_direction direction = KTAG_DIRECTION_FORWARD;
  if (position == context->flows.end()) {
    status = create_flow(context, decoded, info, *packet, now_us, &position);
    if (status != KTAG_SUCCESS) {
      return status;
    }
  } else {
    try {
      feature_context_flow *flow = position->second.get();
      status = get_packet_direction(*flow, decoded, &direction);
      if (status != KTAG_SUCCESS) {
        ++context->stats.rejected_packets;
        return KTAG_SUCCESS;
      }
      packet_belongs_to_flow = flow->node.status == CONNECTED &&
                               (decoded.flow_key.protocol == IPPROTO_UDP ||
                                (info.tcp_flag & (TH_RST | TH_FIN)) == 0U);
      if (context->lifecycle_mode != KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT ||
          packet_belongs_to_flow) {
        flow->last_packet_sequence = packet->sequence;
        flow->last_seen_us = now_us;
      }
      if (decoded.flow_key.protocol == IPPROTO_UDP ||
          (info.tcp_flag & (TH_RST | TH_FIN)) == 0U) {
        if (packet_belongs_to_flow) {
          add_packet_to_flow_direction(&flow->node, info, direction);
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
  if (decoded.network_version == 6U &&
      decoded.ipv6_extension_header_count != 0U &&
      flow->ipv6_extension_packet_count !=
          std::numeric_limits<uint32_t>::max()) {
    ++flow->ipv6_extension_packet_count;
  }
  if (decoded.fragment_state == ktag_internal_fragment_state::first &&
      flow->ip_fragment_first_packet_count !=
          std::numeric_limits<uint32_t>::max()) {
    ++flow->ip_fragment_first_packet_count;
  }
  bool should_finish = false;
  if (decoded.flow_key.protocol == IPPROTO_TCP) {
    if ((info.tcp_flag & TH_RST) != 0U) {
      flow->node.status = CLOSED;
      should_finish = true;
    } else if ((info.tcp_flag & TH_FIN) != 0U) {
      if (flow->node.status == CONNECTED) {
        flow->node.status = CLOSING;
      } else if (flow->node.status == CLOSING) {
        flow->node.status = CLOSED;
      }
    } else if (flow->node.status == CLOSED && (info.tcp_flag & TH_ACK) != 0U) {
      should_finish = true;
    }
  } else if (flow->node.raw_data.packets_cnt +
                 flow->node.reverse_raw_data.packets_cnt >=
             context->udp_packet_limit) {
    flow->node.status = CLOSED;
    should_finish =
        context->lifecycle_mode != KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
  }

  if (context->lifecycle_mode != KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT ||
      packet_belongs_to_flow) {
    const ktag_packet_transport_view transport =
        make_transport_view(decoded, direction);
    status = ktag_feature_flow_runtime_on_packet_view_internal(
        &flow->runtime, packet->data, packet->captured_length,
        &packet->timestamp, decoded.payload, decoded.payload_length,
        &transport);
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
  status = finish_flow(context, flow, flow_finish_reason::protocol, output);
  context->flows.erase(position);
  return status;
}

ktag_status expire_internal(ktag_feature_context *context,
                            uint64_t current_time_us,
                            ktag_feature_batch *output) {
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

  if (context->lifecycle_mode == KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT) {
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
    const flow_finish_reason reason = flow->key.protocol == IPPROTO_TCP
                                          ? flow_finish_reason::tcp_expire
                                          : flow_finish_reason::udp_expire;
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

KTAG_API ktag_feature_context *
ktag_feature_create(const ktag_feature_config *config) {
  if (!valid_feature_config(config)) {
    return nullptr;
  }
  try {
    std::unique_ptr<ktag_feature_context> context(new (std::nothrow)
                                                      ktag_feature_context());
    if (!context) {
      return nullptr;
    }
    context->pipeline =
        ktag_feature_pipeline_create_internal(config->schema, config->registry);
    if (context->pipeline == nullptr) {
      return nullptr;
    }

    context->lifecycle_mode = config->struct_size >= sizeof(*config)
                                  ? config->lifecycle_mode
                                  : KTAG_FEATURE_LIFECYCLE_MODERN;
    context->flow_capacity =
        config->flow_capacity == 0U
            ? (context->lifecycle_mode == KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT
                   ? std::numeric_limits<uint32_t>::max()
                   : KTAG_FEATURE_DEFAULT_FLOW_CAPACITY)
            : config->flow_capacity;
    context->tcp_idle_timeout_us =
        config->tcp_idle_timeout_us == 0U
            ? KTAG_FEATURE_DEFAULT_TCP_IDLE_TIMEOUT_US
            : config->tcp_idle_timeout_us;
    context->udp_idle_timeout_us =
        config->udp_idle_timeout_us == 0U
            ? KTAG_FEATURE_DEFAULT_UDP_IDLE_TIMEOUT_US
            : config->udp_idle_timeout_us;
    context->udp_packet_limit = config->udp_packet_limit == 0U
                                    ? KTAG_FEATURE_DEFAULT_UDP_PACKET_LIMIT
                                    : config->udp_packet_limit;
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
    context->domain_cache =
        ktag_domain_cache_create(context->domain_store, &cache_config);
    if (context->domain_cache == nullptr) {
      return nullptr;
    }

    context->flows.reserve(
        std::min(context->flow_capacity, FEATURE_FLOW_INITIAL_CAPACITY));
    context->output_values.resize(context->pipeline->schema->value_count, 0.0F);
    context->stats.struct_size = sizeof(context->stats);
    context->stats.api_version = KTAG_API_VERSION;
    return context.release();
  } catch (...) {
    return nullptr;
  }
}

KTAG_API ktag_status ktag_feature_process_packet(ktag_feature_context *context,
                                                 const ktag_packet *packet,
                                                 ktag_feature_batch *output) {
  try {
    return process_packet_internal(context, packet, output);
  } catch (const std::bad_alloc &) {
    return KTAG_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return KTAG_ERROR_INTERNAL;
  }
}

KTAG_API ktag_status ktag_feature_expire(ktag_feature_context *context,
                                         uint64_t current_time_us,
                                         ktag_feature_batch *output) {
  try {
    const ktag_status status = prepare_output(context, output);
    return status == KTAG_SUCCESS
               ? expire_internal(context, current_time_us, output)
               : status;
  } catch (const std::bad_alloc &) {
    return KTAG_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return KTAG_ERROR_INTERNAL;
  }
}

KTAG_API ktag_status ktag_feature_flush(ktag_feature_context *context,
                                        ktag_feature_batch *output) {
  try {
    ktag_status status = prepare_output(context, output);
    if (status != KTAG_SUCCESS) {
      return status;
    }
    for (auto position = context->flows.begin();
         position != context->flows.end();) {
      auto flushed = position++;
      status = finish_flow(context, flushed->second.get(),
                           flow_finish_reason::flush, output);
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
    const ktag_feature_context *context, ktag_feature_statistics *statistics) {
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

KTAG_API void ktag_feature_destroy(ktag_feature_context *context) {
  delete context;
}

} // extern "C"
