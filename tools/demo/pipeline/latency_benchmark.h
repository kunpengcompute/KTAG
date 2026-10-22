/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#ifndef KTAG_DEMO_LATENCY_BENCHMARK_H
#define KTAG_DEMO_LATENCY_BENCHMARK_H
#include <csignal>
#include <functional>
#include "pipeline_config.h"
#include "ktag_feature.h"
#include "ktag_inference.h"

// Factories are shared with the normal pipeline: no duplicate model or feature implementation.
using ktag_demo_latency_feature_factory =
    std::function<ktag_feature_context *(ktag_domain_store *)>;
using ktag_demo_latency_inference_factory = std::function<ktag_inference_context *()>;
int ktag_demo_run_latency_benchmark(
    const ktag_demo_pipeline_config &config, const ktag_feature_schema *schema,
    const char **pcap_files, int pcap_file_count,
    const ktag_demo_latency_feature_factory &feature_factory,
    const ktag_demo_latency_inference_factory &inference_factory,
    const volatile sig_atomic_t *interrupted);
#endif
