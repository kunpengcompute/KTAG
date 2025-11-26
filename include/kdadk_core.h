/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef KDADK_CORE_H_
#define KDADK_CORE_H_
#include "kdadk_type.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
/////////////////////////////get product version info/////////////////////////////////
 * @param[out]  packageInfo   Pointer to the constant kdadk package information structure.
 * @retva       KDADKResult    KDADK_STS_NO_ERR       Indicates no error.
                              KDADK_STS_NULL_PTR_ERR Indicates an error when any of the specified pointers is NULL.
 */
KDADKResult KDADK_GetProductVersion(KDADKProVersion *packageInfo);
#ifdef __cplusplus
}
#endif

#endif
