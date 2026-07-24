/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_PIPELINE_RUNNER_H
#define KTAG_DEMO_PIPELINE_RUNNER_H

#include "pipeline_config.h"

enum class ktag_demo_pipeline_output_format {
    csv,
    json
};

int ktag_demo_run_pipeline(
    const ktag_demo_pipeline_config &config,
    const char *config_file,
    const char **pcap_files,
    int pcap_file_count,
    const char *interface_name,
    const char *output_file,
    ktag_demo_pipeline_output_format output_format);

#endif /* KTAG_DEMO_PIPELINE_RUNNER_H */
