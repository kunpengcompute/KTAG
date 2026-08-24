/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_SCHEMA_FEATURE_WRITER_H
#define KTAG_DEMO_SCHEMA_FEATURE_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

#include "ktag_feature.h"

struct ktag_demo_schema_feature_record {
  uint32_t source_index = 0U;
  ktag_feature_record_details details{};
  std::vector<float> values;
};

bool ktag_demo_schema_feature_record_less(
    const ktag_demo_schema_feature_record &left,
    const ktag_demo_schema_feature_record &right);

std::string ktag_demo_schema_flow_key_text(const ktag_flow_key &key);

bool ktag_demo_write_schema_feature_csv(
    const char *path, const ktag_feature_schema *schema,
    const std::vector<ktag_demo_schema_feature_record> &records,
    std::string *error);

bool ktag_demo_write_schema_feature_json(
    const char *path, const ktag_feature_schema *schema,
    const std::vector<ktag_demo_schema_feature_record> &records,
    std::string *error);

#endif /* KTAG_DEMO_SCHEMA_FEATURE_WRITER_H */
