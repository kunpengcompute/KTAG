/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_VIEW_INTERNAL_H
#define KTAG_FEATURE_VIEW_INTERNAL_H

#include "ktag_feature.h"

#include <string>

struct ktag_flow_direction_summary {
    uint64_t total_bytes = 0U;
    uint32_t packet_count = 0U;
    std::string domain;
};

struct ktag_flow_view {
    uint64_t flow_id = 0;
    ktag_flow_key flow_key{};
    ktag_ip_address forward_address{};
    ktag_flow_direction_summary directions[2];
    uint16_t forward_port = 0U;
    bool has_flow_key = false;
};

struct ktag_packet_view {
    const uint8_t *data = nullptr;
    uint32_t length = 0;
    const uint8_t *payload = nullptr;
    uint32_t payload_length = 0U;
    uint32_t captured_header_length = 0U;
    ktag_timestamp timestamp{};
    ktag_packet_transport_view transport{};
};

#endif /* KTAG_FEATURE_VIEW_INTERNAL_H */
