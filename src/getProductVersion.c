/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Megascan is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "kdadk_core.h"
#include "kdadk_export.h"
#include "kdadk_appidver.h"
const KDADKLibraryVersion *KDADK_GetLibVersion()
{
    static KDADKLibraryVersion libVersion = {KDADK_APPID_VERSION_MAJOR, KDADK_APPID_VERSION_MINOR,
                                             KDADK_APPID_VERSION_PATCH, KDADK_APPID_VERSION_BUILDDATE};
    return &libVersion;
}

KDADK_API KDADKResult KDADK_GetProductVersion(KDADKProVersion *packageInfo)
{
    if (packageInfo == NULL) {
        return KDADK_STS_NULL_PTR_ERR;
    }
    
    snprintf(packageInfo->componentName, KDADK_VERSION_INFO_LEN, "%s", KDADK_APPID_COMPONENT_NAME);
    snprintf(packageInfo->componentVersion, KDADK_VERSION_INFO_LEN, "%s", KDADK_APPID_VERSION);
    snprintf(packageInfo->componentAppendInfo, KDADK_VERSION_INFO_LEN, "%s", KDADK_APPID_TOOLCHAIN_INFO);
    snprintf(packageInfo->productName, KDADK_VERSION_INFO_LEN, "%s", "Kunpeng Boostkit");
    snprintf(packageInfo->productVersion, KDADK_VERSION_INFO_LEN, "%s", KDADK_APPID_PRODUCT_VERSION);
    snprintf(packageInfo->softwareName, KDADK_VERSION_INFO_LEN, "%s", KDADK_APPID_SOFTWARE_NAME);
    snprintf(packageInfo->softwareVersion, KDADK_VERSION_INFO_LEN, "%s", KDADK_APPID_VERSION);
    
    return KDADK_STS_NO_ERR;
}
