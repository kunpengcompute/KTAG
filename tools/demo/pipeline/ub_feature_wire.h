/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_UB_FEATURE_WIRE_H
#define KTAG_DEMO_UB_FEATURE_WIRE_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "schema_feature_writer.h"

constexpr uint64_t KTAG_DEMO_UB_FRAME_MAGIC = 0x4b54414755424652ULL;
// Version 2 carries input byte/packet totals in DRAIN for distributed metrics.
constexpr uint32_t KTAG_DEMO_UB_FRAME_VERSION = 2U;
constexpr uint32_t KTAG_DEMO_UB_FRAME_HEADER_SIZE = 88U;
constexpr uint32_t KTAG_DEMO_UB_DRAIN_FRAME_SIZE =
    KTAG_DEMO_UB_FRAME_HEADER_SIZE + 24U;
constexpr uint32_t KTAG_DEMO_UB_FRAME_FLAG_GROUND_TRUTH_VALID = 1U;

enum class ktag_demo_ub_message_type : uint32_t {
  credit = 1U,
  feature = 2U,
  drain = 3U,
  drain_ack = 4U,
  error = 5U
};

struct ktag_demo_ub_frame_contract {
  uint64_t frontend_id_hash = 0U;
  uint64_t epoch = 0U;
  uint64_t sequence = 0U;
  uint64_t schema_hash = 0U;
  uint64_t ruleset_hash = 0U;
};

struct ktag_demo_ub_frame_info {
  ktag_demo_ub_message_type message_type =
      ktag_demo_ub_message_type::error;
  uint32_t flags = 0U;
  uint32_t frame_size = 0U;
  uint32_t record_count = 0U;
  uint32_t value_count = 0U;
  uint32_t source_index = 0U;
  ktag_demo_ub_frame_contract contract{};
};

struct ktag_demo_ub_stream_statistics {
  uint64_t input_packets = 0U;
  uint64_t input_bytes = 0U;
  uint64_t feature_records = 0U;
};

size_t ktag_demo_ub_feature_frame_size(uint32_t value_count);

bool ktag_demo_ub_encode_control_frame(
    void *buffer, size_t buffer_capacity,
    ktag_demo_ub_message_type message_type,
    const ktag_demo_ub_frame_contract &contract, size_t *encoded_size,
    std::string *error);

bool ktag_demo_ub_encode_feature_frame(
    void *buffer, size_t buffer_capacity,
    const ktag_demo_ub_frame_contract &contract,
    const ktag_demo_schema_feature_record &record, bool ground_truth_valid,
    size_t *encoded_size, std::string *error);

bool ktag_demo_ub_encode_drain_frame(
    void *buffer, size_t buffer_capacity,
    const ktag_demo_ub_frame_contract &contract,
    const ktag_demo_ub_stream_statistics &statistics, size_t *encoded_size,
    std::string *error);

bool ktag_demo_ub_decode_drain_frame(
    const void *buffer, size_t buffer_size,
    ktag_demo_ub_stream_statistics *statistics, std::string *error);

bool ktag_demo_ub_inspect_frame(const void *buffer, size_t buffer_size,
                                ktag_demo_ub_frame_info *info,
                                std::string *error);

bool ktag_demo_ub_decode_feature_frame(
    const void *buffer, size_t buffer_size, ktag_demo_ub_frame_info *info,
    ktag_demo_schema_feature_record *record, std::string *error);

#endif /* KTAG_DEMO_UB_FEATURE_WIRE_H */
