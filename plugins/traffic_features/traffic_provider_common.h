/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_TRAFFIC_PROVIDER_COMMON_H
#define KTAG_TRAFFIC_PROVIDER_COMMON_H

#include "feature/traffic_contract.h"
#include "ktag_feature.h"

namespace ktag_traffic_features {

ktag_status register_provider(ktag_feature_registry *registry,
                              const provider_catalog &catalog,
                              const ktag_feature_callbacks &callbacks,
                              ktag_feature_callback_error_policy error_policy,
                              void *user_data);

ktag_status set_optional(ktag_feature_output *output,
                         const feature_definition &feature, const float *values,
                         uint32_t value_count);

ktag_status set_scalar(ktag_feature_output *output,
                       const feature_definition &feature, double value);

} // namespace ktag_traffic_features

#endif /* KTAG_TRAFFIC_PROVIDER_COMMON_H */
