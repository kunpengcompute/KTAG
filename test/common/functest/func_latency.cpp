/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include <gtest/gtest.h>
#include "latency_benchmark_internal.h"
#include "bounded_queue.h"
#include <future>
using namespace ktag_demo_latency;
namespace {
ktag_feature_record_metadata metadata(uint8_t address, uint64_t first, uint64_t last) {
  ktag_feature_record_metadata result{};
  result.flow_key.address_a.version = result.flow_key.address_b.version = 4;
  result.flow_key.address_a.bytes[0] = address;
  result.flow_key.protocol = 6;
  result.first_packet_sequence = first; result.last_packet_sequence = last;
  return result;
}
}
TEST(KTAGLatency, AssociatesInterleavedPacketsWithoutCrossingFlowGenerations) {
  packet_history history;
  const auto first = metadata(1, 0, 3), next = metadata(1, 5, 7), other = metadata(2, 1, 6);
  for (uint64_t i : {0U, 3U, 5U, 7U}) history.add(key_text(first.flow_key), {i, 100 + i});
  for (uint64_t i : {1U, 6U}) history.add(key_text(other.flow_key), {i, 100 + i});
  auto packets = history.take(first);
  ASSERT_EQ(2U, packets.size()); EXPECT_EQ(0U, packets[0].sequence); EXPECT_EQ(3U, packets[1].sequence);
  packets = history.take(next); ASSERT_EQ(2U, packets.size()); EXPECT_EQ(5U, packets[0].sequence);
  EXPECT_EQ(2U, history.take(other).size()); EXPECT_TRUE(history.take(first).empty());
}
TEST(KTAGLatency, RetiredUnmatchedPacketsDoNotBecomeANewFlow) {
  packet_history history; auto item = metadata(1, 4, 5);
  for (uint64_t i = 0; i <= 6; ++i) history.add(key_text(item.flow_key), {i, i});
  auto packets = history.take(item); ASSERT_EQ(2U, packets.size()); EXPECT_EQ(4U, packets[0].sequence);
  item.first_packet_sequence = item.last_packet_sequence = 6;
  EXPECT_EQ(1U, history.take(item).size());
}
TEST(KTAGLatency, IgnoresStructurePaddingButDistinguishesIPv6Endpoints) {
  auto a = metadata(1, 0, 0).flow_key, b = a;
  b.reserved[0] = 42;
  EXPECT_EQ(key_text(a), key_text(b));
  a.address_a.version = b.address_a.version = 6;
  b.address_a.bytes[15] = 1; EXPECT_NE(key_text(a), key_text(b));
}
TEST(KTAGLatency, ComputesPacketWeightedPercentilesRatherThanMeanOfFlowMeans) {
  distribution d; d.add(10, 99); d.add(1000);
  auto r = d.summarize(); EXPECT_EQ(100U, r.count); EXPECT_DOUBLE_EQ(19.9, r.mean);
  EXPECT_DOUBLE_EQ(10, r.p50); EXPECT_DOUBLE_EQ(10, r.p95); EXPECT_DOUBLE_EQ(10, r.p99);
  EXPECT_DOUBLE_EQ(1000, r.maximum);
}
TEST(KTAGLatency, EmptyDistributionsAndZeroWeightsAreNotSamples) {
  distribution d; d.add(123, 0); EXPECT_EQ(0U, d.summarize().count);
}
TEST(KTAGLatency, PacketSequenceMustIncreaseWithinTheSameKey) {
  packet_history history; auto key = key_text(metadata(1, 0, 0).flow_key);
  history.add(key, {1, 10}); EXPECT_THROW(history.add(key, {1, 11}), std::runtime_error);
}
TEST(KTAGLatency, DeadlineAndClosedQueueDoNotConsumePhantomItems) {
  ktag_demo_bounded_queue<int> queue(2); int value = -1;
  EXPECT_FALSE(queue.pop_until(&value, clock::now())); EXPECT_EQ(-1, value);
  EXPECT_EQ(ktag_demo_queue_push_result::pushed, queue.push(42, ktag_demo_backpressure_policy::block));
  queue.close(); EXPECT_TRUE(queue.pop_until(&value, clock::now())); EXPECT_EQ(42, value);
  EXPECT_FALSE(queue.pop_until(&value, clock::now() + std::chrono::hours(1)));
}
TEST(KTAGLatency, TimedPopWakesWhenAProducerPublishesARecord) {
  ktag_demo_bounded_queue<int> queue(1);
  auto read = std::async(std::launch::async, [&] { int item = 0;
    return queue.pop_until(&item, clock::now() + std::chrono::seconds(5)) ? item : -1; });
  EXPECT_EQ(ktag_demo_queue_push_result::pushed, queue.push(7, ktag_demo_backpressure_policy::block));
  EXPECT_EQ(7, read.get());
}
TEST(KTAGInferenceTiming, RejectsMissingOrIncompatibleTimingOutput) {
  EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_inference_predict_with_timing(nullptr, nullptr, nullptr, nullptr));
  ktag_inference_timing timing{};
  EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_inference_predict_with_timing(nullptr, nullptr, nullptr, &timing));
  timing.struct_size = sizeof(timing); timing.api_version = KTAG_API_VERSION + 1;
  EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_inference_predict_with_timing(nullptr, nullptr, nullptr, &timing));
}
TEST(KTAGInferenceTiming, FailureClearsPreviouslyMeasuredTiming) {
  ktag_inference_timing timing{}; timing.struct_size = sizeof(timing); timing.api_version = KTAG_API_VERSION;
  timing.input_record_count = timing.model_record_count = 99; timing.model_nanoseconds = 123;
  EXPECT_EQ(KTAG_ERROR_INVALID_ARGUMENT, ktag_inference_predict_with_timing(nullptr, nullptr, nullptr, &timing));
  EXPECT_EQ(0U, timing.input_record_count); EXPECT_EQ(0U, timing.model_record_count); EXPECT_EQ(0U, timing.model_nanoseconds);
}
