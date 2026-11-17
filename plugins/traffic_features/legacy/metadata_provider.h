/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_TRAFFIC_METADATA_PROVIDER_H
#define KTAG_TRAFFIC_METADATA_PROVIDER_H

#include "ktag_feature.h"

namespace ktag_traffic_features {

ktag_status register_metadata_provider(ktag_feature_registry *registry);

} // namespace ktag_traffic_features

#endif /* KTAG_TRAFFIC_METADATA_PROVIDER_H */
