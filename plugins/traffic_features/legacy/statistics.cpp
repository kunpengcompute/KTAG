/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#include "statistics.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace ktag_traffic_features::legacy {
double calculate_mean(const std::vector<uint32_t> &nums)
{
    if (nums.empty()) {
        return 0.0;
    }
    double sum = accumulate(nums.begin(), nums.end(), 0.0);
    double mean = sum / nums.size();
    return mean;
}

double calculate_var(const std::vector<uint32_t> &nums, double mean)
{
    if (nums.empty()) {
        return 0.0;
    }
    double variance = 0.0;
    for (double num : nums) {
        variance += pow(num - mean, 2);
    }
    variance /= nums.size();
    return variance;
}

void calculate_hist(const std::vector<uint32_t> &nums, double min_val, double max_val, double *histogram,
                    int histogram_size, bool isTimeInterval)
{
    if (histogram == nullptr || histogram_size == 0) {
        return;
    }

    const double epsilon = 1e-5;
    double range = max_val - min_val;
    if (range < epsilon) {
        histogram[0] += nums.size();
        return;
    }

    int bin;
    for (double num : nums) {
        if (isTimeInterval) {
            num = log2(num + 1);
        }
        bin = static_cast<int>(floor((num - min_val) / range * histogram_size));

        bin = bin < 0 ? 0 : bin;
        bin = bin >= histogram_size ? histogram_size - 1 : bin;
        histogram[bin]++;
    }
    return;
}

void calculate_hist_abs(const std::vector<uint32_t> &nums, double max_val, double *histogram, int histogram_size,
                        bool isTimeInterval)
{
    if (histogram == nullptr || histogram_size == 0) {
        return;
    }

    const double epsilon = 1e-5;
    const double div_num = (max_val / histogram_size);
    if (div_num < epsilon) {
        histogram[0] += nums.size();
        return;
    }

    int bin;
    for (double num : nums) {
        if (isTimeInterval) {
            num = log2(num + 1);
        }
        bin = static_cast<int>(floor(num / div_num));

        bin = bin < 0 ? 0 : bin;
        bin = bin >= histogram_size ? histogram_size - 1 : bin;
        histogram[bin]++;
    }
    return;
}

void calculate_hist_nor(const std::vector<uint32_t> &nums, double mean_val, double var_val, double *histogram,
                        int histogram_size, bool isTimeInterval)
{
    if (histogram == nullptr || histogram_size == 0) {
        return;
    }

    const double epsilon = 1e-5;
    if (var_val < epsilon) {
        histogram[0] += nums.size();
        return;
    }

    int bin;
    for (double num : nums) {
        if (isTimeInterval) {
            num = log2(num + 1);
        }
        bin = static_cast<int>(floor((num - mean_val) / var_val * histogram_size));

        bin = bin < 0 ? 0 : bin;
        bin = bin >= histogram_size ? histogram_size - 1 : bin;
        histogram[bin]++;
    }
    return;
}

void cal_diffs(const std::vector<struct timeval> &ts, std::vector<uint32_t> &time_diff)
{
    if (ts.size() <= 1 || (ts.size() - 1) != time_diff.size()) {
        return;
    }

    for (size_t i = 1; i < ts.size(); ++i) {
        if (ts[i].tv_usec > 999999 || ts[i - 1].tv_usec > 999999) {
            continue;
        }
        int64_t current = ts[i].tv_sec * 1000000LL + ts[i].tv_usec;
        int64_t previous = ts[i - 1].tv_sec * 1000000LL + ts[i - 1].tv_usec;

        if (current < previous) {
            continue;
        }

        uint32_t diff_micro = static_cast<uint32_t>(current - previous);

        time_diff[i - 1] = diff_micro;
    }

    return;
}

bool cal_statistics_info(const std::vector<uint32_t> &array, statistics_info &array_si, bool isTimeInterval)
{
    array_si = {};
    if (array.size() == 0) {
        return false;
    }

    // 计算最大值，最小值，均值和方差
    array_si.min_value = array[0];
    array_si.max_value = array[0];
    array_si.mean_value = 0.0;
    array_si.var_value = 0.0;

    double sum = 0.0;
    double sum_sq = 0.0;
    double cur_num = 0.0;
    for (uint32_t i = 0; i < array.size(); ++i) {
        array_si.min_value = array_si.min_value < array[i] ? array_si.min_value : array[i];
        array_si.max_value = array_si.max_value > array[i] ? array_si.max_value : array[i];

        cur_num = static_cast<double>(array[i]);
        sum += cur_num;
        sum_sq += cur_num * cur_num;
    }
    array_si.mean_value = sum / array.size();
    if (array.size() == 1) {
        array_si.var_value = 0.0;
    } else {
        array_si.var_value = (sum_sq - (sum * sum) / array.size()) / (array.size() - 1);
    }

    // 如果是统计的是时间间隔，则对结果取2为底的对数
    if (isTimeInterval) {
        array_si.min_value = log2(array_si.min_value + 1);
        array_si.max_value = log2(array_si.max_value + 1);
        array_si.mean_value = log2(array_si.mean_value + 1);
        array_si.var_value = log2(array_si.var_value + 1);  // 防止方差为0
    }

    calculate_hist(array, array_si.min_value, array_si.max_value, array_si.hist_array, HISTOGRAM_NUMS, isTimeInterval);
    calculate_hist_abs(array, array_si.max_value, array_si.hist_array_abs, HISTOGRAM_NUMS, isTimeInterval);
    calculate_hist_nor(array, array_si.mean_value, array_si.var_value, array_si.hist_array_nor, HISTOGRAM_NUMS,
                       isTimeInterval);
    return true;
}


} // namespace ktag_traffic_features::legacy
