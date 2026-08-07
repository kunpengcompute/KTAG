/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kdadk_test.h"
#include "ktag_capture.h"

namespace {

void write_be16(std::vector<uint8_t> *data, size_t offset, uint16_t value)
{
    (*data)[offset] = static_cast<uint8_t>(value >> 8U);
    (*data)[offset + 1U] = static_cast<uint8_t>(value & 0xffU);
}

std::vector<uint8_t> ipv4_frame(
    const std::array<uint8_t, 4> &source,
    uint16_t source_port,
    const std::array<uint8_t, 4> &destination,
    uint16_t destination_port,
    uint8_t protocol,
    bool vlan = false)
{
    const size_t network_offset = vlan ? 18U : 14U;
    std::vector<uint8_t> frame(network_offset + 24U, 0U);
    write_be16(&frame, 12U, vlan ? 0x8100U : 0x0800U);
    if (vlan) {
        write_be16(&frame, 14U, 1U);
        write_be16(&frame, 16U, 0x0800U);
    }
    frame[network_offset] = 0x45U;
    write_be16(&frame, network_offset + 2U, 24U);
    frame[network_offset + 8U] = 64U;
    frame[network_offset + 9U] = protocol;
    std::memcpy(frame.data() + network_offset + 12U, source.data(), source.size());
    std::memcpy(frame.data() + network_offset + 16U, destination.data(), destination.size());
    write_be16(&frame, network_offset + 20U, source_port);
    write_be16(&frame, network_offset + 22U, destination_port);
    return frame;
}

std::vector<uint8_t> ipv6_frame(
    const std::array<uint8_t, 16> &source,
    uint16_t source_port,
    const std::array<uint8_t, 16> &destination,
    uint16_t destination_port)
{
    constexpr size_t network_offset = 14U;
    std::vector<uint8_t> frame(network_offset + 44U, 0U);
    write_be16(&frame, 12U, 0x86ddU);
    frame[network_offset] = 0x60U;
    write_be16(&frame, network_offset + 4U, 4U);
    frame[network_offset + 6U] = 17U;
    frame[network_offset + 7U] = 64U;
    std::memcpy(frame.data() + network_offset + 8U, source.data(), source.size());
    std::memcpy(frame.data() + network_offset + 24U, destination.data(), destination.size());
    write_be16(&frame, network_offset + 40U, source_port);
    write_be16(&frame, network_offset + 42U, destination_port);
    return frame;
}

std::vector<uint8_t> ipv6_frame_with_extension_chain(
    const std::array<uint8_t, 16> &source,
    uint16_t source_port,
    const std::array<uint8_t, 16> &destination,
    uint16_t destination_port,
    uint32_t extension_count)
{
    constexpr size_t network_offset = 14U;
    const size_t extension_bytes =
        static_cast<size_t>(extension_count) * 8U;
    const size_t payload_length = extension_bytes + 4U;
    std::vector<uint8_t> frame(
        network_offset + 40U + payload_length, 0U);
    write_be16(&frame, 12U, 0x86ddU);
    frame[network_offset] = 0x60U;
    write_be16(
        &frame,
        network_offset + 4U,
        static_cast<uint16_t>(payload_length));
    frame[network_offset + 6U] =
        extension_count == 0U ? 17U : 60U;
    frame[network_offset + 7U] = 64U;
    std::memcpy(
        frame.data() + network_offset + 8U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + network_offset + 24U,
        destination.data(),
        destination.size());
    size_t offset = network_offset + 40U;
    for (uint32_t index = 0U; index < extension_count; ++index) {
        frame[offset] =
            index + 1U == extension_count ? 17U : 60U;
        frame[offset + 1U] = 0U;
        offset += 8U;
    }
    write_be16(&frame, offset, source_port);
    write_be16(&frame, offset + 2U, destination_port);
    return frame;
}

std::vector<uint8_t> ipv6_fragment_frame(
    const std::array<uint8_t, 16> &source,
    uint16_t source_port,
    const std::array<uint8_t, 16> &destination,
    uint16_t destination_port,
    uint16_t fragment_field)
{
    constexpr size_t network_offset = 14U;
    constexpr size_t fragment_length = 8U;
    constexpr size_t port_length = 4U;
    std::vector<uint8_t> frame(
        network_offset + 40U + fragment_length + port_length, 0U);
    write_be16(&frame, 12U, 0x86ddU);
    frame[network_offset] = 0x60U;
    write_be16(
        &frame,
        network_offset + 4U,
        fragment_length + port_length);
    frame[network_offset + 6U] = 44U;
    frame[network_offset + 7U] = 64U;
    std::memcpy(
        frame.data() + network_offset + 8U,
        source.data(),
        source.size());
    std::memcpy(
        frame.data() + network_offset + 24U,
        destination.data(),
        destination.size());
    const size_t fragment = network_offset + 40U;
    frame[fragment] = 17U;
    write_be16(&frame, fragment + 2U, fragment_field);
    write_be16(
        &frame, fragment + fragment_length, source_port);
    write_be16(
        &frame,
        fragment + fragment_length + 2U,
        destination_port);
    return frame;
}

ktag_packet packet_view(const std::vector<uint8_t> &data, uint32_t link_type)
{
    ktag_packet packet{};
    packet.struct_size = sizeof(packet);
    packet.api_version = KTAG_API_VERSION;
    packet.data = data.data();
    packet.captured_length = static_cast<uint32_t>(data.size());
    packet.original_length = packet.captured_length;
    packet.link_type = link_type;
    return packet;
}

void expect_same_key(const ktag_flow_key &first, const ktag_flow_key &second)
{
    EXPECT_EQ(first.address_a.version, second.address_a.version);
    EXPECT_EQ(first.address_b.version, second.address_b.version);
    EXPECT_EQ(first.protocol, second.protocol);
    EXPECT_EQ(first.port_a, second.port_a);
    EXPECT_EQ(first.port_b, second.port_b);
    EXPECT_EQ(0, std::memcmp(
                     first.address_a.bytes, second.address_a.bytes,
                     KTAG_IPV6_ADDRESS_SIZE));
    EXPECT_EQ(0, std::memcmp(
                     first.address_b.bytes, second.address_b.bytes,
                     KTAG_IPV6_ADDRESS_SIZE));
}

ktag_status next_non_timeout(
    ktag_capture_context *capture,
    ktag_packet *packet)
{
    for (uint32_t attempt = 0; attempt < 100U; ++attempt) {
        const ktag_status status = ktag_capture_next(capture, packet);
        if (status != KTAG_STATUS_TIMEOUT) {
            return status;
        }
    }
    return KTAG_STATUS_TIMEOUT;
}

} // namespace

TEST(KTAGCaptureFlowKey, ReverseIPv4TcpPacketsHaveSameKeyHashAndWorker)
{
    const std::array<uint8_t, 4> low = {10U, 0U, 0U, 1U};
    const std::array<uint8_t, 4> high = {10U, 0U, 0U, 2U};
    const auto forward = ipv4_frame(high, 443U, low, 50000U, 6U);
    const auto reverse = ipv4_frame(low, 50000U, high, 443U, 6U);
    const auto forward_packet = packet_view(forward, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    const auto reverse_packet = packet_view(reverse, KTAG_CAPTURE_LINKTYPE_ETHERNET);

    ktag_flow_key forward_key{};
    ktag_flow_key reverse_key{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&forward_packet, &forward_key));
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&reverse_packet, &reverse_key));
    expect_same_key(forward_key, reverse_key);
    EXPECT_EQ(4U, forward_key.address_a.version);
    EXPECT_EQ(50000U, forward_key.port_a);
    EXPECT_EQ(443U, forward_key.port_b);
    EXPECT_EQ(0, std::memcmp(forward_key.address_a.bytes, low.data(), low.size()));
    EXPECT_EQ(0, std::memcmp(forward_key.address_b.bytes, high.data(), high.size()));

    constexpr uint64_t seed = 0x0123456789abcdefULL;
    EXPECT_EQ(ktag_flow_hash(&forward_key, seed), ktag_flow_hash(&reverse_key, seed));
    EXPECT_EQ(0x3df2eddef15ed829ULL, ktag_flow_hash(&forward_key, seed));
    for (uint32_t workers = 1U; workers <= 64U; ++workers) {
        EXPECT_EQ(
            ktag_flow_select_worker(&forward_key, workers, seed),
            ktag_flow_select_worker(&reverse_key, workers, seed));
    }
    EXPECT_EQ(
        KTAG_WORKER_INDEX_INVALID,
        ktag_flow_select_worker(&forward_key, 0U, seed));
}

TEST(KTAGCaptureFlowKey, SupportedLinkTypesProduceTheSameFlowKey)
{
    const std::array<uint8_t, 4> source = {198U, 51U, 100U, 20U};
    const std::array<uint8_t, 4> destination = {203U, 0U, 113U, 10U};
    const auto ethernet = ipv4_frame(source, 12345U, destination, 443U, 6U);

    std::vector<uint8_t> raw(ethernet.begin() + 14U, ethernet.end());
    std::vector<uint8_t> sll(16U + raw.size(), 0U);
    write_be16(&sll, 14U, 0x0800U);
    std::memcpy(sll.data() + 16U, raw.data(), raw.size());
    std::vector<uint8_t> sll2(20U + raw.size(), 0U);
    write_be16(&sll2, 0U, 0x0800U);
    std::memcpy(sll2.data() + 20U, raw.data(), raw.size());
    std::vector<uint8_t> qinq(22U + raw.size(), 0U);
    write_be16(&qinq, 12U, 0x88a8U);
    write_be16(&qinq, 14U, 1U);
    write_be16(&qinq, 16U, 0x8100U);
    write_be16(&qinq, 18U, 2U);
    write_be16(&qinq, 20U, 0x0800U);
    std::memcpy(qinq.data() + 22U, raw.data(), raw.size());

    const auto ethernet_packet = packet_view(
        ethernet, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    const auto raw_packet = packet_view(raw, KTAG_CAPTURE_LINKTYPE_RAW);
    const auto sll_packet = packet_view(sll, KTAG_CAPTURE_LINKTYPE_LINUX_SLL);
    const auto sll2_packet = packet_view(sll2, KTAG_CAPTURE_LINKTYPE_LINUX_SLL2);
    const auto qinq_packet = packet_view(
        qinq, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    ktag_flow_key expected{};
    ktag_flow_key actual{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&ethernet_packet, &expected));
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&raw_packet, &actual));
    expect_same_key(expected, actual);
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&sll_packet, &actual));
    expect_same_key(expected, actual);
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&sll2_packet, &actual));
    expect_same_key(expected, actual);
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&qinq_packet, &actual));
    expect_same_key(expected, actual);
}

TEST(KTAGCaptureFlowKey, SameAddressUsesPortForCanonicalEndpointOrder)
{
    const std::array<uint8_t, 4> address = {127U, 0U, 0U, 1U};
    const auto forward = ipv4_frame(address, 60000U, address, 443U, 17U, true);
    const auto reverse = ipv4_frame(address, 443U, address, 60000U, 17U, true);
    const auto forward_packet = packet_view(forward, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    const auto reverse_packet = packet_view(reverse, KTAG_CAPTURE_LINKTYPE_ETHERNET);

    ktag_flow_key forward_key{};
    ktag_flow_key reverse_key{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&forward_packet, &forward_key));
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&reverse_packet, &reverse_key));
    expect_same_key(forward_key, reverse_key);
    EXPECT_EQ(443U, forward_key.port_a);
    EXPECT_EQ(60000U, forward_key.port_b);
}

TEST(KTAGCaptureFlowKey, ReverseIPv6UdpPacketsHaveSameWorker)
{
    std::array<uint8_t, 16> first{};
    std::array<uint8_t, 16> second{};
    first[0] = 0x20U;
    first[1] = 0x01U;
    first[15] = 1U;
    second[0] = 0x20U;
    second[1] = 0x01U;
    second[15] = 2U;
    const auto forward = ipv6_frame(second, 53U, first, 40000U);
    const auto reverse = ipv6_frame(first, 40000U, second, 53U);
    const auto forward_packet = packet_view(forward, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    const auto reverse_packet = packet_view(reverse, KTAG_CAPTURE_LINKTYPE_ETHERNET);

    ktag_flow_key forward_key{};
    ktag_flow_key reverse_key{};
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&forward_packet, &forward_key));
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_get_flow_key(&reverse_packet, &reverse_key));
    expect_same_key(forward_key, reverse_key);
    EXPECT_EQ(6U, forward_key.address_a.version);
    EXPECT_EQ(
        ktag_flow_select_worker(&forward_key, 17U, 99U),
        ktag_flow_select_worker(&reverse_key, 17U, 99U));
}

TEST(KTAGCaptureFlowKey, IPv6ExtensionChainKeepsTheSameFlowKey)
{
    std::array<uint8_t, 16> first{};
    std::array<uint8_t, 16> second{};
    first[0] = 0x20U;
    first[1] = 0x01U;
    first[15] = 1U;
    second[0] = 0x20U;
    second[1] = 0x01U;
    second[15] = 2U;
    const auto direct = ipv6_frame(first, 41000U, second, 443U);
    const auto extended = ipv6_frame_with_extension_chain(
        first, 41000U, second, 443U, 3U);
    const auto direct_packet = packet_view(
        direct, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    const auto extended_packet = packet_view(
        extended, KTAG_CAPTURE_LINKTYPE_ETHERNET);

    ktag_flow_key expected{};
    ktag_flow_key actual{};
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_packet_get_flow_key(&direct_packet, &expected));
    ASSERT_EQ(
        KTAG_SUCCESS,
        ktag_packet_get_flow_key(&extended_packet, &actual));
    expect_same_key(expected, actual);
}

TEST(KTAGCaptureFlowKey, IPv6FragmentAndExtensionLimitsAreBounded)
{
    std::array<uint8_t, 16> first{};
    std::array<uint8_t, 16> second{};
    first[0] = 0x20U;
    first[1] = 0x01U;
    first[15] = 1U;
    second[0] = 0x20U;
    second[1] = 0x01U;
    second[15] = 2U;

    const auto atomic = ipv6_fragment_frame(
        first, 41000U, second, 443U, 0U);
    const auto non_initial = ipv6_fragment_frame(
        first, 41000U, second, 443U, 8U);
    const auto too_many = ipv6_frame_with_extension_chain(
        first, 41000U, second, 443U, 9U);
    auto jumbogram = ipv6_frame(first, 41000U, second, 443U);
    write_be16(&jumbogram, 14U + 4U, 0U);

    ktag_flow_key key{};
    auto packet = packet_view(
        atomic, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    EXPECT_EQ(
        KTAG_SUCCESS,
        ktag_packet_get_flow_key(&packet, &key));
    packet = packet_view(
        non_initial, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    EXPECT_EQ(
        KTAG_ERROR_NOT_FOUND,
        ktag_packet_get_flow_key(&packet, &key));
    packet = packet_view(
        too_many, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    EXPECT_EQ(
        KTAG_ERROR_NOT_SUPPORTED,
        ktag_packet_get_flow_key(&packet, &key));
    packet = packet_view(
        jumbogram, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    EXPECT_EQ(
        KTAG_ERROR_NOT_SUPPORTED,
        ktag_packet_get_flow_key(&packet, &key));
}

TEST(KTAGCaptureFlowKey, NonInitialIPv4FragmentUsesFallbackPath)
{
    const std::array<uint8_t, 4> first = {192U, 0U, 2U, 1U};
    const std::array<uint8_t, 4> second = {192U, 0U, 2U, 2U};
    auto frame = ipv4_frame(first, 1U, second, 2U, 6U);
    /* IPv4 fragment offset is non-zero. */
    write_be16(&frame, 14U + 6U, 1U);
    const auto packet = packet_view(frame, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    ktag_flow_key key{};
    EXPECT_EQ(KTAG_ERROR_NOT_FOUND, ktag_packet_get_flow_key(&packet, &key));
}

TEST(KTAGCaptureFlowKey, InvalidNetworkLengthDoesNotReadEthernetPaddingAsPorts)
{
    const std::array<uint8_t, 4> first = {192U, 0U, 2U, 1U};
    const std::array<uint8_t, 4> second = {192U, 0U, 2U, 2U};
    auto frame = ipv4_frame(first, 1U, second, 2U, 6U);
    write_be16(&frame, 14U + 2U, 20U);
    const auto packet = packet_view(frame, KTAG_CAPTURE_LINKTYPE_ETHERNET);
    ktag_flow_key key{};
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_packet_get_flow_key(&packet, &key));
}

TEST(KTAGCapturePacketBuffer, BorrowedPacketCanBecomeReferenceCountedOwnedCopy)
{
    std::vector<uint8_t> bytes = {1U, 2U, 3U, 4U};
    ktag_packet packet = packet_view(bytes, KTAG_CAPTURE_LINKTYPE_RAW);
    EXPECT_EQ(0U, ktag_packet_is_owned(&packet));
    EXPECT_EQ(KTAG_ERROR_INVALID_STATE, ktag_packet_retain(&packet));
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_make_owned(&packet));
    ASSERT_EQ(1U, ktag_packet_is_owned(&packet));
    bytes[0] = 99U;
    EXPECT_EQ(1U, packet.data[0]);

    ktag_packet duplicate = packet;
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_retain(&duplicate));
    ktag_packet_release(&packet);
    EXPECT_EQ(0U, ktag_packet_is_owned(&packet));
    EXPECT_EQ(1U, duplicate.data[0]);
    ktag_packet_release(&duplicate);
    EXPECT_EQ(0U, ktag_packet_is_owned(&duplicate));
}

TEST(KTAGCaptureDevices, IPv6AccessorsRejectInvalidViews)
{
    EXPECT_EQ(
        0U,
        ktag_capture_device_list_get_ipv6_count(nullptr, 0U));
    EXPECT_EQ(
        nullptr,
        ktag_capture_device_list_get_ipv6(nullptr, 0U, 0U));
}

TEST(KTAGCaptureSource, OfflineBorrowedAndOwnedModesPreservePacketContract)
{
    const char *configured_trace = std::getenv("KTAG_CAPTURE_TEST_PCAP");
    const std::string trace = configured_trace != nullptr && configured_trace[0] != '\0' ?
        configured_trace : GetFirstPcapFile();
    if (trace.empty()) {
        SUCCEED();
        return;
    }

    ktag_capture_config borrowed{};
    borrowed.struct_size = sizeof(borrowed);
    borrowed.api_version = KTAG_API_VERSION;
    borrowed.source_type = KTAG_CAPTURE_SOURCE_PCAP_FILE;
    borrowed.source = trace.c_str();
    borrowed.packet_mode = KTAG_CAPTURE_PACKET_BORROWED;
    ktag_capture_context *capture = ktag_capture_open(&borrowed);
    ASSERT_NE(nullptr, capture) << ktag_capture_get_last_error();
    EXPECT_NE(0U, ktag_capture_get_link_type(capture));
    ktag_packet packet{};
    EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_capture_next(capture, &packet));
    ktag_packet_init(&packet);
    ASSERT_EQ(KTAG_SUCCESS, next_non_timeout(capture, &packet));
    EXPECT_EQ(1U, packet.sequence);
    EXPECT_EQ(0U, ktag_packet_is_owned(&packet));
    EXPECT_LE(packet.captured_length, packet.original_length);
    ASSERT_EQ(KTAG_SUCCESS, ktag_packet_make_owned(&packet));
    const uint8_t first_byte = packet.captured_length == 0U ? 0U : packet.data[0];
    ktag_capture_close(capture);
    if (packet.captured_length != 0U) {
        EXPECT_EQ(first_byte, packet.data[0]);
    }
    ktag_packet_release(&packet);

    ktag_capture_config owned = borrowed;
    owned.packet_mode = KTAG_CAPTURE_PACKET_OWNED;
    owned.packet_buffer_capacity = 1U;
    capture = ktag_capture_open(&owned);
    ASSERT_NE(nullptr, capture) << ktag_capture_get_last_error();
    ASSERT_EQ(KTAG_SUCCESS, next_non_timeout(capture, &packet));
    EXPECT_EQ(1U, ktag_packet_is_owned(&packet));
    ktag_packet another{};
    ktag_packet_init(&another);
    EXPECT_EQ(KTAG_ERROR_CAPACITY, ktag_capture_next(capture, &another));
    ktag_packet_release(&packet);
    ASSERT_EQ(KTAG_SUCCESS, next_non_timeout(capture, &another));
    EXPECT_EQ(2U, another.sequence);
    const uint32_t packet_length = another.captured_length;
    ktag_capture_close(capture);
    EXPECT_EQ(packet_length, another.captured_length);
    ktag_packet_release(&another);
}
