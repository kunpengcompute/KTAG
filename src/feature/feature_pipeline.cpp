/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "feature_pipeline_internal.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <unordered_set>
#include <utility>

namespace {

ktag_feature_schema *clone_schema(const ktag_feature_schema *source) {
    if (source == nullptr || !source->frozen) {
        return nullptr;
    }
    std::unique_ptr<ktag_feature_schema, decltype(&ktag_feature_schema_destroy)>
        clone(ktag_feature_schema_create(source->name.c_str(), source->version),
              ktag_feature_schema_destroy);
    if (!clone) {
        return nullptr;
    }
    for (const auto &item : source->descriptors) {
        ktag_feature_descriptor descriptor{};
        ktag_feature_copy_descriptor(item, &descriptor);
        if (ktag_feature_schema_add_contract(
                clone.get(), &descriptor, item.contract_provider_id.c_str()) !=
            KTAG_SUCCESS) {
            return nullptr;
        }
    }
    if (ktag_feature_schema_freeze(clone.get()) != KTAG_SUCCESS ||
        ktag_feature_schema_get_hash(clone.get()) !=
            ktag_feature_schema_get_hash(source)) {
        return nullptr;
    }
    return clone.release();
}

bool descriptor_matches(
    const ktag_feature_descriptor_storage &schema_feature,
    const ktag_feature_descriptor_storage &registered_feature) {
    return schema_feature.feature_id == registered_feature.feature_id &&
           schema_feature.provider_id == registered_feature.provider_id &&
           schema_feature.provider_version ==
               registered_feature.provider_version &&
           schema_feature.value_type == registered_feature.value_type &&
           schema_feature.dimension == registered_feature.dimension;
}

void destroy_created_providers(ktag_feature_pipeline *pipeline) {
    if (pipeline == nullptr) {
        return;
    }
    for (auto provider = pipeline->providers.rbegin();
         provider != pipeline->providers.rend(); ++provider) {
        if (provider->provider_created &&
            provider->registration.callbacks.provider_destroy != nullptr) {
            provider->registration.callbacks.provider_destroy(
                provider->provider_state);
        }
        provider->provider_state = nullptr;
        provider->provider_created = false;
    }
    pipeline->providers.clear();
}

void destroy_flow_states(ktag_feature_flow_runtime *runtime) {
    if (runtime == nullptr || runtime->pipeline == nullptr) {
        return;
    }
    const size_t count = std::min(runtime->flow_states.size(),
                                  runtime->pipeline->providers.size());
    for (size_t offset = count; offset > 0; --offset) {
        auto &provider = runtime->pipeline->providers[offset - 1];
        if (offset <= runtime->flow_state_created.size() &&
            runtime->flow_state_created[offset - 1] != 0U &&
            provider.registration.callbacks.flow_destroy != nullptr) {
            provider.registration.callbacks.flow_destroy(
                provider.provider_state, runtime->flow_states[offset - 1]);
        }
    }
    runtime->flow_states.clear();
    runtime->flow_state_created.clear();
    runtime->provider_failed.clear();
    runtime->active = false;
}

bool should_fill_zero(const ktag_feature_pipeline_provider &provider) {
    return provider.registration.callback_error_policy ==
           KTAG_FEATURE_CALLBACK_ERROR_FILL_ZERO;
}

void fill_provider_zero(const ktag_feature_pipeline_provider &provider,
                        ktag_feature_output *writer) {
    for (size_t index = 0; index < writer->schema->descriptors.size();
         ++index) {
        const auto &descriptor = writer->schema->descriptors[index];
        if (descriptor.provider_id != provider.registration.provider_id) {
            continue;
        }
        std::fill_n(writer->values + descriptor.value_offset,
                    descriptor.dimension, 0.0F);
        (*writer->written)[index] = 1U;
    }
}

bool provider_outputs_complete(const ktag_feature_pipeline_provider &provider,
                               const ktag_feature_output &writer) {
    for (size_t index = 0; index < writer.schema->descriptors.size(); ++index) {
        if (writer.schema->descriptors[index].provider_id ==
                provider.registration.provider_id &&
            (*writer.written)[index] == 0U) {
            return false;
        }
    }
    return true;
}

void *flow_state_at(ktag_feature_flow_runtime *runtime, size_t index) {
    return index < runtime->flow_states.size() ? runtime->flow_states[index]
                                               : nullptr;
}

bool provider_failed_at(const ktag_feature_flow_runtime *runtime,
                        size_t index) {
    return index < runtime->provider_failed.size() &&
           runtime->provider_failed[index] != 0U;
}

} // namespace

ktag_feature_pipeline *
ktag_feature_pipeline_create_internal(const ktag_feature_schema *schema,
                                      const ktag_feature_registry *registry) {
    if (schema == nullptr || registry == nullptr || !schema->frozen) {
        return nullptr;
    }
    try {
        auto pipeline_deleter = [](ktag_feature_pipeline *value) {
            if (value != nullptr) {
                destroy_created_providers(value);
                ktag_feature_schema_destroy(value->schema);
                delete value;
            }
        };
        std::unique_ptr<ktag_feature_pipeline, decltype(pipeline_deleter)>
            pipeline(new (std::nothrow) ktag_feature_pipeline(),
                     pipeline_deleter);
        if (!pipeline) {
            return nullptr;
        }
        pipeline->schema = clone_schema(schema);
        if (pipeline->schema == nullptr) {
            return nullptr;
        }
        pipeline->written_scratch.resize(pipeline->schema->descriptors.size(),
                                         0U);

        std::unordered_set<std::string> added_providers;
        for (const auto &schema_feature : pipeline->schema->descriptors) {
            const ktag_feature_provider_storage *registered_provider = nullptr;
            const auto *registered_feature =
                ktag_feature_registry_find_feature_internal(
                    registry, schema_feature.feature_id, &registered_provider);
            if (registered_feature == nullptr ||
                registered_provider == nullptr ||
                !descriptor_matches(schema_feature, *registered_feature)) {
                return nullptr;
            }
            if (added_providers.insert(registered_provider->provider_id)
                    .second) {
                ktag_feature_pipeline_provider provider;
                provider.registration = *registered_provider;
                pipeline->providers.push_back(std::move(provider));
            }
        }

        for (auto &provider : pipeline->providers) {
            if (provider.registration.callbacks.provider_create == nullptr) {
                continue;
            }
            ktag_feature_provider_config config{};
            config.struct_size = sizeof(config);
            config.api_version = KTAG_API_VERSION;
            config.provider_id = provider.registration.provider_id.c_str();
            config.provider_version = provider.registration.provider_version;
            config.schema = pipeline->schema;
            config.user_data = provider.registration.user_data;
            if (provider.registration.callbacks.provider_create(
                    &config, &provider.provider_state) != KTAG_SUCCESS) {
                provider.provider_state = nullptr;
                if (!should_fill_zero(provider)) {
                    return nullptr;
                }
                provider.available = false;
                continue;
            }
            provider.provider_created = true;
        }
        pipeline->requires_flow_state_storage = std::any_of(
            pipeline->providers.cbegin(), pipeline->providers.cend(),
            [](const ktag_feature_pipeline_provider &provider) {
                return !provider.available || should_fill_zero(provider) ||
                       provider.registration.callbacks.flow_create != nullptr;
            });
        return pipeline.release();
    } catch (...) {
        return nullptr;
    }
}

void ktag_feature_pipeline_destroy_internal(ktag_feature_pipeline *pipeline) {
    if (pipeline == nullptr) {
        return;
    }
    destroy_created_providers(pipeline);
    ktag_feature_schema_destroy(pipeline->schema);
    pipeline->schema = nullptr;
    delete pipeline;
}

ktag_status
ktag_feature_flow_runtime_create_internal(ktag_feature_pipeline *pipeline,
                                          uint64_t flow_id,
                                          ktag_feature_flow_runtime *runtime) {
    return ktag_feature_flow_runtime_create_with_key_internal(
        pipeline, flow_id, nullptr, nullptr, 0U, nullptr, runtime);
}

ktag_status ktag_feature_flow_runtime_create_with_key_internal(
    ktag_feature_pipeline *pipeline, uint64_t flow_id,
    const ktag_flow_key *flow_key, const ktag_ip_address *forward_address,
    uint16_t forward_port,
    const ktag_flow_direction_summary *initial_directions,
    ktag_feature_flow_runtime *runtime) {
    if (pipeline == nullptr || runtime == nullptr || runtime->active) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    try {
        runtime->pipeline = pipeline;
        runtime->flow.flow_id = flow_id;
        runtime->flow.has_flow_key =
            flow_key != nullptr && forward_address != nullptr;
        runtime->flow.directions[0U] = initial_directions == nullptr
                                           ? ktag_flow_direction_summary{}
                                           : initial_directions[0U];
        runtime->flow.directions[1U] = initial_directions == nullptr
                                           ? ktag_flow_direction_summary{}
                                           : initial_directions[1U];
        if (runtime->flow.has_flow_key) {
            runtime->flow.flow_key = *flow_key;
            runtime->flow.forward_address = *forward_address;
            runtime->flow.forward_port = forward_port;
        } else {
            runtime->flow.flow_key = {};
            runtime->flow.forward_address = {};
            runtime->flow.forward_port = 0U;
        }
        runtime->flow_states.clear();
        runtime->flow_state_created.clear();
        runtime->provider_failed.clear();
        runtime->annotations.clear();
        runtime->active = true;
        if (!pipeline->requires_flow_state_storage) {
            return KTAG_SUCCESS;
        }
        runtime->flow_states.reserve(pipeline->providers.size());
        runtime->flow_state_created.reserve(pipeline->providers.size());
        runtime->provider_failed.reserve(pipeline->providers.size());
        for (size_t index = 0; index < pipeline->providers.size(); ++index) {
            auto &provider = pipeline->providers[index];
            void *flow_state = nullptr;
            bool flow_state_created = false;
            bool provider_failed = !provider.available;
            if (!provider_failed &&
                provider.registration.callbacks.flow_create != nullptr) {
                if (provider.registration.callbacks.flow_create(
                        provider.provider_state, &runtime->flow, &flow_state) !=
                    KTAG_SUCCESS) {
                    flow_state = nullptr;
                    provider_failed = true;
                    if (!should_fill_zero(provider)) {
                        destroy_flow_states(runtime);
                        return KTAG_ERROR_INTERNAL;
                    }
                } else {
                    flow_state_created = true;
                }
            }
            runtime->flow_states.push_back(flow_state);
            runtime->flow_state_created.push_back(flow_state_created ? 1U : 0U);
            runtime->provider_failed.push_back(provider_failed ? 1U : 0U);
        }
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        destroy_flow_states(runtime);
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        destroy_flow_states(runtime);
        return KTAG_ERROR_INTERNAL;
    }
}

ktag_status ktag_feature_flow_runtime_on_packet_internal(
    ktag_feature_flow_runtime *runtime, const uint8_t *packet,
    uint32_t packet_length, const ktag_timestamp *timestamp) {
    return ktag_feature_flow_runtime_on_packet_view_internal(
        runtime, packet, packet_length, timestamp, nullptr, 0U, 0U, nullptr);
}

ktag_status ktag_feature_flow_runtime_on_packet_view_internal(
    ktag_feature_flow_runtime *runtime, const uint8_t *packet,
    uint32_t packet_length, const ktag_timestamp *timestamp,
    const uint8_t *payload, uint32_t payload_length,
    uint32_t captured_header_length,
    const ktag_packet_transport_view *transport) {
    if (runtime == nullptr || !runtime->active || packet == nullptr ||
        timestamp == nullptr || (payload == nullptr && payload_length != 0U) ||
        captured_header_length > packet_length ||
        (transport != nullptr &&
         (transport->struct_size < sizeof(*transport) ||
          transport->api_version != KTAG_API_VERSION))) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    ktag_packet_view view;
    view.data = packet;
    view.length = packet_length;
    view.payload = payload;
    view.payload_length = payload_length;
    view.captured_header_length = captured_header_length;
    view.timestamp = *timestamp;
    view.transport.struct_size = sizeof(view.transport);
    view.transport.api_version = KTAG_API_VERSION;
    if (transport != nullptr) {
        view.transport = *transport;
        view.transport.struct_size = sizeof(view.transport);
        view.transport.api_version = KTAG_API_VERSION;
    }
    try {
        for (size_t index = 0; index < runtime->pipeline->providers.size();
             ++index) {
            auto &provider = runtime->pipeline->providers[index];
            if (provider_failed_at(runtime, index)) {
                continue;
            }
            if (provider.registration.callbacks.on_packet != nullptr &&
                provider.registration.callbacks.on_packet(
                    provider.provider_state, flow_state_at(runtime, index),
                    &view, &runtime->flow) != KTAG_SUCCESS) {
                if (!should_fill_zero(provider)) {
                    return KTAG_ERROR_INTERNAL;
                }
                runtime->provider_failed[index] = 1U;
            }
        }
        return KTAG_SUCCESS;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

ktag_status
ktag_feature_flow_runtime_finish_internal(ktag_feature_flow_runtime *runtime,
                                          float *output,
                                          uint32_t output_capacity) {
    if (runtime == nullptr || !runtime->active || output == nullptr ||
        output_capacity < runtime->pipeline->schema->value_count) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::memset(output, 0,
                    runtime->pipeline->schema->value_count * sizeof(float));
        auto &written = runtime->pipeline->written_scratch;
        std::fill(written.begin(), written.end(), static_cast<uint8_t>(0U));
        ktag_feature_output writer;
        writer.schema = runtime->pipeline->schema;
        writer.values = output;
        writer.value_capacity = output_capacity;
        writer.written = &written;
        writer.annotations = runtime->pipeline->collect_annotations
                                 ? &runtime->annotations
                                 : nullptr;

        ktag_status result = KTAG_SUCCESS;
        for (size_t index = 0; index < runtime->pipeline->providers.size();
             ++index) {
            auto &provider = runtime->pipeline->providers[index];
            writer.current_provider_id = &provider.registration.provider_id;
            if (provider_failed_at(runtime, index)) {
                fill_provider_zero(provider, &writer);
                continue;
            }
            std::unique_ptr<std::unordered_map<std::string, std::string>>
                annotations_before;
            if (writer.annotations != nullptr) {
                annotations_before = std::make_unique<
                    std::unordered_map<std::string, std::string>>(
                    *writer.annotations);
            }
            const ktag_status callback_status =
                provider.registration.callbacks.flow_finish(
                    provider.provider_state, flow_state_at(runtime, index),
                    &runtime->flow, &writer);
            if (callback_status != KTAG_SUCCESS ||
                !provider_outputs_complete(provider, writer)) {
                if (writer.annotations != nullptr) {
                    *writer.annotations = std::move(*annotations_before);
                }
                if (should_fill_zero(provider)) {
                    fill_provider_zero(provider, &writer);
                    continue;
                }
                result = callback_status == KTAG_SUCCESS
                             ? KTAG_ERROR_INVALID_STATE
                             : KTAG_ERROR_INTERNAL;
                break;
            }
        }
        if (result == KTAG_SUCCESS &&
            std::find(written.cbegin(), written.cend(),
                      static_cast<uint8_t>(0U)) != written.cend()) {
            result = KTAG_ERROR_INVALID_STATE;
        }
        destroy_flow_states(runtime);
        return result;
    } catch (const std::bad_alloc &) {
        destroy_flow_states(runtime);
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        destroy_flow_states(runtime);
        return KTAG_ERROR_INTERNAL;
    }
}

void ktag_feature_flow_runtime_destroy_internal(
    ktag_feature_flow_runtime *runtime) {
    destroy_flow_states(runtime);
    if (runtime != nullptr) {
        runtime->annotations.clear();
        runtime->pipeline = nullptr;
        runtime->flow = {};
    }
}

extern "C" {

KTAG_API ktag_status ktag_feature_output_set(ktag_feature_output *output,
                                             const char *feature_id,
                                             const float *values,
                                             uint32_t value_count) {
    if (output == nullptr || output->schema == nullptr ||
        output->current_provider_id == nullptr || output->values == nullptr ||
        output->written == nullptr || feature_id == nullptr ||
        values == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const auto position =
        output->schema->feature_indices.find(std::string_view(feature_id));
    if (position == output->schema->feature_indices.cend()) {
        return KTAG_ERROR_NOT_FOUND;
    }
    const size_t descriptor_index = position->second;
    const auto &descriptor = output->schema->descriptors[descriptor_index];
    if (descriptor.provider_id != *output->current_provider_id ||
        descriptor.dimension != value_count) {
        return KTAG_ERROR_SCHEMA_MISMATCH;
    }
    if (descriptor.value_offset > output->value_capacity ||
        descriptor.dimension >
            output->value_capacity - descriptor.value_offset) {
        return KTAG_ERROR_CAPACITY;
    }
    if ((*output->written)[descriptor_index] != 0U) {
        return KTAG_ERROR_INVALID_STATE;
    }
    std::copy(values, values + value_count,
              output->values + descriptor.value_offset);
    (*output->written)[descriptor_index] = 1U;
    return KTAG_SUCCESS;
}

KTAG_API uint32_t
ktag_feature_output_annotations_enabled(const ktag_feature_output *output) {
    return output != nullptr && output->annotations != nullptr ? 1U : 0U;
}

KTAG_API ktag_status ktag_feature_output_set_annotation(
    ktag_feature_output *output, const char *key, const char *value) {
    if (output == nullptr || output->current_provider_id == nullptr ||
        key == nullptr || value == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (output->annotations == nullptr) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }
    const size_t key_length = std::strlen(key);
    const size_t value_length = std::strlen(value);
    if (key_length == 0U ||
        key_length > KTAG_FEATURE_ANNOTATION_KEY_MAX_LENGTH ||
        value_length > KTAG_FEATURE_ANNOTATION_VALUE_MAX_LENGTH) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto inserted = output->annotations->emplace(key, value);
        return inserted.second ? KTAG_SUCCESS : KTAG_ERROR_INVALID_STATE;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

} // extern "C"
