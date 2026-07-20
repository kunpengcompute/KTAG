/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#ifndef KTAG_DEMO_UTILS_H
#define KTAG_DEMO_UTILS_H

#include "report_types.h"

double get_time_in_seconds();
double calculate_accuracy(const int *y_true, const int *y_pred, int count);
void get_unique_classes(const int *y_true, const int *y_pred, int count, int **classes, int *num_classes);
void calculate_class_metrics(const int *classes, int num_classes, const int *y_true, const int *y_pred, int count,
                             ClassificationReport *report);
void calculate_average_metrics(const ClassificationReport *report);
ClassificationReport *calculate_classification_report(const int *y_true, const int *y_pred, int count);
void free_classification_report(ClassificationReport *report);
int print_classification_report(const ClassificationReport *report);
int save_classification_report(const ClassificationReport *report, const char *output_file);
int print_and_save_classification_report(const ClassificationReport *report, const char *output_file);
void print_performance_stats(const PerformanceStats *stats);
void print_performance_stats_ex(const PerformanceStats *stats, int stages_overlap);

#endif /* KTAG_DEMO_UTILS_H */
