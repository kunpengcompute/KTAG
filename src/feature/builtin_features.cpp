/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "feature_pipeline_internal.h"

#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

ktag_status set_optional(
    ktag_feature_output *output,
    const char *feature_id,
    const float *values,
    uint32_t value_count)
{
    const ktag_status status = ktag_feature_output_set(output, feature_id, values, value_count);
    return status == KTAG_ERROR_NOT_FOUND ? KTAG_SUCCESS : status;
}

ktag_status set_scalar(ktag_feature_output *output, const char *feature_id, double value)
{
    const float converted = static_cast<float>(value);
    return set_optional(output, feature_id, &converted, 1);
}

struct statistics_feature_ids {
    const char *minimum;
    const char *maximum;
    const char *mean;
    const char *variance;
    const char *relative_histogram;
    const char *absolute_histogram;
    const char *normalized_histogram;
};

constexpr statistics_feature_ids SEND_HEADER_IDS = {
    "traffic.send.header.minimum", "traffic.send.header.maximum",
    "traffic.send.header.mean", "traffic.send.header.variance",
    "traffic.send.header.histogram.relative", "traffic.send.header.histogram.absolute",
    "traffic.send.header.histogram.normalized"};
constexpr statistics_feature_ids SEND_PAYLOAD_IDS = {
    "traffic.send.payload.minimum", "traffic.send.payload.maximum",
    "traffic.send.payload.mean", "traffic.send.payload.variance",
    "traffic.send.payload.histogram.relative", "traffic.send.payload.histogram.absolute",
    "traffic.send.payload.histogram.normalized"};
constexpr statistics_feature_ids SEND_INTERVAL_IDS = {
    "traffic.send.interval.minimum", "traffic.send.interval.maximum",
    "traffic.send.interval.mean", "traffic.send.interval.variance",
    "traffic.send.interval.histogram.relative", "traffic.send.interval.histogram.absolute",
    "traffic.send.interval.histogram.normalized"};
constexpr statistics_feature_ids RECEIVE_HEADER_IDS = {
    "traffic.receive.header.minimum", "traffic.receive.header.maximum",
    "traffic.receive.header.mean", "traffic.receive.header.variance",
    "traffic.receive.header.histogram.relative", "traffic.receive.header.histogram.absolute",
    "traffic.receive.header.histogram.normalized"};
constexpr statistics_feature_ids RECEIVE_PAYLOAD_IDS = {
    "traffic.receive.payload.minimum", "traffic.receive.payload.maximum",
    "traffic.receive.payload.mean", "traffic.receive.payload.variance",
    "traffic.receive.payload.histogram.relative", "traffic.receive.payload.histogram.absolute",
    "traffic.receive.payload.histogram.normalized"};
constexpr statistics_feature_ids RECEIVE_INTERVAL_IDS = {
    "traffic.receive.interval.minimum", "traffic.receive.interval.maximum",
    "traffic.receive.interval.mean", "traffic.receive.interval.variance",
    "traffic.receive.interval.histogram.relative", "traffic.receive.interval.histogram.absolute",
    "traffic.receive.interval.histogram.normalized"};

ktag_status set_statistics(
    ktag_feature_output *output,
    const statistics_feature_ids &ids,
    const statistics_info &statistics)
{
    if (set_scalar(output, ids.minimum, statistics.min_value) != KTAG_SUCCESS ||
        set_scalar(output, ids.maximum, statistics.max_value) != KTAG_SUCCESS ||
        set_scalar(output, ids.mean, statistics.mean_value) != KTAG_SUCCESS ||
        set_scalar(output, ids.variance, statistics.var_value) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }

    float histogram[HISTOGRAM_NUMS];
    for (uint32_t index = 0; index < HISTOGRAM_NUMS; ++index) {
        histogram[index] = static_cast<float>(statistics.hist_array[index]);
    }
    if (set_optional(output, ids.relative_histogram, histogram, HISTOGRAM_NUMS) !=
        KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    for (uint32_t index = 0; index < HISTOGRAM_NUMS; ++index) {
        histogram[index] = static_cast<float>(statistics.hist_array_abs[index]);
    }
    if (set_optional(output, ids.absolute_histogram, histogram, HISTOGRAM_NUMS) !=
        KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    for (uint32_t index = 0; index < HISTOGRAM_NUMS; ++index) {
        histogram[index] = static_cast<float>(statistics.hist_array_nor[index]);
    }
    return set_optional(output, ids.normalized_histogram, histogram, HISTOGRAM_NUMS);
}

ktag_status metadata_finish(
    void *,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    const float flow_id = static_cast<float>(ktag_flow_view_get_flow_id(flow));
    return set_optional(output, "legacy.flow_id", &flow_id, 1);
}

ktag_status tuple_finish(
    void *,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    const stream_feature *feature = nullptr;
    if (ktag_flow_view_get_stream_feature_internal(flow, &feature) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    if (set_scalar(output, "network.src_ip", feature->src_ip) != KTAG_SUCCESS ||
        set_scalar(output, "network.dst_ip", feature->dst_ip) != KTAG_SUCCESS ||
        set_scalar(output, "network.src_port", feature->src_port) != KTAG_SUCCESS ||
        set_scalar(output, "network.dst_port", feature->dst_port) != KTAG_SUCCESS ||
        set_scalar(output, "network.protocol", feature->protocol) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    return KTAG_SUCCESS;
}

ktag_status traffic_finish(
    void *,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    const stream_feature *feature = nullptr;
    if (ktag_flow_view_get_stream_feature_internal(flow, &feature) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }

    if (set_scalar(output, "traffic.send.byte_count", feature->send_byte_nums) != KTAG_SUCCESS ||
        set_scalar(output, "traffic.send.packet_count", feature->send_packet_nums) != KTAG_SUCCESS ||
        set_statistics(output, SEND_HEADER_IDS, feature->send_hdr) != KTAG_SUCCESS ||
        set_statistics(output, SEND_PAYLOAD_IDS, feature->send_pld) != KTAG_SUCCESS ||
        set_statistics(output, SEND_INTERVAL_IDS, feature->send_interval) != KTAG_SUCCESS ||
        set_scalar(output, "traffic.receive.byte_count", feature->receive_byte_nums) != KTAG_SUCCESS ||
        set_scalar(output, "traffic.receive.packet_count", feature->receive_packet_nums) != KTAG_SUCCESS ||
        set_statistics(output, RECEIVE_HEADER_IDS, feature->receive_hdr) != KTAG_SUCCESS ||
        set_statistics(output, RECEIVE_PAYLOAD_IDS, feature->receive_pld) != KTAG_SUCCESS ||
        set_statistics(output, RECEIVE_INTERVAL_IDS, feature->receive_interval) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    return KTAG_SUCCESS;
}

ktag_status rawbow_finish(
    void *,
    void *,
    const ktag_flow_view *flow,
    ktag_feature_output *output)
{
    const stream_feature *feature = nullptr;
    if (ktag_flow_view_get_stream_feature_internal(flow, &feature) != KTAG_SUCCESS) {
        return KTAG_ERROR_INTERNAL;
    }
    float histogram[RAW_BOW_HISTOGRAM_NUMS];
    for (uint32_t index = 0; index < RAW_BOW_HISTOGRAM_NUMS; ++index) {
        histogram[index] = static_cast<float>(feature->raw_bow_crc_result[index]);
    }
    return set_optional(output, "rawbow.crc_histogram", histogram, RAW_BOW_HISTOGRAM_NUMS);
}

ktag_feature_callbacks make_callbacks(
    ktag_status (*finish)(void *, void *, const ktag_flow_view *, ktag_feature_output *))
{
    ktag_feature_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.api_version = KTAG_API_VERSION;
    callbacks.flow_finish = finish;
    return callbacks;
}

ktag_feature_callbacks callbacks_for_provider(const std::string &provider_id)
{
    if (provider_id == "builtin.legacy.metadata") {
        return make_callbacks(metadata_finish);
    }
    if (provider_id == "builtin.legacy.tuple") {
        return make_callbacks(tuple_finish);
    }
    if (provider_id == "builtin.legacy.traffic_statistics") {
        return make_callbacks(traffic_finish);
    }
    if (provider_id == "builtin.legacy.rawbow") {
        return make_callbacks(rawbow_finish);
    }
    return {};
}

} // namespace

extern "C" {

KTAG_API ktag_status ktag_feature_registry_register_legacy_builtins(
    ktag_feature_registry *registry)
{
    if (registry == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    std::unique_ptr<ktag_feature_schema, decltype(&ktag_feature_schema_destroy)> schema(
        ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
    if (!schema) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    }

    std::vector<std::string> registered;
    const auto rollback = [&registered, registry]() {
        for (auto provider = registered.rbegin(); provider != registered.rend(); ++provider) {
            (void)ktag_feature_registry_unregister(registry, provider->c_str());
        }
    };

    try {
        std::map<std::string, std::vector<ktag_feature_descriptor>> grouped;
        for (uint32_t index = 0; index < ktag_feature_schema_get_feature_count(schema.get()); ++index) {
            ktag_feature_descriptor descriptor{};
            uint32_t offset = 0;
            if (ktag_feature_schema_get_descriptor(schema.get(), index, &descriptor, &offset) != KTAG_SUCCESS) {
                return KTAG_ERROR_INTERNAL;
            }
            grouped[descriptor.provider_id].push_back(descriptor);
        }

        registered.reserve(grouped.size());
        for (auto &group : grouped) {
            ktag_feature_provider_registration registration{};
            registration.struct_size = sizeof(registration);
            registration.api_version = KTAG_API_VERSION;
            registration.provider_id = group.first.c_str();
            registration.provider_version = 1;
            registration.features = group.second.data();
            registration.feature_count = static_cast<uint32_t>(group.second.size());
            registration.callbacks = callbacks_for_provider(group.first);
            const ktag_status status = ktag_feature_registry_register(
                registry, &registration, KTAG_FEATURE_REGISTER_DEFAULT);
            if (status != KTAG_SUCCESS) {
                rollback();
                return status;
            }
            registered.push_back(group.first);
        }
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        rollback();
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        rollback();
        return KTAG_ERROR_INTERNAL;
    }
}

} // extern "C"

ktag_feature_pipeline *ktag_feature_legacy_pipeline_create_internal(void)
{
    std::unique_ptr<ktag_feature_registry, decltype(&ktag_feature_registry_destroy)> registry(
        ktag_feature_registry_create(), ktag_feature_registry_destroy);
    std::unique_ptr<ktag_feature_schema, decltype(&ktag_feature_schema_destroy)> schema(
        ktag_feature_schema_create_legacy_v1(), ktag_feature_schema_destroy);
    if (!registry || !schema ||
        ktag_feature_registry_register_legacy_builtins(registry.get()) != KTAG_SUCCESS) {
        return nullptr;
    }
    return ktag_feature_pipeline_create_internal(schema.get(), registry.get());
}
