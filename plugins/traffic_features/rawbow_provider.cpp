/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "rawbow_provider.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>

#include "feature/traffic_contract.h"
#include "ktag_traffic_features.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

constexpr size_t TOKEN_CAPACITY = 32U;
constexpr size_t TOKEN_MAX_LENGTH = 20U;
constexpr uint32_t KEYWORD_WEIGHT = 20U;
const std::unordered_set<char> DELIMITERS{'/', '?', ',', '=', '&', '.', ':'};
const std::unordered_set<std::string> INVALID{
    "http", "https", "com", "cn", "www", "net", "org", "resolve",
    "host", "query", "api", "app", "ftp", "ftps", "ssh", "telnet",
    "smtp", "pop3", "imap", "dns", "dhcp", "tcp", "udp", "icmp",
    "edu", "gov", "mil", "int", "info", "biz", "name", "pro",
    "museum", "aero", "coop", "jobs", "travel", "mobi", "asia",
    "cat", "tel", "xxx", "post", "geo", "us", "uk", "de", "fr",
    "jp", "kr", "ru", "au", "ca", "in", "br", "mx", "it", "es",
    "nl", "se", "no", "dk", "fi", "pl", "tr", "th", "sg", "hk",
    "tw"};
const std::unordered_set<std::string_view> KEYWORDS{
    "iqiyi", "tencent", "deepseek", "qq", "bilibili", "bilivideo",
    "video", "chat", "zijie", "bytedance", "kimi", "mgtv", "hitv",
    "baidu", "yiyan", "youku", "alicdn", "taobao", "iku", "hunyuan",
    "yuanbao"};
const std::array<std::string_view, 5U> HTTP_METHODS{
    "GET ", "POST ", "PUT ", "DELETE ", "HEAD "};

struct rawbow_flow_state {
  std::array<std::string, TOKEN_CAPACITY> tokens{};
  uint32_t count = 0U;
  bool domains_added = false;
};

bool invalid_token(std::string_view token) {
  if (token.size() < 2U || token.size() > TOKEN_MAX_LENGTH ||
      std::all_of(token.begin(), token.end(), [](char value) {
        return std::isdigit(static_cast<unsigned char>(value)) != 0;
      })) {
    return true;
  }
  uint32_t letters = 0U;
  bool single_alpha = true;
  for (const char value : token) {
    if (std::isalpha(static_cast<unsigned char>(value)) != 0) {
      if (++letters > 1U) {
        single_alpha = false;
        break;
      }
    } else if (std::isdigit(static_cast<unsigned char>(value)) == 0 &&
               value != '_' && value != '-') {
      single_alpha = false;
      break;
    }
  }
  if (single_alpha && letters == 1U) {
    return true;
  }
  std::string lower(token);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  });
  return INVALID.count(lower) != 0U;
}

void add_message(rawbow_flow_state *state, std::string_view message) {
  if (state == nullptr || message.empty()) {
    return;
  }
  size_t begin = 0U;
  for (size_t end = 0U; end <= message.size(); ++end) {
    if (end != message.size() && DELIMITERS.count(message[end]) == 0U) {
      continue;
    }
    const std::string_view token = message.substr(begin, end - begin);
    if (!invalid_token(token)) {
      if (state->count >= state->tokens.size()) {
        return;
      }
      state->tokens[state->count++] = token;
    }
    begin = end + 1U;
  }
}

void add_domains(rawbow_flow_state *state, const ktag_flow_view *flow) {
  if (state == nullptr || flow == nullptr || state->domains_added) {
    return;
  }
  const char *forward =
      ktag_flow_view_get_domain(flow, KTAG_DIRECTION_FORWARD);
  const char *reverse =
      ktag_flow_view_get_domain(flow, KTAG_DIRECTION_REVERSE);
  const bool has_forward = forward != nullptr && forward[0] != '\0';
  const bool has_reverse = reverse != nullptr && reverse[0] != '\0';
  if (!has_forward && !has_reverse) {
    return;
  }
  if (has_forward) {
    add_message(state, forward);
  }
  if (has_reverse) {
    add_message(state, reverse);
  }
  state->domains_added = true;
}

std::string http_url(const uint8_t *payload, uint32_t length) {
  if (payload == nullptr || length < 7U) {
    return {};
  }
  const std::string_view text(reinterpret_cast<const char *>(payload), length);
  const bool http = std::any_of(
      HTTP_METHODS.begin(), HTTP_METHODS.end(), [text](std::string_view method) {
        return text.size() >= method.size() &&
               text.substr(0U, method.size()) == method;
      });
  if (!http || text.find("\r\n") == std::string_view::npos) {
    return {};
  }
  const size_t first_space = text.find(' ');
  const size_t url_begin = first_space == std::string_view::npos
                               ? std::string_view::npos
                               : first_space + 1U;
  const size_t url_end = text.find(' ', url_begin);
  if (url_begin == std::string_view::npos || url_end == std::string_view::npos) {
    return {};
  }
  const std::string_view uri = text.substr(url_begin, url_end - url_begin);
  if (uri.size() >= 7U && uri.substr(0U, 7U) == "http://") {
    return std::string(uri);
  }
  size_t host_begin = text.find("Host:", url_end);
  if (host_begin == std::string_view::npos) {
    return std::string(uri);
  }
  host_begin += 5U;
  while (host_begin < text.size() && text[host_begin] == ' ') {
    ++host_begin;
  }
  const size_t host_end = text.find("\r\n", host_begin);
  if (host_end == std::string_view::npos) {
    return std::string(uri);
  }
  return "http://" + std::string(text.substr(host_begin, host_end - host_begin)) +
         std::string(uri);
}

void update_crc(char value, uint16_t *crc) {
  *crc ^= static_cast<uint16_t>(value) << 8U;
  for (uint32_t bit = 0U; bit < 8U; ++bit) {
    *crc = (*crc & 0x8000U) != 0U
               ? static_cast<uint16_t>((*crc << 1U) ^ 0xA001U)
               : static_cast<uint16_t>(*crc << 1U);
  }
}

std::string rawbow_text(const rawbow_flow_state &state) {
  std::string text;
  for (uint32_t index = 0U; index < state.count; ++index) {
    if (!text.empty()) {
      text.push_back('_');
    }
    text.append(state.tokens[index]);
  }
  return text;
}

ktag_status flow_create(void *, const ktag_flow_view *, void **flow_state) {
  if (flow_state == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  *flow_state = new (std::nothrow) rawbow_flow_state();
  return *flow_state == nullptr ? KTAG_ERROR_OUT_OF_MEMORY : KTAG_SUCCESS;
}

ktag_status on_packet(void *, void *flow_state,
                      const ktag_packet_view *packet,
                      const ktag_flow_view *flow) {
  auto *state = static_cast<rawbow_flow_state *>(flow_state);
  if (state == nullptr || packet == nullptr || flow == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  add_domains(state, flow);
  uint32_t length = 0U;
  const uint8_t *payload = ktag_packet_view_get_payload(packet, &length);
  if (payload == nullptr && length != 0U) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  add_message(state, http_url(payload, length));
  return KTAG_SUCCESS;
}

ktag_status flow_finish(void *, void *flow_state, const ktag_flow_view *flow,
                        ktag_feature_output *output) {
  auto *state = static_cast<rawbow_flow_state *>(flow_state);
  if (state == nullptr || flow == nullptr || output == nullptr) {
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  add_domains(state, flow);
  std::array<float, KTAG_LEGACY_RAWBOW_DIMENSION> histogram{};
  for (uint32_t index = 0U; index < state->count; ++index) {
    uint16_t crc = 0xFFFFU;
    for (const char value : state->tokens[index]) {
      update_crc(value, &crc);
    }
    crc = static_cast<uint16_t>((crc << 8U) | (crc >> 8U));
    histogram[crc % histogram.size()] +=
        KEYWORDS.count(state->tokens[index]) != 0U ? KEYWORD_WEIGHT : 1.0F;
  }
  const ktag_status feature_status =
      set_optional(output, KTAG_RAWBOW_FEATURES[0U], histogram.data(),
                   static_cast<uint32_t>(histogram.size()));
  if (feature_status != KTAG_SUCCESS ||
      ktag_feature_output_annotations_enabled(output) == 0U) {
    return feature_status;
  }
  const std::string text = rawbow_text(*state);
  return ktag_feature_output_set_annotation(
      output, KTAG_TRAFFIC_RAWBOW_ANNOTATION, text.c_str());
}

void flow_destroy(void *, void *flow_state) {
  delete static_cast<rawbow_flow_state *>(flow_state);
}

} // namespace

ktag_status register_rawbow_provider(ktag_feature_registry *registry) {
  ktag_feature_callbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.api_version = KTAG_API_VERSION;
  callbacks.flow_create = flow_create;
  callbacks.on_packet = on_packet;
  callbacks.flow_finish = flow_finish;
  callbacks.flow_destroy = flow_destroy;
  return register_provider(registry, KTAG_RAWBOW_CATALOG, callbacks,
                           KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace ktag_traffic_features
