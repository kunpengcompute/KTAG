/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "kdadk_test.h"

size_t getFileSize(const char *fileName) {

	if (fileName == NULL) {
		return 0;
	}
	struct stat statbuf;
	stat(fileName, &statbuf);
	size_t filesize = statbuf.st_size;
	return filesize;
}

TEST(KDADK_PERFTEST_E2E, CALCULATE_MEAN)
{
    struct timespec start_sys, end_sys;
    std::vector<uint32_t> test_data(50);

    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, UINT32_MAX);

    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = dist(gen);
    }

    clock_gettime(CLOCK_REALTIME, &start_sys);
    for (int i = 0; i < 1000; ++i) {
        calculate_mean(test_data);
    }
    clock_gettime(CLOCK_REALTIME, &end_sys);
    double time_sys = calculate_time_ms(start_sys, end_sys);

    printf("Performance of calculate_mean : %.6f ms\n", time_sys);
}

TEST(KDADK_PERFTEST_E2E, CALCULATE_STATIC_INFO)
{
    struct timespec start_sys, end_sys;
    std::vector<uint32_t> test_data(50);

    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, UINT32_MAX);
    KDADKResult ret;
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = dist(gen);
    }
    
    statistics_info result;
    clock_gettime(CLOCK_REALTIME, &start_sys);
    for (int i = 0; i < 1000; ++i) {
        ret = cal_statistics_info(test_data, result);
    }
    clock_gettime(CLOCK_REALTIME, &end_sys);
    EXPECT_EQ(ret, KDADK_STS_NO_ERR);
    double time_sys = calculate_time_ms(start_sys, end_sys);
    printf("Performance of cal_statistics_info : %.6f ms\n", time_sys);
}