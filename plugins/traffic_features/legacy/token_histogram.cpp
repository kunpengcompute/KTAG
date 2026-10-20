/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#include "token_histogram.h"
#include <unordered_set>
namespace ktag_traffic_features::legacy {
namespace {
constexpr uint32_t KEYWORD_WEIGHT = 20U;
const std::unordered_set<std::string_view> KEYWORDS{
    "iqiyi", "tencent", "deepseek", "qq",        "bilibili", "bilivideo",
    "video", "chat",    "zijie",    "bytedance", "kimi",     "mgtv",
    "hitv",  "baidu",   "yiyan",    "youku",     "alicdn",   "taobao",
    "iku",   "hunyuan", "yuanbao"};
void update_crc(char value, uint16_t *crc) {
    *crc ^= static_cast<uint16_t>(value) << 8U;
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
        *crc = (*crc & 0x8000U) != 0U
            ? static_cast<uint16_t>((*crc << 1U) ^ 0xA001U)
            : static_cast<uint16_t>(*crc << 1U);
    }
}
}
void add_crc_token(std::string_view token, float *histogram, uint32_t dimension) {
    if (!histogram || dimension == 0U) return;
    uint16_t crc = 0xFFFFU;
    for (char value : token) update_crc(value, &crc);
    crc = static_cast<uint16_t>((crc << 8U) | (crc >> 8U));
    histogram[crc % dimension] += KEYWORDS.count(token) ? KEYWORD_WEIGHT : 1.0F;
}
}
