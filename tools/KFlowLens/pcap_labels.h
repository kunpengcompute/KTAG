/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#ifndef KTAG_DEMO_PCAP_LABELS_H
#define KTAG_DEMO_PCAP_LABELS_H

#include <cstdint>
#include <string>
#include <vector>

struct ktag_demo_pcap_labels {
    bool explicit_labels = false;
    std::vector<int> source_labels;
    std::vector<int> report_labels;
    std::vector<int> model_labels;
    std::string description;

    int label_for(uint32_t source_index) const;
    void validate_prediction(int64_t label) const;
};

std::string ktag_demo_model_package_path(const char *config_path);
ktag_demo_pcap_labels ktag_demo_load_pcap_labels(
    const std::string &model_package_path, const char *label_list,
    const char **pcap_files, int pcap_count);

#endif
