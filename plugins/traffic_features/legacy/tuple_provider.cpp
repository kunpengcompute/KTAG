/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "tuple_provider.h"

#include <arpa/inet.h>
#include <cstring>

#include "feature/traffic_contract.h"
#include "traffic_provider_common.h"

namespace ktag_traffic_features {
namespace {

enum tuple_feature_index : size_t {
    SOURCE_IP = 0U,
    DESTINATION_IP,
    SOURCE_PORT,
    DESTINATION_PORT,
    PROTOCOL,
};

ktag_status tuple_finish(void *, void *, const ktag_flow_view *flow,
                         ktag_feature_output *output) {
    if (flow == nullptr || output == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    ktag_flow_key key{};
    if (ktag_flow_view_get_flow_key(flow, &key) != KTAG_SUCCESS) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }

    uint32_t source_ip = 0U;
    uint32_t destination_ip = 0U;
    uint16_t source_port = htons(key.port_a);
    uint16_t destination_port = htons(key.port_b);
    if (key.address_a.version == 4U) {
        ktag_ip_address forward_address{};
        ktag_ip_address reverse_address{};
        uint16_t forward_port = 0U;
        uint16_t reverse_port = 0U;
        if (ktag_flow_view_get_endpoint(flow, KTAG_DIRECTION_FORWARD,
                                        &forward_address,
                                        &forward_port) != KTAG_SUCCESS ||
            ktag_flow_view_get_endpoint(flow, KTAG_DIRECTION_REVERSE,
                                        &reverse_address,
                                        &reverse_port) != KTAG_SUCCESS) {
            return KTAG_ERROR_INVALID_ARGUMENT;
        }
        uint32_t forward_ip = 0U;
        uint32_t reverse_ip = 0U;
        std::memcpy(&forward_ip, forward_address.bytes, sizeof(forward_ip));
        std::memcpy(&reverse_ip, reverse_address.bytes, sizeof(reverse_ip));

        /* Preserve the frozen 354-value tuple ordering without exposing the old
         * flow storage object through the Provider API. */
        if (forward_ip <= reverse_ip) {
            source_ip = forward_ip;
            destination_ip = reverse_ip;
            source_port = htons(forward_port);
            destination_port = htons(reverse_port);
        } else {
            source_ip = reverse_ip;
            destination_ip = forward_ip;
            source_port = htons(reverse_port);
            destination_port = htons(forward_port);
        }
    } else if (key.address_a.version != 6U) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }

    if (set_scalar(output, KTAG_TUPLE_FEATURES[SOURCE_IP], source_ip) !=
            KTAG_SUCCESS ||
        set_scalar(output, KTAG_TUPLE_FEATURES[DESTINATION_IP],
                   destination_ip) != KTAG_SUCCESS ||
        set_scalar(output, KTAG_TUPLE_FEATURES[SOURCE_PORT], source_port) !=
            KTAG_SUCCESS ||
        set_scalar(output, KTAG_TUPLE_FEATURES[DESTINATION_PORT],
                   destination_port) != KTAG_SUCCESS ||
        set_scalar(output, KTAG_TUPLE_FEATURES[PROTOCOL], key.protocol) !=
            KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    return KTAG_SUCCESS;
}

} // namespace

ktag_status register_tuple_provider(ktag_feature_registry *registry) {
    ktag_feature_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.api_version = KTAG_API_VERSION;
    callbacks.flow_finish = tuple_finish;
    return register_provider(registry, KTAG_TUPLE_CATALOG, callbacks,
                             KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW, nullptr);
}

} // namespace ktag_traffic_features
