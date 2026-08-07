/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_PIPELINE_RUNNER_H
#define KTAG_DEMO_PIPELINE_RUNNER_H

#include <cstdint>
#include <string>
#include <vector>

#include "ktag_feature.h"
#include "ktag_inference.h"

#include "pipeline_config.h"

enum class ktag_demo_pipeline_output_format {
    csv,
    json
};

/*
 * Demo-owned prediction view.  Feature Record details retain the original
 * first-packet source/destination orientation; Prediction metadata retains
 * the public inference result returned for the same record.
 */
struct ktag_demo_pipeline_prediction {
    uint32_t source_index = 0U;
    ktag_feature_record_details details{};
    ktag_prediction_record record{};
};

bool ktag_demo_write_pipeline_predictions(
    const char *path,
    ktag_demo_pipeline_output_format format,
    const std::vector<ktag_demo_pipeline_prediction> &predictions,
    std::string *error);

int ktag_demo_run_pipeline(
    const ktag_demo_pipeline_config &config,
    const char *config_file,
    const char **pcap_files,
    int pcap_file_count,
    const char *interface_name,
    const char *output_file,
    ktag_demo_pipeline_output_format output_format);

#endif /* KTAG_DEMO_PIPELINE_RUNNER_H */
