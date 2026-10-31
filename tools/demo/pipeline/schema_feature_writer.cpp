/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "schema_feature_writer.h"

#include <arpa/inet.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

void set_error(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
}

int compare_address(const ktag_ip_address &left, const ktag_ip_address &right) {
  if (left.version != right.version) {
    return left.version < right.version ? -1 : 1;
  }
  const uint32_t length = left.version == 4U ? 4U : KTAG_IPV6_ADDRESS_SIZE;
  return std::memcmp(left.bytes, right.bytes, length);
}

bool flow_key_less(const ktag_flow_key &left, const ktag_flow_key &right) {
  int comparison = compare_address(left.address_a, right.address_a);
  if (comparison != 0) {
    return comparison < 0;
  }
  if (left.port_a != right.port_a) {
    return left.port_a < right.port_a;
  }
  comparison = compare_address(left.address_b, right.address_b);
  if (comparison != 0) {
    return comparison < 0;
  }
  if (left.port_b != right.port_b) {
    return left.port_b < right.port_b;
  }
  return left.protocol < right.protocol;
}

std::string address_text(const ktag_ip_address &address) {
  char buffer[INET6_ADDRSTRLEN] = {};
  const int family = address.version == 4U ? AF_INET : AF_INET6;
  if (address.version != 4U && address.version != 6U) {
    return {};
  }
  if (inet_ntop(family, address.bytes, buffer, sizeof(buffer)) == nullptr) {
    return {};
  }
  return buffer;
}

std::string endpoint_text(const ktag_ip_address &address, uint16_t port) {
  const std::string address_value = address_text(address);
  if (address_value.empty()) {
    return {};
  }
  std::ostringstream output;
  if (address.version == 6U) {
    output << '[' << address_value << ']';
  } else {
    output << address_value;
  }
  output << ':' << port;
  return output.str();
}

void write_csv_string(std::ostream &output, const std::string &value) {
  output << '"';
  for (char character : value) {
    if (character == '"') {
      output << "\"\"";
    } else {
      output << character;
    }
  }
  output << '"';
}

bool write_header(std::ostream &output, const ktag_feature_schema *schema,
                  std::string *error) {
  output << "meta.flow_id,meta.ip_version,meta.src_ip,meta.dst_ip,"
            "meta.src_port,meta.dst_port,meta.protocol,meta.flow_key,"
            "meta.ipv6_extension_packet_count,"
            "meta.ip_fragment_first_packet_count,"
            "meta.first_packet_sequence,meta.last_packet_sequence";
  const uint32_t feature_count = ktag_feature_schema_get_feature_count(schema);
  for (uint32_t index = 0U; index < feature_count; ++index) {
    ktag_feature_descriptor descriptor{};
    uint32_t value_offset = 0U;
    if (ktag_feature_schema_get_descriptor(schema, index, &descriptor,
                                           &value_offset) != KTAG_SUCCESS) {
      set_error(error, "failed to read Feature Schema descriptor");
      return false;
    }
    for (uint32_t element = 0U; element < descriptor.dimension; ++element) {
      output << ',' << descriptor.feature_id;
      if (descriptor.dimension != 1U) {
        output << '[' << element << ']';
      }
    }
  }
  output << '\n';
  return static_cast<bool>(output);
}

bool write_record(std::ostream &output, uint32_t expected_value_count,
                  const ktag_demo_schema_feature_record &record,
                  std::string *error) {
  if (record.values.size() != expected_value_count) {
    set_error(error, "Feature Record value count does not match the Schema");
    return false;
  }
  const auto &details = record.details;
  const auto &metadata = details.metadata;
  const uint8_t ip_version = details.source_address.version;
  if ((ip_version != 4U && ip_version != 6U) ||
      details.destination_address.version != ip_version) {
    set_error(error, "Feature Record contains an invalid IP address");
    return false;
  }
  const std::string source = address_text(details.source_address);
  const std::string destination = address_text(details.destination_address);
  const std::string flow_key =
      ktag_demo_schema_flow_key_text(metadata.flow_key);
  if (source.empty() || destination.empty() || flow_key.empty()) {
    set_error(error, "failed to format Feature Record metadata");
    return false;
  }

  output << metadata.flow_id << ',' << static_cast<uint32_t>(ip_version) << ',';
  write_csv_string(output, source);
  output << ',';
  write_csv_string(output, destination);
  output << ',' << details.source_port << ',' << details.destination_port << ','
         << static_cast<uint32_t>(metadata.flow_key.protocol) << ',';
  write_csv_string(output, flow_key);
  output << ',' << details.ipv6_extension_packet_count << ','
         << details.ip_fragment_first_packet_count << ','
         << metadata.first_packet_sequence << ','
         << metadata.last_packet_sequence;
  for (float value : record.values) {
    if (!std::isfinite(value)) {
      set_error(error, "Feature Record contains a non-finite value");
      return false;
    }
    output << ',' << value;
  }
  output << '\n';
  return static_cast<bool>(output);
}

} // namespace

bool ktag_demo_schema_feature_record_less(
    const ktag_demo_schema_feature_record &left,
    const ktag_demo_schema_feature_record &right) {
  if (left.source_index != right.source_index) {
    return left.source_index < right.source_index;
  }
  const auto &left_metadata = left.details.metadata;
  const auto &right_metadata = right.details.metadata;
  if (flow_key_less(left_metadata.flow_key, right_metadata.flow_key)) {
    return true;
  }
  if (flow_key_less(right_metadata.flow_key, left_metadata.flow_key)) {
    return false;
  }
  if (left_metadata.first_packet_sequence !=
      right_metadata.first_packet_sequence) {
    return left_metadata.first_packet_sequence <
           right_metadata.first_packet_sequence;
  }
  if (left_metadata.last_packet_sequence !=
      right_metadata.last_packet_sequence) {
    return left_metadata.last_packet_sequence <
           right_metadata.last_packet_sequence;
  }
  return left_metadata.flow_id < right_metadata.flow_id;
}

std::string ktag_demo_schema_flow_key_text(const ktag_flow_key &key) {
  const std::string first = endpoint_text(key.address_a, key.port_a);
  const std::string second = endpoint_text(key.address_b, key.port_b);
  if (first.empty() || second.empty()) {
    return {};
  }
  std::ostringstream output;
  output << first << '-' << second << '/'
         << static_cast<uint32_t>(key.protocol);
  return output.str();
}

bool ktag_demo_write_schema_feature_csv(
    const char *path, const ktag_feature_schema *schema,
    const std::vector<ktag_demo_schema_feature_record> &records,
    std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (path == nullptr || path[0] == '\0' || schema == nullptr ||
      ktag_feature_schema_is_frozen(schema) == 0U) {
    set_error(error, "a frozen Feature Schema and output path are required");
    return false;
  }
  std::ofstream output(path);
  if (!output) {
    set_error(error, std::string("failed to open Schema CSV: ") + path);
    return false;
  }
  output << std::setprecision(9);
  if (!write_header(output, schema, error)) {
    return false;
  }
  const uint32_t value_count = ktag_feature_schema_get_value_count(schema);
  for (const auto &record : records) {
    if (!write_record(output, value_count, record, error)) {
      return false;
    }
  }
  if (!output) {
    set_error(error, "failed while writing Schema CSV");
    return false;
  }
  return true;
}
