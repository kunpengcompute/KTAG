/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROXY_PROVIDER_COMMON_H
#define KTAG_PROXY_PROVIDER_COMMON_H

#include "ktag_feature.h"
#include "proxy_feature_catalog.h"

namespace ktag_proxy_features {

ktag_status register_provider(
    ktag_feature_registry *registry,
    const provider_catalog &catalog,
    const ktag_feature_callbacks &callbacks,
    ktag_feature_callback_error_policy error_policy,
    void *user_data);

ktag_status register_zero_provider(
    ktag_feature_registry *registry,
    const provider_catalog &catalog);

} // namespace ktag_proxy_features

#endif /* KTAG_PROXY_PROVIDER_COMMON_H */
