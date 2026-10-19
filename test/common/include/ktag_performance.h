/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#ifndef KTAG_PERFORMANCE_H
#define KTAG_PERFORMANCE_H

#include "ktag_test.h"

// 性能指标结构
struct PerformanceMetrics {
    double total_time_ms;       // 总耗时（毫秒）
    double avg_time_ms;         // 平均耗时
    double min_time_ms;         // 最小耗时
    double max_time_ms;         // 最大耗时
    uint64_t total_operations;  // 总操作数
    double throughput;          // 吞吐量（操作/秒）
    double memory_usage_mb;     // 内存使用（MB）
};

// 性能计时器
class PerformanceTimer {
private:
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point end_time;
    std::vector<double> durations;

public:
    void start()
    {
        start_time = std::chrono::high_resolution_clock::now();
    }

    void stop()
    {
        end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        durations.push_back(duration.count() / 1000.0);  // 转换为毫秒
    }

    PerformanceMetrics getMetrics(uint64_t operations = 0)
    {
        PerformanceMetrics metrics;

        if (durations.empty()) {
            memset(&metrics, 0, sizeof(metrics));
            return metrics;
        }

        metrics.total_time_ms = 0;
        metrics.min_time_ms = durations[0];
        metrics.max_time_ms = durations[0];

        for (double d : durations) {
            metrics.total_time_ms += d;
            if (d < metrics.min_time_ms)
                metrics.min_time_ms = d;
            if (d > metrics.max_time_ms)
                metrics.max_time_ms = d;
        }

        metrics.avg_time_ms = metrics.total_time_ms / durations.size();
        metrics.total_operations = operations > 0 ? operations : durations.size();
        metrics.throughput = (metrics.total_operations * 1000.0) / metrics.total_time_ms;
        metrics.memory_usage_mb = 0;  // 需要外部设置

        return metrics;
    }

    void reset()
    {
        durations.clear();
    }
};

#endif