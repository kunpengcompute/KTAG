/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ub_feature_wire.h"

#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr size_t RECORD_FIXED_SIZE = 109U;

void set_error(std::string *error, const char *message) {
  if (error != nullptr) {
    *error = message;
  }
}

class wire_writer {
public:
  wire_writer(void *buffer, size_t capacity)
      : data_(static_cast<uint8_t *>(buffer)), capacity_(capacity) {}

  bool put_u8(uint8_t value) {
    if (!reserve(1U)) {
      return false;
    }
    data_[offset_++] = value;
    return true;
  }

  bool put_u16(uint16_t value) {
    return put_u8(static_cast<uint8_t>(value)) &&
           put_u8(static_cast<uint8_t>(value >> 8U));
  }

  bool put_u32(uint32_t value) {
    return put_u16(static_cast<uint16_t>(value)) &&
           put_u16(static_cast<uint16_t>(value >> 16U));
  }

  bool put_u64(uint64_t value) {
    return put_u32(static_cast<uint32_t>(value)) &&
           put_u32(static_cast<uint32_t>(value >> 32U));
  }

  bool put_bytes(const void *source, size_t size) {
    if (!reserve(size)) {
      return false;
    }
    if (size != 0U) {
      std::memcpy(data_ + offset_, source, size);
    }
    offset_ += size;
    return true;
  }

  size_t size() const { return offset_; }

private:
  bool reserve(size_t size) const {
    return size <= capacity_ && offset_ <= capacity_ - size;
  }

  uint8_t *data_ = nullptr;
  size_t capacity_ = 0U;
  size_t offset_ = 0U;
};

class wire_reader {
public:
  wire_reader(const void *buffer, size_t size)
      : data_(static_cast<const uint8_t *>(buffer)), size_(size) {}

  bool get_u8(uint8_t *value) {
    if (!reserve(1U)) {
      return false;
    }
    *value = data_[offset_++];
    return true;
  }

  bool get_u16(uint16_t *value) {
    uint8_t low = 0U;
    uint8_t high = 0U;
    if (!get_u8(&low) || !get_u8(&high)) {
      return false;
    }
    *value = static_cast<uint16_t>(low) |
             static_cast<uint16_t>(static_cast<uint16_t>(high) << 8U);
    return true;
  }

  bool get_u32(uint32_t *value) {
    uint16_t low = 0U;
    uint16_t high = 0U;
    if (!get_u16(&low) || !get_u16(&high)) {
      return false;
    }
    *value = static_cast<uint32_t>(low) |
             (static_cast<uint32_t>(high) << 16U);
    return true;
  }

  bool get_u64(uint64_t *value) {
    uint32_t low = 0U;
    uint32_t high = 0U;
    if (!get_u32(&low) || !get_u32(&high)) {
      return false;
    }
    *value = static_cast<uint64_t>(low) |
             (static_cast<uint64_t>(high) << 32U);
    return true;
  }

  bool get_bytes(void *destination, size_t size) {
    if (!reserve(size)) {
      return false;
    }
    if (size != 0U) {
      std::memcpy(destination, data_ + offset_, size);
    }
    offset_ += size;
    return true;
  }

  size_t offset() const { return offset_; }

private:
  bool reserve(size_t size) const {
    return size <= size_ && offset_ <= size_ - size;
  }

  const uint8_t *data_ = nullptr;
  size_t size_ = 0U;
  size_t offset_ = 0U;
};

uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0U; index < size; ++index) {
    crc ^= data[index];
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

bool valid_message_type(uint32_t value) {
  return value >= static_cast<uint32_t>(ktag_demo_ub_message_type::credit) &&
         value <= static_cast<uint32_t>(ktag_demo_ub_message_type::error);
}

bool control_message_type(ktag_demo_ub_message_type type) {
  return type == ktag_demo_ub_message_type::credit ||
         type == ktag_demo_ub_message_type::drain_ack ||
         type == ktag_demo_ub_message_type::error;
}

bool put_address(wire_writer *writer, const ktag_ip_address &address) {
  return writer->put_u8(address.version) &&
         writer->put_bytes(address.bytes, KTAG_IPV6_ADDRESS_SIZE);
}

bool get_address(wire_reader *reader, ktag_ip_address *address) {
  *address = {};
  return reader->get_u8(&address->version) &&
         reader->get_bytes(address->bytes, KTAG_IPV6_ADDRESS_SIZE);
}

bool put_header(wire_writer *writer, ktag_demo_ub_message_type message_type,
                uint32_t flags, uint32_t frame_size, uint32_t record_count,
                uint32_t value_count, uint32_t source_index,
                const ktag_demo_ub_frame_contract &contract,
                uint32_t payload_crc) {
  return writer->put_u64(KTAG_DEMO_UB_FRAME_MAGIC) &&
         writer->put_u32(KTAG_DEMO_UB_FRAME_VERSION) &&
         writer->put_u32(KTAG_DEMO_UB_FRAME_HEADER_SIZE) &&
         writer->put_u32(static_cast<uint32_t>(message_type)) &&
         writer->put_u32(flags) && writer->put_u32(frame_size) &&
         writer->put_u32(record_count) && writer->put_u32(value_count) &&
         writer->put_u32(source_index) &&
         writer->put_u64(contract.frontend_id_hash) &&
         writer->put_u64(contract.epoch) &&
         writer->put_u64(contract.sequence) &&
         writer->put_u64(contract.schema_hash) &&
         writer->put_u64(contract.ruleset_hash) &&
         writer->put_u32(payload_crc) && writer->put_u32(0U);
}

bool put_record(wire_writer *writer,
                const ktag_demo_schema_feature_record &record) {
  const auto &details = record.details;
  const auto &metadata = details.metadata;
  const auto &flow_key = metadata.flow_key;
  if (!writer->put_u64(metadata.flow_id) ||
      !put_address(writer, flow_key.address_a) ||
      !put_address(writer, flow_key.address_b) ||
      !writer->put_u16(flow_key.port_a) ||
      !writer->put_u16(flow_key.port_b) ||
      !writer->put_u8(flow_key.protocol) ||
      !writer->put_u64(metadata.first_packet_sequence) ||
      !writer->put_u64(metadata.last_packet_sequence) ||
      !put_address(writer, details.source_address) ||
      !put_address(writer, details.destination_address) ||
      !writer->put_u16(details.source_port) ||
      !writer->put_u16(details.destination_port) ||
      !writer->put_u32(details.ipv6_extension_packet_count) ||
      !writer->put_u32(details.ip_fragment_first_packet_count)) {
    return false;
  }
  static_assert(sizeof(float) == sizeof(uint32_t),
                "UB Feature Wire requires 32-bit float");
  for (float value : record.values) {
    uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    if (!writer->put_u32(bits)) {
      return false;
    }
  }
  return true;
}

bool get_record(wire_reader *reader, uint32_t value_count,
                uint32_t source_index,
                ktag_demo_schema_feature_record *record) {
  ktag_demo_schema_feature_record decoded;
  decoded.source_index = source_index;
  decoded.details.struct_size = sizeof(decoded.details);
  decoded.details.api_version = KTAG_API_VERSION;
  auto &metadata = decoded.details.metadata;
  metadata.struct_size = sizeof(metadata);
  metadata.api_version = KTAG_API_VERSION;
  metadata.flow_key.struct_size = sizeof(metadata.flow_key);
  metadata.flow_key.api_version = KTAG_API_VERSION;

  if (!reader->get_u64(&metadata.flow_id) ||
      !get_address(reader, &metadata.flow_key.address_a) ||
      !get_address(reader, &metadata.flow_key.address_b) ||
      !reader->get_u16(&metadata.flow_key.port_a) ||
      !reader->get_u16(&metadata.flow_key.port_b) ||
      !reader->get_u8(&metadata.flow_key.protocol) ||
      !reader->get_u64(&metadata.first_packet_sequence) ||
      !reader->get_u64(&metadata.last_packet_sequence) ||
      !get_address(reader, &decoded.details.source_address) ||
      !get_address(reader, &decoded.details.destination_address) ||
      !reader->get_u16(&decoded.details.source_port) ||
      !reader->get_u16(&decoded.details.destination_port) ||
      !reader->get_u32(&decoded.details.ipv6_extension_packet_count) ||
      !reader->get_u32(&decoded.details.ip_fragment_first_packet_count)) {
    return false;
  }

  try {
    decoded.values.resize(value_count);
  } catch (...) {
    return false;
  }
  for (uint32_t index = 0U; index < value_count; ++index) {
    uint32_t bits = 0U;
    if (!reader->get_u32(&bits)) {
      return false;
    }
    std::memcpy(&decoded.values[index], &bits, sizeof(bits));
  }
  *record = std::move(decoded);
  return true;
}

bool read_header(const void *buffer, size_t buffer_size,
                 ktag_demo_ub_frame_info *info, uint32_t *payload_crc,
                 std::string *error) {
  if (buffer == nullptr || info == nullptr || payload_crc == nullptr) {
    set_error(error, "UB frame arguments are required");
    return false;
  }
  if (buffer_size < KTAG_DEMO_UB_FRAME_HEADER_SIZE) {
    set_error(error, "UB frame is smaller than its fixed header");
    return false;
  }

  wire_reader reader(buffer, KTAG_DEMO_UB_FRAME_HEADER_SIZE);
  uint64_t magic = 0U;
  uint32_t version = 0U;
  uint32_t header_size = 0U;
  uint32_t message_type = 0U;
  uint32_t reserved = 0U;
  ktag_demo_ub_frame_info decoded;
  if (!reader.get_u64(&magic) || !reader.get_u32(&version) ||
      !reader.get_u32(&header_size) || reader.offset() != 16U ||
      !reader.get_u32(&message_type) || !reader.get_u32(&decoded.flags) ||
      !reader.get_u32(&decoded.frame_size) ||
      !reader.get_u32(&decoded.record_count) ||
      !reader.get_u32(&decoded.value_count) ||
      !reader.get_u32(&decoded.source_index) ||
      !reader.get_u64(&decoded.contract.frontend_id_hash) ||
      !reader.get_u64(&decoded.contract.epoch) ||
      !reader.get_u64(&decoded.contract.sequence) ||
      !reader.get_u64(&decoded.contract.schema_hash) ||
      !reader.get_u64(&decoded.contract.ruleset_hash) ||
      !reader.get_u32(payload_crc) || !reader.get_u32(&reserved)) {
    set_error(error, "UB frame header is truncated");
    return false;
  }
  if (magic != KTAG_DEMO_UB_FRAME_MAGIC) {
    set_error(error, "UB frame magic is incompatible");
    return false;
  }
  if (version != KTAG_DEMO_UB_FRAME_VERSION) {
    if (error != nullptr) {
      *error = "UB frame version is incompatible: expected=" +
               std::to_string(KTAG_DEMO_UB_FRAME_VERSION) +
               ", received=" + std::to_string(version) +
               "; rebuild and deploy both endpoints";
    }
    return false;
  }
  if (header_size != KTAG_DEMO_UB_FRAME_HEADER_SIZE) {
    set_error(error, "UB frame header size is incompatible");
    return false;
  }
  if (!valid_message_type(message_type)) {
    set_error(error, "UB frame message type is invalid");
    return false;
  }
  if (decoded.frame_size < KTAG_DEMO_UB_FRAME_HEADER_SIZE ||
      decoded.frame_size > buffer_size) {
    set_error(error, "UB frame size exceeds the received buffer");
    return false;
  }
  if (reserved != 0U ||
      (decoded.flags &
       ~KTAG_DEMO_UB_FRAME_FLAG_GROUND_TRUTH_VALID) != 0U) {
    set_error(error, "UB frame contains unsupported flags or reserved data");
    return false;
  }
  decoded.message_type =
      static_cast<ktag_demo_ub_message_type>(message_type);
  if (decoded.message_type == ktag_demo_ub_message_type::feature) {
    if (decoded.record_count != 1U ||
        decoded.frame_size !=
            ktag_demo_ub_feature_frame_size(decoded.value_count)) {
      set_error(error, "UB Feature frame shape is inconsistent");
      return false;
    }
  } else if (decoded.message_type == ktag_demo_ub_message_type::drain) {
    if (decoded.record_count != 0U || decoded.value_count != 0U ||
        decoded.frame_size != KTAG_DEMO_UB_DRAIN_FRAME_SIZE ||
        decoded.flags != 0U) {
      set_error(error, "UB DRAIN frame must contain stream statistics");
      return false;
    }
  } else if (decoded.record_count != 0U || decoded.value_count != 0U ||
             decoded.frame_size != KTAG_DEMO_UB_FRAME_HEADER_SIZE ||
             decoded.flags != 0U) {
    set_error(error, "UB control frame must not contain a record payload");
    return false;
  }
  *info = decoded;
  return true;
}

} // namespace

size_t ktag_demo_ub_feature_frame_size(uint32_t value_count) {
  constexpr size_t max_size = std::numeric_limits<size_t>::max();
  if (value_count >
      (max_size - KTAG_DEMO_UB_FRAME_HEADER_SIZE - RECORD_FIXED_SIZE) /
          sizeof(float)) {
    return 0U;
  }
  return KTAG_DEMO_UB_FRAME_HEADER_SIZE + RECORD_FIXED_SIZE +
         static_cast<size_t>(value_count) * sizeof(float);
}

bool ktag_demo_ub_encode_control_frame(
    void *buffer, size_t buffer_capacity,
    ktag_demo_ub_message_type message_type,
    const ktag_demo_ub_frame_contract &contract, size_t *encoded_size,
    std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (buffer == nullptr || encoded_size == nullptr ||
      !control_message_type(message_type)) {
    set_error(error, "valid UB control-frame arguments are required");
    return false;
  }
  if (buffer_capacity < KTAG_DEMO_UB_FRAME_HEADER_SIZE) {
    set_error(error, "UB buffer is too small for a control frame");
    return false;
  }
  wire_writer writer(buffer, buffer_capacity);
  if (!put_header(&writer, message_type, 0U,
                  KTAG_DEMO_UB_FRAME_HEADER_SIZE, 0U, 0U, 0U, contract, 0U)) {
    set_error(error, "failed to encode UB control frame");
    return false;
  }
  *encoded_size = writer.size();
  return true;
}

bool ktag_demo_ub_encode_drain_frame(
    void *buffer, size_t buffer_capacity,
    const ktag_demo_ub_frame_contract &contract,
    const ktag_demo_ub_stream_statistics &statistics, size_t *encoded_size,
    std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (buffer == nullptr || encoded_size == nullptr ||
      buffer_capacity < KTAG_DEMO_UB_DRAIN_FRAME_SIZE) {
    set_error(error, "UB buffer is too small for DRAIN statistics");
    return false;
  }
  if (statistics.feature_records != contract.sequence) {
    set_error(error, "UB DRAIN statistics do not match the Feature sequence");
    return false;
  }
  auto *payload = static_cast<uint8_t *>(buffer) + KTAG_DEMO_UB_FRAME_HEADER_SIZE;
  wire_writer writer(payload,
                     KTAG_DEMO_UB_DRAIN_FRAME_SIZE - KTAG_DEMO_UB_FRAME_HEADER_SIZE);
  if (!writer.put_u64(statistics.input_packets) ||
      !writer.put_u64(statistics.input_bytes) ||
      !writer.put_u64(statistics.feature_records)) {
    set_error(error, "failed to encode UB DRAIN statistics");
    return false;
  }
  wire_writer header_writer(buffer, KTAG_DEMO_UB_FRAME_HEADER_SIZE);
  if (!put_header(&header_writer, ktag_demo_ub_message_type::drain, 0U,
                  KTAG_DEMO_UB_DRAIN_FRAME_SIZE, 0U, 0U, 0U, contract,
                  crc32(payload, writer.size()))) {
    set_error(error, "failed to encode UB DRAIN header");
    return false;
  }
  *encoded_size = KTAG_DEMO_UB_DRAIN_FRAME_SIZE;
  return true;
}

bool ktag_demo_ub_decode_drain_frame(
    const void *buffer, size_t buffer_size,
    ktag_demo_ub_stream_statistics *statistics, std::string *error) {
  if (statistics == nullptr) {
    set_error(error, "UB DRAIN statistics output is required");
    return false;
  }
  ktag_demo_ub_frame_info info;
  if (!ktag_demo_ub_inspect_frame(buffer, buffer_size, &info, error)) {
    return false;
  }
  if (info.message_type != ktag_demo_ub_message_type::drain) {
    set_error(error, "UB frame is not a DRAIN frame");
    return false;
  }
  wire_reader reader(
      static_cast<const uint8_t *>(buffer) + KTAG_DEMO_UB_FRAME_HEADER_SIZE,
      KTAG_DEMO_UB_DRAIN_FRAME_SIZE - KTAG_DEMO_UB_FRAME_HEADER_SIZE);
  ktag_demo_ub_stream_statistics decoded;
  if (!reader.get_u64(&decoded.input_packets) ||
      !reader.get_u64(&decoded.input_bytes) ||
      !reader.get_u64(&decoded.feature_records) ||
      decoded.feature_records != info.contract.sequence) {
    set_error(error, "UB DRAIN statistics do not match the Feature sequence");
    return false;
  }
  *statistics = decoded;
  return true;
}

bool ktag_demo_ub_encode_feature_frame(
    void *buffer, size_t buffer_capacity,
    const ktag_demo_ub_frame_contract &contract,
    const ktag_demo_schema_feature_record &record, bool ground_truth_valid,
    size_t *encoded_size, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (buffer == nullptr || encoded_size == nullptr ||
      record.values.size() > std::numeric_limits<uint32_t>::max()) {
    set_error(error, "valid UB Feature-frame arguments are required");
    return false;
  }
  const uint32_t value_count = static_cast<uint32_t>(record.values.size());
  const size_t frame_size = ktag_demo_ub_feature_frame_size(value_count);
  if (frame_size == 0U || frame_size > buffer_capacity ||
      frame_size > std::numeric_limits<uint32_t>::max()) {
    set_error(error, "UB buffer is too small for the Feature record");
    return false;
  }

  wire_writer payload_writer(
      static_cast<uint8_t *>(buffer) + KTAG_DEMO_UB_FRAME_HEADER_SIZE,
      buffer_capacity - KTAG_DEMO_UB_FRAME_HEADER_SIZE);
  if (!put_record(&payload_writer, record) ||
      payload_writer.size() != frame_size - KTAG_DEMO_UB_FRAME_HEADER_SIZE) {
    set_error(error, "failed to encode UB Feature payload");
    return false;
  }
  const uint32_t payload_crc = crc32(
      static_cast<const uint8_t *>(buffer) + KTAG_DEMO_UB_FRAME_HEADER_SIZE,
      payload_writer.size());
  wire_writer header_writer(buffer, KTAG_DEMO_UB_FRAME_HEADER_SIZE);
  if (!put_header(&header_writer, ktag_demo_ub_message_type::feature,
                  ground_truth_valid
                      ? KTAG_DEMO_UB_FRAME_FLAG_GROUND_TRUTH_VALID
                      : 0U,
                  static_cast<uint32_t>(frame_size), 1U, value_count,
                  record.source_index, contract, payload_crc) ||
      header_writer.size() != KTAG_DEMO_UB_FRAME_HEADER_SIZE) {
    set_error(error, "failed to encode UB Feature header");
    return false;
  }
  *encoded_size = frame_size;
  return true;
}

bool ktag_demo_ub_inspect_frame(const void *buffer, size_t buffer_size,
                                ktag_demo_ub_frame_info *info,
                                std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  uint32_t payload_crc = 0U;
  ktag_demo_ub_frame_info decoded;
  if (!read_header(buffer, buffer_size, &decoded, &payload_crc, error)) {
    return false;
  }
  const auto *bytes = static_cast<const uint8_t *>(buffer);
  const size_t payload_size =
      decoded.frame_size - KTAG_DEMO_UB_FRAME_HEADER_SIZE;
  if (crc32(bytes + KTAG_DEMO_UB_FRAME_HEADER_SIZE, payload_size) !=
      payload_crc) {
    set_error(error, "UB frame payload CRC does not match");
    return false;
  }
  if (info != nullptr) {
    *info = decoded;
  }
  return true;
}

bool ktag_demo_ub_decode_feature_frame(
    const void *buffer, size_t buffer_size, ktag_demo_ub_frame_info *info,
    ktag_demo_schema_feature_record *record, std::string *error) {
  if (record == nullptr) {
    set_error(error, "UB Feature output record is required");
    return false;
  }
  ktag_demo_ub_frame_info decoded;
  if (!ktag_demo_ub_inspect_frame(buffer, buffer_size, &decoded, error)) {
    return false;
  }
  if (decoded.message_type != ktag_demo_ub_message_type::feature) {
    set_error(error, "UB frame is not a Feature frame");
    return false;
  }
  wire_reader reader(
      static_cast<const uint8_t *>(buffer) + KTAG_DEMO_UB_FRAME_HEADER_SIZE,
      decoded.frame_size - KTAG_DEMO_UB_FRAME_HEADER_SIZE);
  ktag_demo_schema_feature_record output;
  if (!get_record(&reader, decoded.value_count, decoded.source_index,
                  &output) ||
      reader.offset() !=
          decoded.frame_size - KTAG_DEMO_UB_FRAME_HEADER_SIZE) {
    set_error(error, "UB Feature payload is truncated");
    return false;
  }
  *record = std::move(output);
  if (info != nullptr) {
    *info = decoded;
  }
  return true;
}
