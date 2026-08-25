/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_H
#define KTAG_FEATURE_H

#include <stdint.h>

#include "ktag_capture.h"
#include "ktag_export.h"
#include "ktag_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KTAG_FEATURE_ID_MAX_LENGTH 255U
#define KTAG_FEATURE_NAME_MAX_LENGTH 255U
#define KTAG_PROVIDER_ID_MAX_LENGTH 255U
#define KTAG_DOMAIN_NAME_MAX_LENGTH 255U
#define KTAG_DOMAIN_DEFAULT_MAX_ENTRIES 10000U
#define KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY 4096U
#define KTAG_FEATURE_DEFAULT_FLOW_CAPACITY 65536U
#define KTAG_FEATURE_DEFAULT_TCP_IDLE_TIMEOUT_US 300000000ULL
#define KTAG_FEATURE_DEFAULT_UDP_IDLE_TIMEOUT_US 30000000ULL
#define KTAG_FEATURE_DEFAULT_UDP_PACKET_LIMIT 100U
#define KTAG_FEATURE_ANNOTATION_KEY_MAX_LENGTH 255U
#define KTAG_FEATURE_ANNOTATION_VALUE_MAX_LENGTH 4095U

/* Number of values written by the legacy CSV/model contract. */
#define KTAG_LEGACY_V1_VALUE_COUNT 354U

typedef struct ktag_feature_schema ktag_feature_schema;
typedef struct ktag_feature_batch ktag_feature_batch;
typedef struct ktag_feature_registry ktag_feature_registry;
typedef struct ktag_feature_output ktag_feature_output;
typedef struct ktag_flow_view ktag_flow_view;
typedef struct ktag_packet_view ktag_packet_view;
typedef struct ktag_feature_context ktag_feature_context;
typedef struct ktag_domain_store ktag_domain_store;
typedef struct ktag_domain_cache ktag_domain_cache;

typedef enum {
  KTAG_DOMAIN_PUT = 1,
  KTAG_DOMAIN_DELETE = 2
} ktag_domain_operation;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t max_entries;
  uint32_t journal_capacity;
} ktag_domain_store_config;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t authority_lookup_on_miss;
} ktag_domain_cache_config;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t version;
  uint32_t entry_count;
  uint32_t journal_count;
  uint64_t journal_oldest_version;
  uint32_t registered_cache_count;
} ktag_domain_store_stats;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t applied_version;
  uint64_t local_hits;
  uint64_t local_misses;
  uint64_t sync_count;
  uint64_t incremental_changes;
  uint64_t snapshot_recoveries;
  uint64_t authority_lookups;
  uint64_t authority_hits;
} ktag_domain_cache_stats;

typedef enum { KTAG_FEATURE_FLOAT32 = 1 } ktag_feature_value_type;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  const char *feature_id;
  const char *display_name;
  const char *provider_id;
  uint32_t provider_version;
  ktag_feature_value_type value_type;
  uint32_t dimension;
} ktag_feature_descriptor;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t flow_id;
  ktag_flow_key flow_key;
  uint64_t first_packet_sequence;
  uint64_t last_packet_sequence;
} ktag_feature_record_metadata;

/*
 * Optional extended record metadata for audit/CSV output. These fields are
 * not part of the Feature Schema and therefore never enter model input.
 */
typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  ktag_feature_record_metadata metadata;
  ktag_ip_address source_address;
  ktag_ip_address destination_address;
  uint16_t source_port;
  uint16_t destination_port;
  uint32_t ipv6_extension_packet_count;
  uint32_t ip_fragment_first_packet_count;
} ktag_feature_record_details;

/*
 * Borrowed record view. values remains valid until the batch is mutated,
 * cleared or destroyed.
 */
typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t schema_hash;
  ktag_feature_record_metadata metadata;
  const float *values;
  uint32_t value_count;
} ktag_feature_record;

typedef enum {
  KTAG_DIRECTION_FORWARD = 0,
  KTAG_DIRECTION_REVERSE = 1
} ktag_direction;

typedef enum {
  KTAG_IP_FRAGMENT_NONE = 0,
  KTAG_IP_FRAGMENT_ATOMIC = 1,
  KTAG_IP_FRAGMENT_FIRST = 2,
  KTAG_IP_FRAGMENT_NON_INITIAL = 3
} ktag_ip_fragment_state;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  ktag_direction direction;
  uint8_t network_version;
  uint8_t transport_protocol;
  uint8_t tcp_flags;
  uint8_t fragment_state;
  uint32_t network_header_length;
  uint32_t transport_header_length;
  uint32_t ipv6_extension_header_count;
  uint32_t ipv6_extension_header_bytes;
  uint32_t tcp_sequence;
  uint32_t tcp_acknowledgment;
} ktag_packet_transport_view;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  const char *provider_id;
  uint32_t provider_version;
  const ktag_feature_schema *schema;
  void *user_data;
} ktag_feature_provider_config;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;

  ktag_status (*provider_create)(const ktag_feature_provider_config *config,
                                 void **provider_state);

  ktag_status (*flow_create)(void *provider_state, const ktag_flow_view *flow,
                             void **flow_state);

  ktag_status (*on_packet)(void *provider_state, void *flow_state,
                           const ktag_packet_view *packet,
                           const ktag_flow_view *flow);

  ktag_status (*flow_finish)(void *provider_state, void *flow_state,
                             const ktag_flow_view *flow,
                             ktag_feature_output *output);

  void (*flow_destroy)(void *provider_state, void *flow_state);

  void (*provider_destroy)(void *provider_state);
} ktag_feature_callbacks;

/*
 * Policy applied when a Provider callback fails for one flow. Provider and
 * flow construction failures use the same policy where recovery is possible.
 */
typedef enum {
  KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW = 0,
  KTAG_FEATURE_CALLBACK_ERROR_FILL_ZERO = 1
} ktag_feature_callback_error_policy;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  const char *provider_id;
  uint32_t provider_version;
  const ktag_feature_descriptor *features;
  uint32_t feature_count;
  ktag_feature_callbacks callbacks;
  ktag_feature_callback_error_policy callback_error_policy;
  void *user_data;
  /* Optional model-contract identity; NULL uses provider_id. */
  const char *contract_provider_id;
} ktag_feature_provider_registration;

/*
 * MODERN ends UDP flows at udp_packet_limit and lets expire() apply idle
 * timeouts. LEGACY_COMPAT preserves the historical application-model flow
 * lifecycle:
 * a UDP flow keeps only its first udp_packet_limit packets and is emitted once
 * by flush(), while expire() performs DomainStore maintenance without ending
 * flows. TCP protocol close handling is identical in both modes.
 */
typedef enum {
  KTAG_FEATURE_LIFECYCLE_MODERN = 0,
  KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT = 1
} ktag_feature_lifecycle_mode;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  const ktag_feature_schema *schema;
  const ktag_feature_registry *registry;
  ktag_domain_store *shared_domain_store;
  uint32_t flow_capacity;
  uint64_t tcp_idle_timeout_us;
  uint64_t udp_idle_timeout_us;
  uint32_t udp_packet_limit;
  uint32_t domain_authority_lookup_on_miss;
  ktag_feature_lifecycle_mode lifecycle_mode;
  /* Auxiliary Provider text for diagnostics; never enters model input. */
  uint32_t collect_annotations;
} ktag_feature_config;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t received_packets;
  uint64_t received_bytes;
  uint64_t decoded_packets;
  uint64_t rejected_packets;
  uint64_t created_flows;
  uint64_t completed_flows;
  uint64_t emitted_flows;
  uint64_t expired_tcp_flows;
  uint64_t expired_udp_flows;
  uint64_t flushed_flows;
  uint64_t dropped_flows;
  uint64_t capacity_rejections;
  uint64_t callback_failures;
  uint64_t domain_expired_entries;
  uint64_t thread_violations;
  uint32_t active_flows;
  uint32_t peak_active_flows;
} ktag_feature_statistics;

enum {
  KTAG_FEATURE_REGISTER_DEFAULT = 0,
  KTAG_FEATURE_REGISTER_REPLACE = 1U << 0
};

/*
 * Shared authoritative DomainStore. Store mutation and statistics APIs are
 * thread-safe. A Store reference may be released before its Caches; the
 * object remains alive until the last registered Cache is destroyed.
 */
KTAG_API ktag_domain_store *
ktag_domain_store_create(const ktag_domain_store_config *config);

KTAG_API void ktag_domain_store_destroy(ktag_domain_store *store);

/* expire_at_us == 0 means the mapping does not expire automatically. */
KTAG_API ktag_status ktag_domain_store_put(ktag_domain_store *store,
                                           const ktag_ip_address *address,
                                           const char *domain,
                                           uint64_t expire_at_us,
                                           uint64_t *published_version);

KTAG_API ktag_status ktag_domain_store_delete(ktag_domain_store *store,
                                              const ktag_ip_address *address,
                                              uint64_t *published_version);

/* Publishes DELETE changes for entries whose TTL is at or before now_us. */
KTAG_API ktag_status ktag_domain_store_expire(ktag_domain_store *store,
                                              uint64_t now_us,
                                              uint32_t *deleted_count);

KTAG_API uint64_t ktag_domain_store_get_version(const ktag_domain_store *store);

KTAG_API ktag_status ktag_domain_store_get_stats(
    const ktag_domain_store *store, ktag_domain_store_stats *stats);

/*
 * Per-Feature-Context local cache. A Cache is single-thread owned; its lookup
 * hot path only performs an acquire version load and accesses its local map.
 */
KTAG_API ktag_domain_cache *
ktag_domain_cache_create(ktag_domain_store *store,
                         const ktag_domain_cache_config *config);

KTAG_API void ktag_domain_cache_destroy(ktag_domain_cache *cache);

KTAG_API ktag_status ktag_domain_cache_sync(ktag_domain_cache *cache);

/* required_capacity includes the trailing NUL byte. */
KTAG_API ktag_status ktag_domain_cache_lookup(ktag_domain_cache *cache,
                                              const ktag_ip_address *address,
                                              uint64_t now_us, char *domain,
                                              uint32_t domain_capacity,
                                              uint32_t *required_capacity);

KTAG_API uint64_t
ktag_domain_cache_get_applied_version(const ktag_domain_cache *cache);

KTAG_API ktag_status ktag_domain_cache_get_stats(
    const ktag_domain_cache *cache, ktag_domain_cache_stats *stats);

/*
 * A Feature Context is single-thread owned after its first process/expire/
 * flush/statistics call. Different Contexts may run concurrently.
 * The Schema and Registry are copied into the Context's Provider Pipeline.
 */
KTAG_API ktag_feature_context *
ktag_feature_create(const ktag_feature_config *config);

/* output is cleared first and must have the same schema hash as the Context. */
KTAG_API ktag_status ktag_feature_process_packet(ktag_feature_context *context,
                                                 const ktag_packet *packet,
                                                 ktag_feature_batch *output);

KTAG_API ktag_status ktag_feature_expire(ktag_feature_context *context,
                                         uint64_t current_time_us,
                                         ktag_feature_batch *output);

KTAG_API ktag_status ktag_feature_flush(ktag_feature_context *context,
                                        ktag_feature_batch *output);

KTAG_API ktag_status ktag_feature_get_statistics(
    const ktag_feature_context *context, ktag_feature_statistics *statistics);

/* Active flow callback state is destroyed; call flush first to emit results. */
KTAG_API void ktag_feature_destroy(ktag_feature_context *context);

/* Schema construction. A schema becomes immutable after freeze. */
KTAG_API ktag_feature_schema *
ktag_feature_schema_create(const char *schema_name, uint32_t schema_version);

KTAG_API void ktag_feature_schema_destroy(ktag_feature_schema *schema);

KTAG_API ktag_status ktag_feature_schema_add(
    ktag_feature_schema *schema, const ktag_feature_descriptor *descriptor);

KTAG_API ktag_status ktag_feature_schema_remove(ktag_feature_schema *schema,
                                                const char *feature_id);

KTAG_API ktag_status ktag_feature_schema_freeze(ktag_feature_schema *schema);

KTAG_API uint32_t
ktag_feature_schema_is_frozen(const ktag_feature_schema *schema);

KTAG_API uint32_t
ktag_feature_schema_get_feature_count(const ktag_feature_schema *schema);

KTAG_API uint32_t
ktag_feature_schema_get_value_count(const ktag_feature_schema *schema);

KTAG_API uint64_t
ktag_feature_schema_get_hash(const ktag_feature_schema *schema);

KTAG_API const char *
ktag_feature_schema_get_name(const ktag_feature_schema *schema);

KTAG_API uint32_t
ktag_feature_schema_get_version(const ktag_feature_schema *schema);

/* Returned strings are owned by the schema and remain valid until destroy. */
KTAG_API ktag_status ktag_feature_schema_get_descriptor(
    const ktag_feature_schema *schema, uint32_t index,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset);

KTAG_API ktag_status ktag_feature_schema_find(
    const ktag_feature_schema *schema, const char *feature_id,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset);

/* Factory for the frozen 354-value application-traffic value/CSV contract.
 * Runtime ownership is decomposed into independently replaceable Provider
 * slots without changing the stable model-contract hash. */
KTAG_API ktag_feature_schema *ktag_feature_schema_create_legacy_v1(void);

/*
 * Provider registry. Registration data and descriptors are deep-copied;
 * user_data remains borrowed until every Pipeline made from the Registry is
 * destroyed.
 */
KTAG_API ktag_feature_registry *ktag_feature_registry_create(void);

KTAG_API void ktag_feature_registry_destroy(ktag_feature_registry *registry);

KTAG_API ktag_status ktag_feature_registry_register(
    ktag_feature_registry *registry,
    const ktag_feature_provider_registration *registration, uint32_t flags);

KTAG_API ktag_status ktag_feature_registry_unregister(
    ktag_feature_registry *registry, const char *provider_id);

KTAG_API uint32_t
ktag_feature_registry_get_provider_count(const ktag_feature_registry *registry);

KTAG_API ktag_status ktag_feature_schema_add_registered(
    ktag_feature_schema *schema, const ktag_feature_registry *registry,
    const char *feature_id);

/* Feature callbacks use this writer to populate their declared outputs. */
KTAG_API ktag_status ktag_feature_output_set(ktag_feature_output *output,
                                             const char *feature_id,
                                             const float *values,
                                             uint32_t value_count);

/* Optional diagnostic text is collected only when Feature Config enables it. */
KTAG_API uint32_t
ktag_feature_output_annotations_enabled(const ktag_feature_output *output);

KTAG_API ktag_status ktag_feature_output_set_annotation(
    ktag_feature_output *output, const char *key, const char *value);

/* Read-only flow view accessors. Returned strings are borrowed. */
KTAG_API uint64_t ktag_flow_view_get_flow_id(const ktag_flow_view *flow);

KTAG_API ktag_status ktag_flow_view_get_flow_key(const ktag_flow_view *flow,
                                                 ktag_flow_key *key);

/* Returns the endpoint observed in the requested packet direction. */
KTAG_API ktag_status ktag_flow_view_get_endpoint(
    const ktag_flow_view *flow, ktag_direction direction,
    ktag_ip_address *address, uint16_t *port);

KTAG_API uint32_t ktag_flow_view_get_packet_count(const ktag_flow_view *flow,
                                                  ktag_direction direction);

KTAG_API uint64_t ktag_flow_view_get_total_bytes(const ktag_flow_view *flow,
                                                 ktag_direction direction);

KTAG_API const char *ktag_flow_view_get_domain(const ktag_flow_view *flow,
                                               ktag_direction direction);

/* Read-only packet view accessors for on_packet callbacks. */
KTAG_API const uint8_t *
ktag_packet_view_get_data(const ktag_packet_view *packet, uint32_t *length);

KTAG_API ktag_status ktag_packet_view_get_timestamp(
    const ktag_packet_view *packet, ktag_timestamp *timestamp);

KTAG_API ktag_status ktag_packet_view_get_transport(
    const ktag_packet_view *packet, ktag_packet_transport_view *transport);

KTAG_API const uint8_t *
ktag_packet_view_get_payload(const ktag_packet_view *packet, uint32_t *length);

/* Captured packet/header/payload lengths preserve link padding semantics while
 * keeping per-packet histories inside the owning Provider flow_state. */
KTAG_API ktag_status ktag_packet_view_get_capture_lengths(
    const ktag_packet_view *packet, uint32_t *packet_length,
    uint32_t *header_length, uint32_t *payload_length);

/* Dynamic feature batch. Values are copied when a record is appended. */
KTAG_API ktag_feature_batch *
ktag_feature_batch_create(const ktag_feature_schema *schema,
                          uint32_t initial_capacity);

KTAG_API void ktag_feature_batch_destroy(ktag_feature_batch *batch);

KTAG_API ktag_status ktag_feature_batch_reserve(ktag_feature_batch *batch,
                                                uint32_t capacity);

KTAG_API ktag_status ktag_feature_batch_append(
    ktag_feature_batch *batch, const ktag_feature_record_metadata *metadata,
    const float *values, uint32_t value_count);

KTAG_API void ktag_feature_batch_clear(ktag_feature_batch *batch);

KTAG_API uint32_t ktag_feature_batch_get_count(const ktag_feature_batch *batch);

KTAG_API uint32_t
ktag_feature_batch_get_value_count(const ktag_feature_batch *batch);

KTAG_API uint64_t
ktag_feature_batch_get_schema_hash(const ktag_feature_batch *batch);

KTAG_API ktag_status
ktag_feature_batch_get_record(const ktag_feature_batch *batch, uint32_t index,
                              ktag_feature_record *record);

KTAG_API ktag_status ktag_feature_batch_get_record_details(
    const ktag_feature_batch *batch, uint32_t index,
    ktag_feature_record_details *details);

/* Returned text is borrowed until the batch is mutated or destroyed. */
KTAG_API ktag_status ktag_feature_batch_get_annotation(
    const ktag_feature_batch *batch, uint32_t index, const char *key,
    const char **value);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_FEATURE_H */
