/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROXY_TCP_SIGNATURE_PROVIDER_H
#define KTAG_PROXY_TCP_SIGNATURE_PROVIDER_H

#include "ktag_feature.h"

namespace ktag_proxy_features {

ktag_status register_tcp_signature_provider(ktag_feature_registry *registry);

} // namespace ktag_proxy_features

#endif /* KTAG_PROXY_TCP_SIGNATURE_PROVIDER_H */
