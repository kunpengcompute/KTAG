/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "kdadk_test.h"

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

test\common\perftest\perf_feature_extract.cpp

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

    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = dist(gen);
    }
    
    statistics_info result;
    clock_gettime(CLOCK_REALTIME, &start_sys);
    for (int i = 0; i < 1000; ++i) {
        cal_statistics_info(test_data, result);
    }
    clock_gettime(CLOCK_REALTIME, &end_sys);
    double time_sys = calculate_time_ms(start_sys, end_sys);
    printf("Performance of cal_statistics_info : %.6f ms\n", time_sys);
}

TEST(KDADK_PERFTEST_E2E, FEATURE_EXTRACT)
{
    struct timespec start_sys, end_sys;
    int ITER = 1;
    std::string pcap_file = GetFirstPcapFile();
    size_t pcapFileSize = getFileSize(pcap_file.c_str());
    clock_gettime(CLOCK_REALTIME, &start_sys);
    for (int i = 0; i < ITER; ++i) {
        feature_extract(pcap_file.c_str());
    }
    clock_gettime(CLOCK_REALTIME, &end_sys);
    double time_ms = calculate_time_ms(start_sys, end_sys);
    double avg_time_ms = time_ms / ITER;                      // 毫秒
    double avg_time_s = avg_time_ms / 1000.0;                 // 秒
    printf("pcap file: %s\n", pcap_file.c_str());
    printf("pcap file size: %d B, (%.5f MB)\n", pcapFileSize, pcapFileSize / (1024.0 * 1024.0));
    printf("Total time: %.3f ms  of ITER: %d\n", time_ms, ITER);
    printf("Average time per iteration: %.3f ms (%.6f s)\n", avg_time_ms, avg_time_s);
    
    // 速率计算
    double speed_bps = pcapFileSize / avg_time_s;             // B/s
    double speed_mbps = speed_bps / (1024.0 * 1024.0);       // MB/s
    double speed_gbps = speed_bps * 8 / (1000.0 * 1000.0 * 1000.0);  // Gbps
    
    printf("Processing speed: %.4f MB/s\n", speed_mbps);
    printf("Processing speed: %.4f Gbps\n", speed_gbps);
}