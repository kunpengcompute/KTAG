/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "rawbow_provider.h"

#include <array>
#include <string_view>
#include <unordered_set>

#include "traffic_feature_catalog.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

constexpr uint32_t LEGACY_KEYWORD_WEIGHT = 20U;

/*
 * This is the original Legacy V1 RawBoW vocabulary, relocated with its
 * algorithm.  It is intentionally not part of the core Feature engine.
 */
const std::unordered_set<std::string_view> LEGACY_KEYWORDS{
    "iqiyi", "tencent", "deepseek", "qq", "bilibili", "bilivideo",
    "video", "chat", "zijie", "bytedance", "kimi", "mgtv", "hitv",
    "baidu", "yiyan", "youku", "alicdn", "taobao", "iku", "hunyuan",
    "yuanbao",
};

void update_crc(char byte, uint16_t *crc) {
  *crc ^= static_cast<uint16_t>(byte) << 8U;
  for (uint32_t bit = 0U; bit < 8U; ++bit) {
    *crc = (*crc & 0x8000U) != 0U
               ? static_cast<uint16_t>((*crc << 1U) ^ 0xA001U)
               : static_cast<uint16_t>(*crc << 1U);
  }
}

ktag_status rawbow_finish(
    void *,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output) {
  if (flow == nullptr || output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }

  std::array<float, KTAG_LEGACY_RAWBOW_DIMENSION> histogram{};
  const uint32_t token_count = ktag_flow_view_get_rawbow_count(flow);
  for (uint32_t index = 0U; index < token_count; ++index) {
    const char *token = ktag_flow_view_get_rawbow_token(flow, index);
    if (token == nullptr) {
      continue;
    }
    uint16_t crc = 0xFFFFU;
    const std::string_view word(token);
    for (const char byte : word) {
      update_crc(byte, &crc);
    }
    crc = static_cast<uint16_t>((crc << 8U) | (crc >> 8U));
    const size_t bucket = crc % histogram.size();
    histogram[bucket] += LEGACY_KEYWORDS.count(word) != 0U
                             ? static_cast<float>(LEGACY_KEYWORD_WEIGHT)
                             : 1.0F;
  }
  return set_optional(
      output, KTAG_RAWBOW_FEATURES[0U], histogram.data(),
      static_cast<uint32_t>(histogram.size()));
}

} // namespace

ktag_status register_rawbow_provider(ktag_feature_registry *registry) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.flow_finish = rawbow_finish;
  return register_provider(registry, KTAG_RAWBOW_CATALOG, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace ktag_traffic_features
