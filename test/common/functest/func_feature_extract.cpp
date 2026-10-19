#include <gtest/gtest.h>
#include <limits>
#include <array>
#include <cmath>
#include "legacy/statistics.h"
using namespace ktag_traffic_features::legacy;

TEST(KTAG_FEATURE_EXTRACT, CALCULATE_MEAN_Basic)
{
    std::vector<uint32_t> test_data = {1, 2, 3, 4, 5};
    double mean_result = calculate_mean(test_data);

    ASSERT_DOUBLE_EQ(mean_result, 3);
}


TEST(KTAG_FEATURE_EXTRACT, CALCULATE_MEAN_SingleElement)
{
    std::vector<uint32_t> nums = {42};
    EXPECT_DOUBLE_EQ(42.0, calculate_mean(nums));
}


TEST(KTAG_FEATURE_EXTRACT, CALCULATE_MEAN_LargeNumbers)
{
    std::vector<uint32_t> nums = {
        100'000'000,
        200'000'000,
        300'000'000
    };
    EXPECT_DOUBLE_EQ(200'000'000.0, calculate_mean(nums));
}


TEST(KTAG_FEATURE_EXTRACT, CALCULATE_MEAN_MaxUint32)
{
    uint32_t max_val = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> nums = {max_val, max_val, max_val};
    EXPECT_DOUBLE_EQ(static_cast<double>(max_val), calculate_mean(nums));
}


TEST(KTAG_FEATURE_EXTRACT, CALCULATE_VAR)
{
    std::vector<uint32_t> nums = {1, 2, 3, 4, 5};
    double mean = 3.0;
    double expected = (4+1+0+1+4)/5.0; // (2^2 + 1^2 + 0 + 1^2 + 2^2)/5
    EXPECT_DOUBLE_EQ(expected, calculate_var(nums, mean));
}


TEST(KTAG_FEATURE_EXTRACT, CALCULATE_HIST_NullPointer)
{
    std::vector<uint32_t> nums = {1, 2, 3};
    calculate_hist(nums, 1, 3, nullptr, HISTOGRAM_NUMS);
    SUCCEED(); // 不崩溃即通过
}


TEST(KTAG_FEATURE_EXTRACT, CALCULATE_HIST_InvalidRange)
{
    std::vector<uint32_t> nums = {5, 10, 15};
    std::array<double, HISTOGRAM_NUMS> storage{};
    double *hist = storage.data();
    calculate_hist(nums, 10.0, 10.0 + 1e-6, hist, HISTOGRAM_NUMS);

    EXPECT_DOUBLE_EQ(nums.size(), hist[0]);
    for (int i = 1; i < HISTOGRAM_NUMS; i++) {
        EXPECT_DOUBLE_EQ(0.0, hist[i]);
    }
}


TEST(KTAG_FEATURE_EXTRACT, CALCULATE_HIST_Basic)
{
    std::vector<uint32_t> nums = {
        0,
        1000,
        50000,
        1000000,
        std::numeric_limits<uint32_t>::max()
    };
    std::array<double, HISTOGRAM_NUMS> storage{};
    double *hist = storage.data();
    calculate_hist(nums, 0.0, 1000000.0, hist, HISTOGRAM_NUMS);

    EXPECT_DOUBLE_EQ(3.0, hist[0]);   // 0, 1000, 50000
    EXPECT_DOUBLE_EQ(2.0, hist[15]);  // 1000000 和 max_value
    for (int i = 1; i < 15; i++) {
        EXPECT_DOUBLE_EQ(0.0, hist[i]);
    }
}


TEST(KTAG_FEATURE_EXTRACT, CAL_DIFFS_EmptyTS)
{
    std::vector<struct timeval> ts;
    std::vector<uint32_t> time_diff(1, 555);
    cal_diffs(ts, time_diff);

    // time_diff应保持原状不修改
    EXPECT_EQ((uint32_t)1, time_diff.size());
    EXPECT_EQ((uint32_t)555, time_diff[0]);
}


TEST(KTAG_FEATURE_EXTRACT, CAL_DIFFS_SizeMismatch)
{
    std::vector<struct timeval> ts = {
        {100, 0},
        {101, 0},
        {102, 0}
    };
    std::vector<uint32_t> time_diff(1, 555);
    cal_diffs(ts, time_diff);

    // time_diff应保持原状不修改
    EXPECT_EQ((uint32_t)1, time_diff.size());
    EXPECT_EQ((uint32_t)555, time_diff[0]);
}


TEST(KTAG_FEATURE_EXTRACT, CAL_DIFFS_Basic)
{
    std::vector<struct timeval> ts = {
        {100, 123456},
        {101, 123456},
        {101, 123456},
        {101, 123457}
    };
    std::vector<uint32_t> time_diff(3, 0);

    cal_diffs(ts, time_diff);

    ASSERT_EQ(3, time_diff.size());
    EXPECT_EQ(1000000, time_diff[0]);
    EXPECT_EQ(0, time_diff[1]);
    EXPECT_EQ(1, time_diff[2]);
}


TEST(KTAG_FEATURE_EXTRACT, CAL_STATISTICS_INFO_EmptyArrary)
{
    std::vector<uint32_t> empty;
    statistics_info si;

    bool res = cal_statistics_info(empty, si);
    EXPECT_EQ(res, false);
}


TEST(KTAG_FEATURE_EXTRACT, FrozenStatisticsUseSampleVarianceAndResetHistograms) {
    statistics_info result{};
    ASSERT_TRUE(cal_statistics_info({2U, 4U, 6U}, result));
    EXPECT_DOUBLE_EQ(4.0, result.mean_value);
    EXPECT_DOUBLE_EQ(4.0, result.var_value);
    ASSERT_TRUE(cal_statistics_info({5U}, result));
    EXPECT_DOUBLE_EQ(0.0, result.var_value);
    EXPECT_DOUBLE_EQ(1.0, result.hist_array[0]);
    for (int i = 1; i < HISTOGRAM_NUMS; ++i) EXPECT_DOUBLE_EQ(0.0, result.hist_array[i]);
}
TEST(KTAG_FEATURE_EXTRACT, FrozenIntervalStatisticsKeepLogarithmicRules) {
    statistics_info result{};
    ASSERT_TRUE(cal_statistics_info({1U, 3U}, result, true));
    EXPECT_DOUBLE_EQ(1.0, result.min_value);
    EXPECT_DOUBLE_EQ(2.0, result.max_value);
    EXPECT_DOUBLE_EQ(std::log2(3.0), result.mean_value);
    EXPECT_DOUBLE_EQ(std::log2(3.0), result.var_value);
}
