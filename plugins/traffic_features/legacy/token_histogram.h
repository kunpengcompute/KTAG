/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#ifndef KTAG_LEGACY_TOKEN_HISTOGRAM_H
#define KTAG_LEGACY_TOKEN_HISTOGRAM_H
#include <cstdint>
#include <string_view>
namespace ktag_traffic_features::legacy {
void add_crc_token(std::string_view, float *, uint32_t);
}
#endif
