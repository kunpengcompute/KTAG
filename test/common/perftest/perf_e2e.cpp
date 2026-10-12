/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "kdadk_test.h"
#include "feature_extract.h"

TEST(KDADK_PERFTEST_E2E, TimeTest)
{
    struct timespec start_sys, end_sys;
    std::vector<uint32_t> test_data = {1, 2, 3, 4, 5};

    clock_gettime(CLOCK_REALTIME, &start_sys);
    for (int i = 0; i < 1000; ++i) {
        calculate_mean(test_data);
    }
    clock_gettime(CLOCK_REALTIME, &end_sys);
    double time_sys = calculate_time_ms(start_sys, end_sys);

    printf("Performance of E2E : %.6f ms\n", time_sys);
}