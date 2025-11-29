/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "securec.h"
#include "kdadk_core.h"
#include "kdadk_appidver.h"
const KDADKLibraryVersion *KDADK_GetLibVersion()
{
    static KDADKLibraryVersion libVersion = {KDADK_APPID_VERSION_MAJOR, KDADK_APPID_VERSION_MINOR,
                                             KDADK_APPID_VERSION_PATCH, KDADK_APPID_VERSION_BUILDDATE};
    return &libVersion;
}

KDADKResult KDADK_GetProductVersion(KDADKProVersion *packageInfo)
{
    if (packageInfo == NULL) {
        return KDADK_STS_NULL_PTR_ERR;
    }
    strcpy_s(packageInfo->componentName, KDADK_VERSION_INFO_LEN, KDADK_APPID_COMPONENT_NAME);
    strcpy_s(packageInfo->componentVersion, KDADK_VERSION_INFO_LEN, KDADK_APPID_VERSION);
    strcpy_s(packageInfo->componentAppendInfo, KDADK_VERSION_INFO_LEN, KDADK_APPID_TOOLCHAIN_INFO);
    strcpy_s(packageInfo->productName, KDADK_VERSION_INFO_LEN, "Kunpeng Boostkit");
    strcpy_s(packageInfo->productVersion, KDADK_VERSION_INFO_LEN, KDADK_APPID_PRODUCT_VERSION);
    strcpy_s(packageInfo->softwareName, KDADK_VERSION_INFO_LEN, KDADK_APPID_SOFTWARE_NAME);
    strcpy_s(packageInfo->softwareVersion, KDADK_VERSION_INFO_LEN, KDADK_APPID_VERSION);
    return KDADK_STS_NO_ERR;
}
