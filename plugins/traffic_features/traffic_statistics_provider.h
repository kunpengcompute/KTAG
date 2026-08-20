/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_TRAFFIC_STATISTICS_PROVIDER_H
#define KTAG_TRAFFIC_STATISTICS_PROVIDER_H

#include "ktag_feature.h"

namespace ktag_traffic_features {

ktag_status register_send_counters_provider(ktag_feature_registry *registry);
ktag_status register_send_header_provider(ktag_feature_registry *registry);
ktag_status register_send_payload_provider(ktag_feature_registry *registry);
ktag_status register_send_interval_provider(ktag_feature_registry *registry);
ktag_status register_receive_counters_provider(
    ktag_feature_registry *registry);
ktag_status register_receive_header_provider(ktag_feature_registry *registry);
ktag_status register_receive_payload_provider(
    ktag_feature_registry *registry);
ktag_status register_receive_interval_provider(
    ktag_feature_registry *registry);

} // namespace ktag_traffic_features

#endif /* KTAG_TRAFFIC_STATISTICS_PROVIDER_H */
