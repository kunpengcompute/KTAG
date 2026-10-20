/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#ifndef KTAG_LEGACY_STATISTICS_H
#define KTAG_LEGACY_STATISTICS_H
#include <cstdint>
#include <vector>
#include <sys/time.h>
namespace ktag_traffic_features::legacy {
inline constexpr int HISTOGRAM_NUMS = 16;
struct statistics_info {
    double min_value = 0, max_value = 0, mean_value = 0, var_value = 0;
    double hist_array[HISTOGRAM_NUMS]{};
    double hist_array_abs[HISTOGRAM_NUMS]{};
    double hist_array_nor[HISTOGRAM_NUMS]{};
};
double calculate_mean(const std::vector<uint32_t> &nums);
double calculate_var(const std::vector<uint32_t> &nums, double mean);
void calculate_hist(const std::vector<uint32_t> &, double, double, double *, int, bool = false);
void calculate_hist_abs(const std::vector<uint32_t> &, double, double *, int, bool = false);
void calculate_hist_nor(const std::vector<uint32_t> &, double, double, double *, int, bool = false);
void cal_diffs(const std::vector<timeval> &, std::vector<uint32_t> &);
bool cal_statistics_info(const std::vector<uint32_t> &, statistics_info &, bool = false);
}
#endif
