/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_FEATURE_PIPELINE_INTERNAL_H
#define KTAG_FEATURE_PIPELINE_INTERNAL_H

#include <string>
#include <vector>

#include "feature_view_internal.h"
#include "provider_registry_internal.h"

struct ktag_feature_output {
  const ktag_feature_schema *schema = nullptr;
  const std::string *current_provider_id = nullptr;
  float *values = nullptr;
  uint32_t value_capacity = 0;
  std::vector<uint8_t> *written = nullptr;
};

struct ktag_feature_pipeline_provider {
  ktag_feature_provider_storage registration;
  void *provider_state = nullptr;
  bool provider_created = false;
  bool available = true;
};

struct ktag_feature_pipeline {
  ktag_feature_schema *schema = nullptr;
  std::vector<ktag_feature_pipeline_provider> providers;
  std::vector<uint8_t> written_scratch;
  bool requires_flow_state_storage = false;
};

struct ktag_feature_flow_runtime {
  ktag_feature_pipeline *pipeline = nullptr;
  ktag_flow_view flow;
  std::vector<void *> flow_states;
  std::vector<uint8_t> flow_state_created;
  std::vector<uint8_t> provider_failed;
  bool active = false;
};

ktag_feature_pipeline *
ktag_feature_pipeline_create_internal(const ktag_feature_schema *schema,
                                      const ktag_feature_registry *registry);

void ktag_feature_pipeline_destroy_internal(ktag_feature_pipeline *pipeline);

ktag_status ktag_feature_flow_runtime_create_internal(
    ktag_feature_pipeline *pipeline, const flow_hash_node *node,
    uint64_t flow_id, ktag_feature_flow_runtime *runtime);

ktag_status ktag_feature_flow_runtime_create_with_key_internal(
    ktag_feature_pipeline *pipeline, const flow_hash_node *node,
    uint64_t flow_id, const ktag_flow_key *flow_key,
    const ktag_ip_address *forward_address, uint16_t forward_port,
    bool forward_is_node_source, ktag_feature_flow_runtime *runtime);

ktag_status ktag_feature_flow_runtime_on_packet_internal(
    ktag_feature_flow_runtime *runtime, const uint8_t *packet,
    uint32_t packet_length, const ktag_timestamp *timestamp);

ktag_status ktag_feature_flow_runtime_on_packet_view_internal(
    ktag_feature_flow_runtime *runtime, const uint8_t *packet,
    uint32_t packet_length, const ktag_timestamp *timestamp,
    const uint8_t *payload, uint32_t payload_length,
    uint32_t captured_header_length,
    const ktag_packet_transport_view *transport);

ktag_status
ktag_feature_flow_runtime_finish_internal(ktag_feature_flow_runtime *runtime,
                                          float *output,
                                          uint32_t output_capacity);

void ktag_feature_flow_runtime_destroy_internal(
    ktag_feature_flow_runtime *runtime);

#endif /* KTAG_FEATURE_PIPELINE_INTERNAL_H */
