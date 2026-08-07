/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_CAPTURE_H
#define KTAG_CAPTURE_H

#include <stdint.h>

#include "ktag_export.h"
#include "ktag_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KTAG_CAPTURE_LINKTYPE_ETHERNET 1U
#define KTAG_CAPTURE_LINKTYPE_RAW 12U
#define KTAG_CAPTURE_LINKTYPE_LINUX_SLL 113U
#define KTAG_CAPTURE_LINKTYPE_LINUX_SLL2 276U
#define KTAG_WORKER_INDEX_INVALID UINT32_MAX

typedef struct ktag_capture_context ktag_capture_context;
typedef struct ktag_packet_buffer ktag_packet_buffer;
typedef struct ktag_capture_device_list ktag_capture_device_list;

typedef enum {
    KTAG_CAPTURE_SOURCE_PCAP_FILE = 1,
    KTAG_CAPTURE_SOURCE_LIVE_INTERFACE = 2
} ktag_capture_source_type;

typedef enum {
    /* data is valid only until the next next() call or capture close. */
    KTAG_CAPTURE_PACKET_BORROWED = 0,
    /* data is copied into a reference-counted PacketBuffer. */
    KTAG_CAPTURE_PACKET_OWNED = 1
} ktag_capture_packet_mode;

typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    ktag_capture_source_type source_type;
    const char *source;
    ktag_capture_packet_mode packet_mode;
    uint32_t snapshot_length;
    uint32_t promiscuous;
    uint32_t read_timeout_ms;
    uint32_t live_buffer_size;
    uint32_t packet_buffer_capacity;
} ktag_capture_config;

typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    const uint8_t *data;
    uint32_t captured_length;
    uint32_t original_length;
    uint32_t link_type;
    uint64_t sequence;
    ktag_timestamp timestamp;
    ktag_packet_buffer *owner;
} ktag_packet;

/* Initializes fresh packet storage before its first capture_next() call. */
KTAG_API void ktag_packet_init(ktag_packet *packet);

/* Opens an offline PCAP file or a live interface. No worker thread is created. */
KTAG_API ktag_capture_context *ktag_capture_open(
    const ktag_capture_config *config);

/*
 * Returns KTAG_SUCCESS, KTAG_STATUS_TIMEOUT, KTAG_STATUS_END_OF_STREAM, or an
 * error. packet must first be initialized with ktag_packet_init(). An owned
 * packet must be released before the same output object is passed to next()
 * again. ktag_packet_release() leaves the object initialized and reusable.
 */
KTAG_API ktag_status ktag_capture_next(
    ktag_capture_context *context,
    ktag_packet *packet);

KTAG_API uint32_t ktag_capture_get_link_type(
    const ktag_capture_context *context);

/* Wakes a blocked next() call, which then reports END_OF_STREAM. */
KTAG_API void ktag_capture_interrupt(ktag_capture_context *context);

KTAG_API void ktag_capture_close(ktag_capture_context *context);

/* Thread-local detail for the most recent Capture API failure. */
KTAG_API const char *ktag_capture_get_last_error(void);

/*
 * Converts a borrowed packet into a standalone owned copy. It is then safe to
 * enqueue the packet across threads and to call capture_next() again.
 */
KTAG_API ktag_status ktag_packet_make_owned(ktag_packet *packet);

/*
 * Adds one reference to an owned packet view. Copy the ktag_packet value first,
 * retain the copy, then release each view exactly once. Borrowed packets are
 * rejected.
 */
KTAG_API ktag_status ktag_packet_retain(ktag_packet *packet);

/* Releases one owned reference and clears this packet view. */
KTAG_API void ktag_packet_release(ktag_packet *packet);

KTAG_API uint32_t ktag_packet_is_owned(const ktag_packet *packet);

/* Lightweight, read-only L2/L3/L4 parsing and bidirectional normalization. */
KTAG_API ktag_status ktag_packet_get_flow_key(
    const ktag_packet *packet,
    ktag_flow_key *key);

/* Stable hash over logical key fields; no struct padding is hashed. */
KTAG_API uint64_t ktag_flow_hash(
    const ktag_flow_key *key,
    uint64_t seed);

KTAG_API uint32_t ktag_flow_select_worker(
    const ktag_flow_key *key,
    uint32_t worker_count,
    uint64_t seed);

/* Optional device discovery used by the demo without exposing libpcap types. */
KTAG_API ktag_capture_device_list *ktag_capture_list_devices(void);

KTAG_API void ktag_capture_device_list_destroy(
    ktag_capture_device_list *devices);

KTAG_API uint32_t ktag_capture_device_list_get_count(
    const ktag_capture_device_list *devices);

KTAG_API const char *ktag_capture_device_list_get_name(
    const ktag_capture_device_list *devices,
    uint32_t index);

KTAG_API const char *ktag_capture_device_list_get_description(
    const ktag_capture_device_list *devices,
    uint32_t index);

KTAG_API uint32_t ktag_capture_device_list_get_ipv4_count(
    const ktag_capture_device_list *devices,
    uint32_t index);

KTAG_API const char *ktag_capture_device_list_get_ipv4(
    const ktag_capture_device_list *devices,
    uint32_t device_index,
    uint32_t address_index);

KTAG_API uint32_t ktag_capture_device_list_get_ipv6_count(
    const ktag_capture_device_list *devices,
    uint32_t index);

KTAG_API const char *ktag_capture_device_list_get_ipv6(
    const ktag_capture_device_list *devices,
    uint32_t device_index,
    uint32_t address_index);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_CAPTURE_H */
