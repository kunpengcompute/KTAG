/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "kdadk_test.h"
extern "C" const KDADKLibraryVersion* KDADK_GetLibVersion();
typedef KDADKResult (*GetProductVersionFn)(KDADKProVersion *packageInfo);
typedef const KDADKLibraryVersion* (*GetLibVersionFunc)();

TEST(GetProductVersionTest, ReturnProductVersion) {
    void* handle = dlopen(LIB_KDADK_APPID_NAME, RTLD_LAZY);
    ASSERT_NE(handle, nullptr) << "Failed to open shared library";
    GetProductVersionFn GetProductVersion = (GetProductVersionFn) dlsym(handle, "KDADK_GetProductVersion");
    ASSERT_NE(GetProductVersion, nullptr) << "Failed to find GetProductVersion function";

    KDADKProVersion packageInfo;
    KDADKResult result = GetProductVersion(&packageInfo);

    EXPECT_EQ(result, KDADK_STS_NO_ERR);

    printf("GetProductVersion worked correctly.\n");
    printf("Product Name: %s\n", packageInfo.productName);
    printf("Product Version: %s\n", packageInfo.productVersion);
    printf("ComponentName: %s\n", packageInfo.componentName);
    printf("ComponentVersion: %s\n", packageInfo.componentVersion);

    GetLibVersionFunc getLibVersion = (GetLibVersionFunc)dlsym(handle, "KDADK_GetLibVersion");
    ASSERT_NE(getLibVersion, nullptr) << "Failed to find function: " << dlerror();
    const KDADKLibraryVersion* libVersion = getLibVersion();
    ASSERT_NE(libVersion, nullptr);

    printf("Library Version:\n");
    printf("Major: %d\n", libVersion->major);
    printf("Minor: %d\n", libVersion->minor);
    printf("Patch: %d\n", libVersion->patch);
    printf("Build Date: %s\n", libVersion->buildDate);
    dlclose(handle);
}