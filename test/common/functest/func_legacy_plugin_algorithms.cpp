/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#include <gtest/gtest.h>
#include "legacy/http.h"
#include "legacy/token_histogram.h"
#include <array>
#include <numeric>
#include <string>
namespace {
std::string url(const std::string &text) {
    return ktag_traffic_features::legacy::http_url(
        reinterpret_cast<const uint8_t *>(text.data()), static_cast<uint32_t>(text.size()));
}
}
TEST(KTAGLegacyPlugin, HttpKeepsAbsoluteUriAndHostRules) {
    EXPECT_EQ("http://example/path", url("GET http://example/path HTTP/1.1\r\nHost: other\r\n\r\n"));
    EXPECT_EQ("http://example/path", url("GET /path HTTP/1.1\r\nHost: example\r\n\r\n"));
    EXPECT_EQ("/path", url("GET /path HTTP/1.1\r\n\r\n"));
}
TEST(KTAGLegacyPlugin, IncompleteHostKeepsParsedUri) {
    EXPECT_EQ("/path", url("GET /path HTTP/1.1\r\nHost: example"));
    EXPECT_EQ("", url("GET /path HTTP/1.1"));
    EXPECT_EQ("", url("invalid"));
}
TEST(KTAGLegacyPlugin, RawbowPreservesKeywordWeightsAndEmptyTokenCrc) {
    std::array<float, 32> histogram{};
    ktag_traffic_features::legacy::add_crc_token("", histogram.data(), histogram.size());
    EXPECT_FLOAT_EQ(1.0F, histogram[31]);
    ktag_traffic_features::legacy::add_crc_token("bilibili", histogram.data(), histogram.size());
    EXPECT_FLOAT_EQ(21.0F, std::accumulate(histogram.begin(), histogram.end(), 0.0F));
    ktag_traffic_features::legacy::add_crc_token("BILIBILI", histogram.data(), histogram.size());
    EXPECT_FLOAT_EQ(22.0F, std::accumulate(histogram.begin(), histogram.end(), 0.0F));
}
