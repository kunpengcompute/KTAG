/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_VIEW_INTERNAL_H
#define KTAG_FEATURE_VIEW_INTERNAL_H

#include "feature_extract.h"
#include "ktag_feature.h"

struct ktag_flow_view {
    const flow_hash_node *node = nullptr;
    uint64_t flow_id = 0;
    mutable bool stream_feature_ready = false;
    mutable stream_feature cached_stream_feature{};
};

struct ktag_packet_view {
    const uint8_t *data = nullptr;
    uint32_t length = 0;
    ktag_timestamp timestamp{};
};

ktag_status ktag_flow_view_get_stream_feature_internal(
    const ktag_flow_view *flow,
    const stream_feature **feature);

#endif /* KTAG_FEATURE_VIEW_INTERNAL_H */
