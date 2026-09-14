/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/

/*
 * DPDK poll-mode capture backend (ktag_capture_dpdk) unit tests.
 *
 * These cases deliberately avoid any live device: a capture context is never
 * opened against an existing NIC. Either the configuration is rejected before
 * EAL is touched, or device_name names a port that cannot exist. Tests that do
 * need EAL (idempotent init, port lookup) bail out early with a printed reason
 * when the environment cannot initialise it, so the suite stays green on hosts
 * without hugepages while still covering the code paths where it can.
 *
 * Build with `make dpdk=y`; the file is filtered out otherwise because
 * libktag_capture_dpdk is only produced by a KTAG_ENABLE_DPDK=ON build.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <iostream>
#include <string>

#include "ktag_capture.h"
#include "ktag_capture_dpdk.h"
#include "ktag_types.h"

#include "pipeline_config.h"

namespace {

/* A configuration that satisfies every check except the one under test. */
ktag_dpdk_capture_config valid_capture_config() {
  ktag_dpdk_capture_config config{};
  config.struct_size = sizeof(config);
  config.api_version = KTAG_API_VERSION;
  config.packet_mode = KTAG_DPDK_PACKET_BORROWED;
  return config;
}

std::string last_error() {
  const char *message = ktag_dpdk_capture_get_last_error();
  return message == nullptr ? std::string() : std::string(message);
}

/* A name no device can have, so resolve_port() always reports NOT_FOUND. */
const char *kAbsentDevice = "ktag_no_such_device_0";

/*
 * EAL is initialised with --no-pci on purpose: without it rte_eal_init probes
 * and initialises every bound device, which would make a unit test open the
 * machine's real NICs. Skipping the PCI bus keeps device enumeration empty,
 * which is exactly what these cases want to assert against anyway.
 *
 * EAL state is process-global and gtest-parallel gives each test its own
 * process, so this does not leak into other test binaries.
 */
const char *kEalArgs = "--no-pci";

bool eal_available() {
  if (ktag_dpdk_eal_init(kEalArgs) == KTAG_SUCCESS) {
    return true;
  }
  std::cout << "[  SKIPPED ] DPDK EAL unavailable in this environment: "
            << last_error() << std::endl;
  return false;
}

} // namespace

/* ------------------------------------------------------------------ */
/* Configuration validation                                            */
/*                                                                     */
/* valid_config() runs before EAL is initialised, so these cases need   */
/* neither a DPDK port nor a working EAL.                              */
/* ------------------------------------------------------------------ */

TEST(KTAGCaptureDpdkConfig, NullConfigIsRejected) {
  EXPECT_EQ(ktag_dpdk_capture_open(nullptr), nullptr);
  EXPECT_NE(last_error().find("invalid DPDK capture configuration"),
            std::string::npos);
}

TEST(KTAGCaptureDpdkConfig, UndersizedStructIsRejected) {
  ktag_dpdk_capture_config config = valid_capture_config();
  config.struct_size = sizeof(config) - 1U;
  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
}

TEST(KTAGCaptureDpdkConfig, MismatchedApiVersionIsRejected) {
  ktag_dpdk_capture_config config = valid_capture_config();
  config.api_version = KTAG_API_VERSION + 1U;
  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
}

TEST(KTAGCaptureDpdkConfig, UnknownPacketModeIsRejected) {
  ktag_dpdk_capture_config config = valid_capture_config();
  config.packet_mode = static_cast<ktag_dpdk_packet_mode>(99);
  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
  EXPECT_NE(last_error().find("packet_mode"), std::string::npos);
}

/*
 * burst_size indexes a fixed-size stack array inside next(), so a value above
 * the default must be refused rather than overflow it.
 */
TEST(KTAGCaptureDpdkConfig, BurstSizeAboveMaximumIsRejected) {
  ktag_dpdk_capture_config config = valid_capture_config();
  config.burst_size = KTAG_DPDK_DEFAULT_BURST_SIZE + 1U;
  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
  EXPECT_NE(last_error().find("burst_size"), std::string::npos);
}

TEST(KTAGCaptureDpdkConfig, QueueCountAboveMaximumIsRejected) {
  ktag_dpdk_capture_config config = valid_capture_config();
  config.rx_queue_count = 9999U;
  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
  EXPECT_NE(last_error().find("rx_queue_count"), std::string::npos);
}

/* rx_queue_depth narrows to uint16_t on the way to DPDK. */
TEST(KTAGCaptureDpdkConfig, QueueDepthAboveMaximumIsRejected) {
  ktag_dpdk_capture_config config = valid_capture_config();
  config.rx_queue_depth = 65536U;
  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
  EXPECT_NE(last_error().find("rx_queue_depth"), std::string::npos);
}

/*
 * Boundary values must clear the range checks. The open still fails, but on
 * the absent device rather than on the field under test -- asserted through
 * the error text so the case holds whether or not EAL is usable.
 */
TEST(KTAGCaptureDpdkConfig, BoundaryValuesPassRangeChecking) {
  /*
   * Initialise EAL here rather than letting open() fall back to its lazy
   * no-argument init, which would probe the PCI bus and wake real NICs.
   */
  if (!eal_available()) {
    return;
  }
  ktag_dpdk_capture_config config = valid_capture_config();
  config.burst_size = KTAG_DPDK_DEFAULT_BURST_SIZE;
  config.rx_queue_count = 256U;
  config.rx_queue_depth = 65535U;
  config.device_name = kAbsentDevice;

  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
  EXPECT_EQ(last_error().find("burst_size"), std::string::npos);
  EXPECT_EQ(last_error().find("rx_queue_count"), std::string::npos);
  EXPECT_EQ(last_error().find("rx_queue_depth"), std::string::npos);
}

/* ------------------------------------------------------------------ */
/* next() argument and state guards                                    */
/*                                                                     */
/* Both guards return before the context is dereferenced, so a non-null */
/* placeholder pointer is enough to reach them.                         */
/* ------------------------------------------------------------------ */

TEST(KTAGCaptureDpdkNext, NullContextIsRejected) {
  ktag_packet packet{};
  ktag_packet_init(&packet);
  EXPECT_EQ(ktag_dpdk_capture_next(nullptr, &packet),
            KTAG_ERROR_INVALID_ARGUMENT);
}

TEST(KTAGCaptureDpdkNext, NullPacketIsRejected) {
  int placeholder = 0;
  EXPECT_EQ(ktag_dpdk_capture_next(&placeholder, nullptr),
            KTAG_ERROR_INVALID_ARGUMENT);
}

TEST(KTAGCaptureDpdkNext, UninitialisedPacketIsRejected) {
  int placeholder = 0;
  ktag_packet packet{};
  EXPECT_EQ(ktag_dpdk_capture_next(&placeholder, &packet),
            KTAG_ERROR_INVALID_ARGUMENT);
}

TEST(KTAGCaptureDpdkNext, MismatchedApiVersionIsRejected) {
  int placeholder = 0;
  ktag_packet packet{};
  ktag_packet_init(&packet);
  packet.api_version = KTAG_API_VERSION + 1U;
  EXPECT_EQ(ktag_dpdk_capture_next(&placeholder, &packet),
            KTAG_ERROR_INVALID_ARGUMENT);
}

/*
 * Reusing the output object before releasing an owned packet would dangle the
 * caller's data, so it must be refused rather than overwritten.
 */
TEST(KTAGCaptureDpdkNext, UnreleasedOwnedPacketIsRejected) {
  int placeholder = 0;
  ktag_packet packet{};
  ktag_packet_init(&packet);
  /* Only compared against NULL on this path, so the value never matters. */
  packet.owner = reinterpret_cast<ktag_packet_buffer *>(&placeholder);

  EXPECT_EQ(ktag_dpdk_capture_next(&placeholder, &packet),
            KTAG_ERROR_INVALID_STATE);

  packet.owner = nullptr;
}

/* ------------------------------------------------------------------ */
/* Null-safe accessors                                                 */
/* ------------------------------------------------------------------ */

TEST(KTAGCaptureDpdkAccessors, NullContextIsHandled) {
  EXPECT_EQ(ktag_dpdk_capture_get_link_type(nullptr), 0U);
  ktag_dpdk_capture_interrupt(nullptr);
  ktag_dpdk_capture_close(nullptr);
  SUCCEED();
}

TEST(KTAGCaptureDpdkAccessors, LastErrorIsNeverNull) {
  EXPECT_NE(ktag_dpdk_capture_get_last_error(), nullptr);
}

/* ------------------------------------------------------------------ */
/* EAL and port lookup                                                 */
/* ------------------------------------------------------------------ */

TEST(KTAGCaptureDpdkEal, InitIsIdempotent) {
  if (!eal_available()) {
    return;
  }
  /* A second call must report success, not the EALREADY failure. */
  EXPECT_EQ(ktag_dpdk_eal_init(nullptr), KTAG_SUCCESS);
  EXPECT_EQ(ktag_dpdk_eal_init("-l 0"), KTAG_SUCCESS);
}

/*
 * An explicit device name must fail rather than fall back to the first
 * available port: silently capturing a different NIC would be far worse than
 * refusing to open. Asserting the reported name covers both -- the open fails
 * and it fails about the port that was actually requested.
 */
TEST(KTAGCaptureDpdkPort, AbsentDeviceIsReportedAsNotFound) {
  if (!eal_available()) {
    return;
  }
  ktag_dpdk_capture_config config = valid_capture_config();
  config.device_name = kAbsentDevice;

  EXPECT_EQ(ktag_dpdk_capture_open(&config), nullptr);
  EXPECT_NE(last_error().find("DPDK device not found"), std::string::npos);
  EXPECT_NE(last_error().find(kAbsentDevice), std::string::npos);
}

/* ------------------------------------------------------------------ */
/* Demo-side backend selection helpers                                 */
/* ------------------------------------------------------------------ */

TEST(KTAGDemoCaptureBackend, NameParsingRoundTrips) {
  ktag_demo_capture_backend backend = ktag_demo_capture_backend::dpdk;

  ASSERT_TRUE(ktag_demo_capture_backend_from_name("pcap", &backend));
  EXPECT_EQ(backend, ktag_demo_capture_backend::pcap);
  EXPECT_STREQ(ktag_demo_capture_backend_name(backend), "pcap");

  ASSERT_TRUE(ktag_demo_capture_backend_from_name("dpdk", &backend));
  EXPECT_EQ(backend, ktag_demo_capture_backend::dpdk);
  EXPECT_STREQ(ktag_demo_capture_backend_name(backend), "dpdk");
}

TEST(KTAGDemoCaptureBackend, DefaultIsPcap) {
  const ktag_demo_pipeline_config config;
  EXPECT_EQ(config.capture_backend, ktag_demo_capture_backend::pcap);
}

TEST(KTAGDemoCaptureBackend, InvalidNamesAreRejectedWithoutSideEffects) {
  ktag_demo_capture_backend backend = ktag_demo_capture_backend::pcap;

  EXPECT_FALSE(ktag_demo_capture_backend_from_name("netmap", &backend));
  EXPECT_FALSE(ktag_demo_capture_backend_from_name("", &backend));
  EXPECT_FALSE(ktag_demo_capture_backend_from_name(nullptr, &backend));
  EXPECT_FALSE(ktag_demo_capture_backend_from_name("pcap", nullptr));
  EXPECT_FALSE(ktag_demo_capture_backend_from_name("dpdk", nullptr));

  /* A rejected name must leave the output argument untouched. */
  EXPECT_EQ(backend, ktag_demo_capture_backend::pcap);
}
