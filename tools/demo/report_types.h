/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_REPORT_TYPES_H
#define KTAG_DEMO_REPORT_TYPES_H

#include <stdint.h>

typedef struct {
    int *classes;
    int num_classes;
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
    double feature_extraction_time;
    double inference_time;
    double file_io_time;
    double total_time;
    double end_to_end_time;
    uint64_t total_bytes;
    uint64_t total_packets;
} PerformanceStats;

#endif /* KTAG_DEMO_REPORT_TYPES_H */
