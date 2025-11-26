/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef KDADK_TYPE_H_
#define KDADK_TYPE_H_
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t major;
    int32_t minor;
    int32_t patch;
    const char *buildDate;
}KDADKLibraryVersion;

#define KDADK_VERSION_INFO_LEN 100
typedef struct {
    char productName[KDADK_VERSION_INFO_LEN];
    char productVersion[KDADK_VERSION_INFO_LEN];
    char componentName[KDADK_VERSION_INFO_LEN];
    char componentVersion[KDADK_VERSION_INFO_LEN];
    char componentAppendInfo[KDADK_VERSION_INFO_LEN];
    char softwareName[KDADK_VERSION_INFO_LEN];
    char softwareVersion[KDADK_VERSION_INFO_LEN];
}KDADKProVersion;

/*
* The following macro defines the status of the KDADK operation.
* value of 0 means no error.
*/
typedef enum {
    KDADK_STS_NO_ERR = 0,
    KDADK_STS_NULL_PTR_ERR,
    KDADK_STS_PARAMETER_ERR,
    KDADK_STS_NOT_SUPPORT,
    KDADK_STS_DIV_BY_ZERO_ERR,
    KDADK_STS_MALLOC_FAILED,
    KDADK_STS_INT32_OVERFLOW_ERR,
    KDADK_STS_INIT_RWLOCK_FAIL,
    KDADK_STS_INVALID_PCAP_PACKET,
    KDADK_STS_DECODE_HTTP_ERROR,
    KDADK_STS_VECTOR_INDEX_OVERFLOW,
    KDADK_STS_FOPEN_FILE_ERR,
    KDADK_STS_DECODE_TLS_ERROR
} KDADKResult;
#ifdef __cplusplus
}
#endif

#endif /* KDADK_TYPE_H__ */
