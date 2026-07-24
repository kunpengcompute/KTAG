/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_TYPES_H
#define KTAG_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KTAG_API_VERSION 1U
#define KTAG_IPV6_ADDRESS_SIZE 16U

typedef int32_t ktag_status;

enum {
    KTAG_SUCCESS = 0,
    KTAG_STATUS_TIMEOUT = 1,
    KTAG_STATUS_END_OF_STREAM = 2,
    KTAG_ERROR_INVALID_ARGUMENT = -1,
    KTAG_ERROR_OUT_OF_MEMORY = -2,
    KTAG_ERROR_ALREADY_EXISTS = -3,
    KTAG_ERROR_NOT_FOUND = -4,
    KTAG_ERROR_INVALID_STATE = -5,
    KTAG_ERROR_CAPACITY = -6,
    KTAG_ERROR_SCHEMA_MISMATCH = -7,
    KTAG_ERROR_INTERNAL = -8,
    KTAG_ERROR_NOT_SUPPORTED = -9,
    KTAG_ERROR_IO = -10
};

typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
} ktag_struct_header;

typedef struct {
    int64_t seconds;
    int32_t microseconds;
} ktag_timestamp;

typedef struct {
    uint8_t bytes[KTAG_IPV6_ADDRESS_SIZE];
    uint8_t version;
    uint8_t reserved[3];
} ktag_ip_address;

/*
 * Canonical bidirectional flow key. Endpoint A must sort before endpoint B.
 * IPv4 addresses occupy bytes[0..3], with the remaining bytes set to zero.
 */
typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    ktag_ip_address address_a;
    ktag_ip_address address_b;
    uint16_t port_a;
    uint16_t port_b;
    uint8_t protocol;
    uint8_t reserved[3];
} ktag_flow_key;

#ifdef __cplusplus
}
#endif

#endif /* KTAG_TYPES_H */
