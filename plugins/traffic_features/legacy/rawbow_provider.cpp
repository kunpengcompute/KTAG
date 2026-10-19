/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "rawbow_provider.h"
#include "legacy/rawbow_token_dfa.h"
#include "legacy/http.h"
#include "legacy/token_histogram.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <new>
#include <string>
#include <string_view>

#include "feature/traffic_contract.h"
#include "ktag_traffic_features.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

constexpr size_t TOKEN_CAPACITY = 32U;

struct rawbow_flow_state {
    std::array<std::string, TOKEN_CAPACITY> tokens{};
    uint32_t count = 0U;
    bool domains_added = false;
};



void add_message(rawbow_flow_state *state, std::string_view message) {
    if (state == nullptr || message.empty() || state->count >= state->tokens.size()) {
        return;
    }
    const auto tokens = rawbow_dfa::collect_filtered_tokens(
        message, state->tokens.size() - state->count);
    for (const auto &token : tokens) {
        state->tokens[state->count++] = token;
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

ktag_status on_packet(void *, void *flow_state, const ktag_packet_view *packet,
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
    add_message(state, legacy::http_url(payload, length));
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
        legacy::add_crc_token(state->tokens[index], histogram.data(),
                              static_cast<uint32_t>(histogram.size()));
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
