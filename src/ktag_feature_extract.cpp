/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "ktag_feature_extract.h"
#include "ktag_export.h"
#include "ktag_feature_plugin.h"
#include "ktag_traffic_features.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

static_assert(
    KTAG_LEGACY_V1_VALUE_COUNT == FEATURES_NUM - 1,
    "Traffic Provider output must match the fixed-vector public contract");

namespace {

thread_local std::string extractor_last_error;
const int legacy_plugin_path_anchor = 0;

void set_last_error(const std::string &message) {
    extractor_last_error = message;
}

bool default_plugin_path(std::string *path) {
    const char *environment = std::getenv(KTAG_LEGACY_FEATURE_PLUGIN_ENV);
    if (environment != nullptr && environment[0] != '\0') {
        *path = environment;
        return true;
    }

    Dl_info library_info{};
    if (dladdr(&legacy_plugin_path_anchor, &library_info) == 0 ||
        library_info.dli_fname == nullptr) {
        set_last_error("cannot locate libktag_feature_compat to resolve "
                       "the default Feature Plugin path");
        return false;
    }
    const std::string library_path(library_info.dli_fname);
    const size_t separator = library_path.find_last_of('/');
    if (separator == std::string::npos) {
        set_last_error("libktag_feature_compat has no absolute directory; "
                       "set " KTAG_LEGACY_FEATURE_PLUGIN_ENV);
        return false;
    }
    *path = library_path.substr(0U, separator + 1U) +
            "plugins/libktag_traffic_features.so";
    return true;
}

struct feature_runtime {
    ktag_feature_context *context = nullptr;
    ktag_feature_batch *batch = nullptr;
    ktag_loaded_feature_plugin *plugin = nullptr;
    uint64_t next_packet_sequence = 1U;
    ktag_feature_statistics statistics_baseline{};
};

feature_runtime *runtime_of(feature_extractor *extractor) {
    return extractor == nullptr ? nullptr
                                : static_cast<feature_runtime *>(
                                      extractor->legacy_feature_pipeline);
}

const feature_runtime *runtime_of(const feature_extractor *extractor) {
    return extractor == nullptr ? nullptr
                                : static_cast<const feature_runtime *>(
                                      extractor->legacy_feature_pipeline);
}

void destroy_runtime(feature_runtime *runtime) {
    if (runtime == nullptr) {
        return;
    }
    ktag_feature_batch_destroy(runtime->batch);
    ktag_feature_destroy(runtime->context);
    ktag_feature_plugin_unload(runtime->plugin);
    delete runtime;
}

feature_runtime *create_runtime(ktag_domain_store *domain_store,
                                const char *plugin_path) {
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    if (registry == nullptr || schema == nullptr) {
        set_last_error(
            "failed to create the legacy Feature Registry or Schema");
        ktag_feature_schema_destroy(schema);
        ktag_feature_registry_destroy(registry);
        return nullptr;
    }

    std::string resolved_path;
    if (plugin_path != nullptr && plugin_path[0] != '\0') {
        resolved_path = plugin_path;
    } else if (!default_plugin_path(&resolved_path)) {
        ktag_feature_schema_destroy(schema);
        ktag_feature_registry_destroy(registry);
        return nullptr;
    }
    ktag_loaded_feature_plugin *plugin =
        ktag_feature_plugin_load(resolved_path.c_str(), registry);
    if (plugin == nullptr) {
        set_last_error(
            std::string("failed to initialize legacy Feature Plugin: ") +
            ktag_feature_plugin_get_last_error());
        ktag_feature_schema_destroy(schema);
        ktag_feature_registry_destroy(registry);
        return nullptr;
    }

    feature_runtime *runtime = new (std::nothrow) feature_runtime();
    if (runtime == nullptr) {
        set_last_error("out of memory creating the legacy Feature runtime");
        ktag_feature_schema_destroy(schema);
        ktag_feature_registry_destroy(registry);
        ktag_feature_plugin_unload(plugin);
        return nullptr;
    }
    runtime->plugin = plugin;

    runtime->batch = ktag_feature_batch_create(schema, 1U);
    ktag_feature_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.schema = schema;
    config.registry = registry;
    config.shared_domain_store = domain_store;
    config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
    config.collect_annotations = 1U;
    runtime->context = ktag_feature_create(&config);

    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);

    if (runtime->batch == nullptr || runtime->context == nullptr) {
        set_last_error("the dynamically loaded Providers do not satisfy the "
                       "legacy Feature contract");
        destroy_runtime(runtime);
        return nullptr;
    }
    runtime->statistics_baseline.struct_size =
        sizeof(runtime->statistics_baseline);
    runtime->statistics_baseline.api_version = KTAG_API_VERSION;
    return runtime;
}

bool valid_extractor(const feature_extractor *extractor) {
    const feature_runtime *runtime = runtime_of(extractor);
    return extractor != nullptr && extractor->initialized != 0 &&
           runtime != nullptr && runtime->context != nullptr &&
           runtime->batch != nullptr;
}

int copy_record(const ktag_feature_batch *batch, uint32_t index,
                feature_vector *features, rawbow *rawbows,
                bool extract_rawbow) {
    if (batch == nullptr || features == nullptr) {
        return EXTRACTOR_ERROR;
    }

    ktag_feature_record record{};
    if (ktag_feature_batch_get_record(batch, index, &record) != KTAG_SUCCESS ||
        record.value_count != KTAG_LEGACY_V1_VALUE_COUNT) {
        return EXTRACTOR_ERROR;
    }

    std::memset(features, 0, sizeof(*features));
    std::memcpy(features->features, record.values,
                sizeof(float) * record.value_count);

    if (!extract_rawbow) {
        return EXTRACTOR_SUCCESS;
    }
    if (rawbows == nullptr) {
        return EXTRACTOR_ERROR;
    }

    std::memset(rawbows, 0, sizeof(*rawbows));
    const char *text = nullptr;
    const ktag_status annotation_status = ktag_feature_batch_get_annotation(
        batch, index, KTAG_TRAFFIC_RAWBOW_ANNOTATION, &text);
    if (annotation_status == KTAG_ERROR_NOT_FOUND) {
        return EXTRACTOR_SUCCESS;
    }
    if (annotation_status != KTAG_SUCCESS || text == nullptr) {
        return EXTRACTOR_ERROR;
    }

    const size_t length =
        std::min(std::strlen(text), static_cast<size_t>(RAWBOW_MAX_LEN - 1U));
    std::memcpy(rawbows->rawbow, text, length);
    rawbows->rawbow[length] = '\0';
    rawbows->len = static_cast<uint32_t>(length);
    return EXTRACTOR_SUCCESS;
}

uint64_t counter_delta(uint64_t value, uint64_t baseline) {
    return value >= baseline ? value - baseline : 0U;
}

uint32_t narrow_counter(uint64_t value) {
    return static_cast<uint32_t>(
        std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
}

int refresh_public_statistics(feature_extractor *extractor) {
    feature_runtime *runtime = runtime_of(extractor);
    if (runtime == nullptr) {
        return EXTRACTOR_ERROR;
    }

    ktag_feature_statistics current{};
    current.struct_size = sizeof(current);
    current.api_version = KTAG_API_VERSION;
    if (ktag_feature_get_statistics(runtime->context, &current) !=
        KTAG_SUCCESS) {
        return EXTRACTOR_ERROR;
    }

    const auto &baseline = runtime->statistics_baseline;
    extractor->stats.total_packets =
        counter_delta(current.received_packets, baseline.received_packets);
    extractor->stats.total_bytes =
        counter_delta(current.received_bytes, baseline.received_bytes);
    extractor->stats.decoded_packets = narrow_counter(
        counter_delta(current.decoded_packets, baseline.decoded_packets));
    extractor->stats.extracted_flows = narrow_counter(
        counter_delta(current.emitted_flows, baseline.emitted_flows));
    return EXTRACTOR_SUCCESS;
}

feature_extractor *extractor_init_internal(int link_type,
                                           ktag_domain_store *domain_store,
                                           const char *plugin_path) {
    set_last_error(std::string());
    if (link_type != DLT_EN10MB && link_type != DLT_LINUX_SLL) {
        set_last_error("unsupported link type");
        std::fprintf(stderr, "Error: Unsupported link type %d\n", link_type);
        return nullptr;
    }

    feature_extractor *extractor = static_cast<feature_extractor *>(
        std::calloc(1U, sizeof(feature_extractor)));
    if (extractor == nullptr) {
        set_last_error("out of memory creating the legacy Feature Extractor");
        return nullptr;
    }

    feature_runtime *runtime = create_runtime(domain_store, plugin_path);
    if (runtime == nullptr) {
        std::free(extractor);
        return nullptr;
    }

    extractor->link_type = link_type;
    extractor->initialized = 1;
    extractor->next_flow_id = 1U;
    extractor->legacy_feature_pipeline = runtime;
    return extractor;
}

int process_packet_internal(feature_extractor *extractor,
                            const unsigned char *packet, uint32_t packet_len,
                            const timeval *timestamp, feature_vector *features,
                            rawbow *rawbows, int *has_feature,
                            bool extract_rawbow) {
    if (!valid_extractor(extractor) || packet == nullptr || packet_len == 0U ||
        timestamp == nullptr || timestamp->tv_sec < 0 ||
        timestamp->tv_usec < 0 || timestamp->tv_usec > 999999 ||
        features == nullptr || has_feature == nullptr ||
        (extract_rawbow && rawbows == nullptr)) {
        return EXTRACTOR_ERROR;
    }

    *has_feature = 0;
    std::memset(features, 0, sizeof(*features));
    if (extract_rawbow) {
        std::memset(rawbows, 0, sizeof(*rawbows));
    }

    feature_runtime *runtime = runtime_of(extractor);
    if (runtime->next_packet_sequence == std::numeric_limits<uint64_t>::max()) {
        return EXTRACTOR_ERROR;
    }

    ktag_packet packet_view{};
    packet_view.struct_size = sizeof(packet_view);
    packet_view.api_version = KTAG_API_VERSION;
    packet_view.data = packet;
    packet_view.captured_length = packet_len;
    packet_view.original_length = packet_len;
    packet_view.link_type = static_cast<uint32_t>(extractor->link_type);
    packet_view.sequence = runtime->next_packet_sequence++;
    packet_view.timestamp.seconds = timestamp->tv_sec;
    packet_view.timestamp.microseconds =
        static_cast<int32_t>(timestamp->tv_usec);

    const ktag_status status = ktag_feature_process_packet(
        runtime->context, &packet_view, runtime->batch);
    if (status != KTAG_SUCCESS) {
        return EXTRACTOR_ERROR;
    }

    const uint32_t count = ktag_feature_batch_get_count(runtime->batch);
    if (count == 0U) {
        return EXTRACTOR_SUCCESS;
    }
    if (count != 1U || copy_record(runtime->batch, 0U, features, rawbows,
                                   extract_rawbow) != EXTRACTOR_SUCCESS) {
        return EXTRACTOR_ERROR;
    }

    extractor->next_flow_id = static_cast<uint64_t>(features->features[0]) + 1U;
    *has_feature = 1;
    return EXTRACTOR_SUCCESS;
}

int finalize_internal(feature_extractor *extractor,
                      feature_vector_list *features, rawbow_list *rawbows,
                      bool extract_rawbow) {
    if (!valid_extractor(extractor) || features == nullptr ||
        features->vectors == nullptr || features->capacity == 0U ||
        (extract_rawbow && (rawbows == nullptr || rawbows->rawbows == nullptr ||
                            rawbows->capacity == 0U))) {
        return EXTRACTOR_ERROR;
    }

    features->count = 0U;
    if (extract_rawbow) {
        rawbows->count = 0U;
    }

    feature_runtime *runtime = runtime_of(extractor);
    if (ktag_feature_flush(runtime->context, runtime->batch) != KTAG_SUCCESS) {
        return EXTRACTOR_ERROR;
    }

    const uint32_t available = ktag_feature_batch_get_count(runtime->batch);
    uint32_t capacity = features->capacity;
    if (extract_rawbow) {
        capacity = std::min(capacity, rawbows->capacity);
    }
    const uint32_t copied = std::min(available, capacity);
    for (uint32_t index = 0U; index < copied; ++index) {
        rawbow *rawbow_output =
            extract_rawbow ? &rawbows->rawbows[index] : nullptr;
        if (copy_record(runtime->batch, index, &features->vectors[index],
                        rawbow_output, extract_rawbow) != EXTRACTOR_SUCCESS) {
            return EXTRACTOR_ERROR;
        }
        ++features->count;
        if (extract_rawbow) {
            ++rawbows->count;
        }
    }

    if (copied != 0U) {
        extractor->next_flow_id =
            static_cast<uint64_t>(features->vectors[copied - 1U].features[0]) +
            1U;
    }
    if (available > capacity) {
        std::fprintf(stderr,
                     "Error: feature output capacity exceeded "
                     "(required=%u, capacity=%u)\n",
                     available, capacity);
        return EXTRACTOR_ERROR;
    }
    return EXTRACTOR_SUCCESS;
}

} // namespace

extern "C" {

KTAG_API feature_extractor *extractor_init(int link_type) {
    return extractor_init_internal(link_type, nullptr, nullptr);
}

KTAG_API feature_extractor *
extractor_init_with_config(const ktag_feature_extractor_config *config) {
    if (config == nullptr || config->struct_size < sizeof(*config) ||
        config->api_version != KTAG_API_VERSION) {
        set_last_error("invalid legacy Feature Extractor configuration");
        return nullptr;
    }
    return extractor_init_internal(config->link_type,
                                   config->shared_domain_store,
                                   config->feature_plugin_path);
}

KTAG_API feature_extractor *
extractor_init_with_domain_store(int link_type,
                                 ktag_domain_store *domain_store) {
    if (domain_store == nullptr) {
        set_last_error("a shared Domain Store is required");
        return nullptr;
    }
    return extractor_init_internal(link_type, domain_store, nullptr);
}

KTAG_API const char *extractor_get_last_error(void) {
    return extractor_last_error.c_str();
}

KTAG_API int process_packet(feature_extractor *extractor,
                             const unsigned char *packet, uint32_t packet_len,
                             const timeval *timestamp, feature_vector *features,
                             int *has_feature) {
    return process_packet_internal(extractor, packet, packet_len, timestamp,
                                   features, nullptr, has_feature, false);
}

KTAG_API int process_packet_with_rawbow(feature_extractor *extractor,
                                         const unsigned char *packet,
                                         uint32_t packet_len,
                                         const timeval *timestamp,
                                         feature_vector *features,
                                         rawbow *rawbows, int *has_feature) {
    return process_packet_internal(extractor, packet, packet_len, timestamp,
                                   features, rawbows, has_feature, true);
}

KTAG_API int extractor_finalize(feature_extractor *extractor,
                                 feature_vector_list *features) {
    return finalize_internal(extractor, features, nullptr, false);
}

KTAG_API int extractor_finalize_with_rawbow(feature_extractor *extractor,
                                             feature_vector_list *features,
                                             rawbow_list *rawbows) {
    return finalize_internal(extractor, features, rawbows, true);
}

KTAG_API int extractor_get_statistics(feature_extractor *extractor,
                                       statistics *stats) {
    if (!valid_extractor(extractor) || stats == nullptr ||
        refresh_public_statistics(extractor) != EXTRACTOR_SUCCESS) {
        return EXTRACTOR_ERROR;
    }
    *stats = extractor->stats;
    return EXTRACTOR_SUCCESS;
}

KTAG_API void extractor_reset_statistics(feature_extractor *extractor) {
    if (!valid_extractor(extractor)) {
        return;
    }
    feature_runtime *runtime = runtime_of(extractor);
    ktag_feature_statistics current{};
    current.struct_size = sizeof(current);
    current.api_version = KTAG_API_VERSION;
    if (ktag_feature_get_statistics(runtime->context, &current) ==
        KTAG_SUCCESS) {
        runtime->statistics_baseline = current;
        std::memset(&extractor->stats, 0, sizeof(extractor->stats));
    }
}

KTAG_API void extractor_destroy(feature_extractor *extractor) {
    if (extractor == nullptr) {
        return;
    }
    destroy_runtime(runtime_of(extractor));
    extractor->legacy_feature_pipeline = nullptr;
    extractor->initialized = 0;
    std::free(extractor);
}

} // extern "C"
