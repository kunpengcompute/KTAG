/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "ktag_core.h"
#include "ktag_export.h"
#include "ktag_appidver.h"
const KTAGLibraryVersion *KTAG_GetLibVersion()
{
    static KTAGLibraryVersion libVersion = {KTAG_APPID_VERSION_MAJOR, KTAG_APPID_VERSION_MINOR,
                                             KTAG_APPID_VERSION_PATCH, KTAG_APPID_VERSION_BUILDDATE};
    return &libVersion;
}

KTAG_API KTAGResult KTAG_GetProductVersion(KTAGProVersion *packageInfo)
{
    if (packageInfo == NULL) {
        return KTAG_STS_NULL_PTR_ERR;
    }
    
    snprintf(packageInfo->componentName, KTAG_VERSION_INFO_LEN, "%s", KTAG_APPID_COMPONENT_NAME);
    snprintf(packageInfo->componentVersion, KTAG_VERSION_INFO_LEN, "%s", KTAG_APPID_VERSION);
    snprintf(packageInfo->componentAppendInfo, KTAG_VERSION_INFO_LEN, "%s", KTAG_APPID_TOOLCHAIN_INFO);
    snprintf(packageInfo->productName, KTAG_VERSION_INFO_LEN, "%s", "Kunpeng Boostkit");
    snprintf(packageInfo->productVersion, KTAG_VERSION_INFO_LEN, "%s", KTAG_APPID_PRODUCT_VERSION);
    snprintf(packageInfo->softwareName, KTAG_VERSION_INFO_LEN, "%s", KTAG_APPID_SOFTWARE_NAME);
    snprintf(packageInfo->softwareVersion, KTAG_VERSION_INFO_LEN, "%s", KTAG_APPID_VERSION);
    
    return KTAG_STS_NO_ERR;
}
