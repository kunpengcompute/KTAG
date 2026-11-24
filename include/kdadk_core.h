/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Megascan is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 */
#ifndef KDADK_CORE_H_
#define KDADK_CORE_H_
#include "kdadk_type.h"
#include "kdadk_export.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
/////////////////////////////get product version info/////////////////////////////////
 * @param[out]  packageInfo   Pointer to the constant kdadk package information structure.
 * @retva       KDADKResult    KDADK_STS_NO_ERR       Indicates no error.
                              KDADK_STS_NULL_PTR_ERR Indicates an error when any of the specified pointers is NULL.
 */
KDADKResult KDADK_API KDADK_GetProductVersion(KDADKProVersion *packageInfo);
#ifdef __cplusplus
}
#endif

#endif
