/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_REPORT_TYPES_H
#define KTAG_DEMO_REPORT_TYPES_H

#include <stdint.h>

typedef struct {
    int *classes;
    int num_classes;
    char *context;
    int outside_report_predictions;
    double *precision;
    double *recall;
    double *f1_score;
    int *support;
    double accuracy;
    double macro_avg_precision;
    double macro_avg_recall;
    double macro_avg_f1;
    double weighted_avg_precision;
    double weighted_avg_recall;
    double weighted_avg_f1;
} ClassificationReport;

typedef struct {
    double pcap_read_time;
    /* Timed Feature API calls; parallel stages use the busiest worker. */
    double feature_extraction_time;
    /* Sum of compute time across all Feature workers (CPU-seconds). */
    double feature_cpu_time;
    /* Timed Inference API calls, excluding Demo input/output conversion. */
    double inference_time;
    /* Sum of compute time across all Inference workers (CPU-seconds). */
    double inference_cpu_time;
    double end_to_end_time;
    uint64_t total_bytes;
    uint64_t total_packets;
    uint64_t inference_inputs;
    uint64_t predictions;
} PerformanceStats;

#endif /* KTAG_DEMO_REPORT_TYPES_H */
