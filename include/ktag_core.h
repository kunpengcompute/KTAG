/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef KTAG_CORE_H_
#define KTAG_CORE_H_
#include "ktag_type.h"
#include "ktag_export.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
/////////////////////////////get product version info/////////////////////////////////
 * @param[out]  packageInfo   Pointer to the constant ktag package information structure.
 * @retva       KTAGResult    KTAG_STS_NO_ERR       Indicates no error.
                              KTAG_STS_NULL_PTR_ERR Indicates an error when any of the specified pointers is NULL.
 */
KTAGResult KTAG_API KTAG_GetProductVersion(KTAGProVersion *packageInfo);
#ifdef __cplusplus
}
#endif

#endif
