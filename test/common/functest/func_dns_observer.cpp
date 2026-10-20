/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#include <gtest/gtest.h>
#include "feature/dns_observer_internal.h"
#include <vector>
namespace {
struct observation { unsigned count = 0U; ktag_ip_address address{}; std::string domain; uint64_t expiry = 0U; };
ktag_status observe(void *user, const ktag_ip_address *address, const std::string &domain, uint64_t expiry) {
    auto &out = *static_cast<observation *>(user);
    ++out.count; out.address = *address; out.domain = domain; out.expiry = expiry;
    return KTAG_SUCCESS;
}
std::vector<uint8_t> response() {
    return {0,1,0x81,0x80,0,1,0,1,0,0,0,0,
            1,'a',0,0,1,0,1,
            0xc0,0x0c,0,1,0,1,0,0,0,2,0,4,192,0,2,1};
}
}
TEST(KTAGDnsObserver, CompressedAnswerKeepsAddressDomainAndTtl) {
    auto data = response(); observation out;
    ktag_timestamp timestamp{3, 4};
    ASSERT_EQ(KTAG_SUCCESS, ktag_dns_observe_internal(data.data(), data.size(), timestamp, observe, &out));
    ASSERT_EQ(1U, out.count);
    EXPECT_EQ(4U, out.address.version);
    EXPECT_EQ(192U, out.address.bytes[0]); EXPECT_EQ(1U, out.address.bytes[3]);
    EXPECT_EQ("a.", out.domain); EXPECT_EQ(5000004U, out.expiry);
}
TEST(KTAGDnsObserver, TruncatedAnswerAndCompressionLoopAreBounded) {
    auto data = response(); observation out;
    data.pop_back();
    ASSERT_EQ(KTAG_SUCCESS, ktag_dns_observe_internal(data.data(), data.size(), {1,0}, observe, &out));
    EXPECT_EQ(0U, out.count);
    data = response(); data[12] = 0xc0; data[13] = 12;
    ASSERT_EQ(KTAG_SUCCESS, ktag_dns_observe_internal(data.data(), data.size(), {1,0}, observe, &out));
    EXPECT_EQ(0U, out.count);
}
TEST(KTAGDnsObserver, CallbackFailureIsPropagated) {
    auto data = response();
    const auto fail = +[](void *, const ktag_ip_address *, const std::string &, uint64_t) -> ktag_status { return KTAG_ERROR_CAPACITY; };
    EXPECT_EQ(KTAG_ERROR_CAPACITY, ktag_dns_observe_internal(data.data(), data.size(), {1,0}, fail, nullptr));
}
