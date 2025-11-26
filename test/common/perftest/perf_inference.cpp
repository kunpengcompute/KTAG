/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "kdadk_test.h"

TEST(KDADK_PERFTEST_INFERENCE, TimeTest)
{
    struct timespec start_sys, end_sys;
    int ITER = 10;
    std::string config_file = "../../../src/config.yaml";
    clock_gettime(CLOCK_REALTIME, &start_sys);
    for (int i = 0; i < ITER; ++i) {
        KDADK_Inference(config_file);
    }
    clock_gettime(CLOCK_REALTIME, &end_sys);
    double time_ms = calculate_time_ms(start_sys, end_sys);
    double avg_time_ms = time_ms / ITER;                      // 毫秒
    double avg_time_s = avg_time_ms / 1000.0;                 // 秒
    printf("Inference total time: %.3f ms  of ITER: %d\n", time_ms, ITER);
    printf("Inference average time per iteration: %.3f ms (%.6f s)\n", avg_time_ms, avg_time_s);
}