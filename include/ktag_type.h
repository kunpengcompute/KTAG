/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef KTAG_TYPE_H_
#define KTAG_TYPE_H_
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t major;
    int32_t minor;
    int32_t patch;
    const char *buildDate;
}KTAGLibraryVersion;

#define KTAG_VERSION_INFO_LEN 100
typedef struct {
    char productName[KTAG_VERSION_INFO_LEN];
    char productVersion[KTAG_VERSION_INFO_LEN];
    char componentName[KTAG_VERSION_INFO_LEN];
    char componentVersion[KTAG_VERSION_INFO_LEN];
    char componentAppendInfo[KTAG_VERSION_INFO_LEN];
    char softwareName[KTAG_VERSION_INFO_LEN];
    char softwareVersion[KTAG_VERSION_INFO_LEN];
}KTAGProVersion;

/*
* The following macro defines the status of the KTAG operation.
* value of 0 means no error.
*/
typedef enum {
    KTAG_STS_NO_ERR = 0,
    KTAG_STS_NULL_PTR_ERR,
    KTAG_STS_PARAMETER_ERR,
    KTAG_STS_NOT_SUPPORT,
    KTAG_STS_DIV_BY_ZERO_ERR,
    KTAG_STS_MALLOC_FAILED,
    KTAG_STS_INT32_OVERFLOW_ERR,
    KTAG_STS_INIT_RWLOCK_FAIL,
    KTAG_STS_INVALID_PCAP_PACKET,
    KTAG_STS_DECODE_HTTP_ERROR,
    KTAG_STS_VECTOR_INDEX_OVERFLOW,
    KTAG_STS_FOPEN_FILE_ERR,
    KTAG_STS_DECODE_TLS_ERROR,
    KTAG_STS_UNKNOWN_ERR
} KTAGResult;
#ifdef __cplusplus
}
#endif

#endif /* KTAG_TYPE_H__ */
