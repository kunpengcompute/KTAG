/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#ifndef KTAG_LEGACY_HTTP_H
#define KTAG_LEGACY_HTTP_H
#include <cstdint>
#include <string>
#include <string_view>
namespace ktag_traffic_features::legacy {
std::string http_url(const uint8_t *, uint32_t);
}
#endif
