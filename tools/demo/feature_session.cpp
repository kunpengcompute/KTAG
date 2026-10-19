/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#include "feature_session.h"
#include "ktag_traffic_features.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <new>
#include <string>
namespace {
std::string plugin_location() {
    if (const char *value = std::getenv("KTAG_LEGACY_FEATURE_PLUGIN")) {
        if (*value) return value;
    }
    Dl_info info{};
    void *entry = dlsym(RTLD_DEFAULT, "ktag_feature_create");
    if (entry == nullptr || dladdr(entry, &info) == 0 ||
        info.dli_fname == nullptr) return {};
    const std::string library(info.dli_fname);
    const auto slash = library.find_last_of('/');
    if (slash == std::string::npos) return {};
    return library.substr(0, slash + 1) + "plugins/libktag_traffic_features.so";
}
}
ktag_demo_feature_session *ktag_demo_feature_session::create(
    int link_type, ktag_domain_store *store, const char *plugin_path) {
    std::unique_ptr<ktag_demo_feature_session> session(new (std::nothrow) ktag_demo_feature_session());
    if (!session) return nullptr;
    auto *registry = ktag_feature_registry_create();
    auto *schema = ktag_feature_schema_create_legacy_v1();
    if (!registry || !schema) {
        ktag_feature_registry_destroy(registry);
        ktag_feature_schema_destroy(schema);
        return nullptr;
    }
    const std::string path = plugin_path && *plugin_path ? plugin_path : plugin_location();
    session->plugin_ = ktag_feature_plugin_load(path.c_str(), registry);
    if (session->plugin_) {
        session->batch_ = ktag_feature_batch_create(schema, 1U);
        ktag_feature_config config{};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.schema = schema;
        config.registry = registry;
        config.shared_domain_store = store;
        config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
        config.collect_annotations = 1U;
        session->context_ = ktag_feature_create(&config);
    } else {
        std::fprintf(stderr, "Feature plugin load failed: %s\n", ktag_feature_plugin_get_last_error());
    }
    ktag_feature_registry_destroy(registry);
    ktag_feature_schema_destroy(schema);
    if (!session->context_ || !session->batch_) return nullptr;
    session->link_type_ = static_cast<uint32_t>(link_type);
    return session.release();
}
ktag_demo_feature_session::~ktag_demo_feature_session() {
    ktag_feature_batch_destroy(batch_);
    ktag_feature_destroy(context_);
    ktag_feature_plugin_unload(plugin_);
}
int ktag_demo_feature_session::copy_record(uint32_t index, feature_vector *out, rawbow *text) {
    ktag_feature_record record{};
    if (!out || ktag_feature_batch_get_record(batch_, index, &record) != KTAG_SUCCESS ||
        record.value_count != KTAG_LEGACY_V1_VALUE_COUNT) return KTAG_ERROR_SCHEMA_MISMATCH;
    *out = {};
    std::copy_n(record.values, record.value_count, out->features);
    if (text) {
        *text = {};
        const char *annotation = nullptr;
        const auto status = ktag_feature_batch_get_annotation(batch_, index, KTAG_TRAFFIC_RAWBOW_ANNOTATION, &annotation);
        if (status != KTAG_SUCCESS && status != KTAG_ERROR_NOT_FOUND) return status;
        if (annotation) {
            const auto length = std::min(std::strlen(annotation), static_cast<size_t>(RAWBOW_MAX_LEN - 1U));
            std::memcpy(text->rawbow, annotation, length);
            text->len = static_cast<uint32_t>(length);
        }
    }
    return KTAG_SUCCESS;
}
int ktag_demo_feature_session::process_record(const unsigned char *data, uint32_t length,
    const timeval *timestamp, feature_vector *out, rawbow *text, int *ready) {
    if (!data || !length || !timestamp || !out || !ready || timestamp->tv_sec < 0 ||
        timestamp->tv_usec < 0 || timestamp->tv_usec > 999999 ||
        sequence_ == std::numeric_limits<uint64_t>::max()) return KTAG_ERROR_INVALID_ARGUMENT;
    *ready = 0; *out = {};
    if (text) *text = {};
    ktag_packet packet{};
    packet.struct_size = sizeof(packet); packet.api_version = KTAG_API_VERSION;
    packet.data = data; packet.captured_length = packet.original_length = length;
    packet.link_type = link_type_; packet.sequence = sequence_++;
    packet.timestamp.seconds = timestamp->tv_sec;
    packet.timestamp.microseconds = static_cast<int32_t>(timestamp->tv_usec);
    const auto status = ktag_feature_process_packet(context_, &packet, batch_);
    if (status != KTAG_SUCCESS) return status;
    const auto count = ktag_feature_batch_get_count(batch_);
    if (!count) return KTAG_SUCCESS;
    if (count != 1U) return KTAG_ERROR_CAPACITY;
    const auto copied = copy_record(0U, out, text);
    if (copied == KTAG_SUCCESS) *ready = 1;
    return copied;
}
int ktag_demo_feature_session::process(const unsigned char *p, uint32_t n, const timeval *t, feature_vector *v, int *ready) {
    return process_record(p, n, t, v, nullptr, ready);
}
int ktag_demo_feature_session::process_with_rawbow(const unsigned char *p, uint32_t n, const timeval *t, feature_vector *v, rawbow *s, int *ready) {
    if (!s) return KTAG_ERROR_INVALID_ARGUMENT;
    return process_record(p, n, t, v, s, ready);
}
int ktag_demo_feature_session::flush_records(feature_vector_list *out, rawbow_list *text) {
    if (!out) return KTAG_ERROR_INVALID_ARGUMENT;
    out->count = 0U;
    if (text) text->count = 0U;
    const auto status = ktag_feature_flush(context_, batch_);
    if (status != KTAG_SUCCESS) return status;
    const auto count = ktag_feature_batch_get_count(batch_);
    if (count > SIZE_MAX / sizeof(feature_vector) || count > SIZE_MAX / sizeof(rawbow)) return KTAG_ERROR_CAPACITY;
    if (count > out->capacity || (count && !out->vectors)) {
        auto *values = static_cast<feature_vector *>(std::realloc(out->vectors, count * sizeof(feature_vector)));
        if (!values) return KTAG_ERROR_OUT_OF_MEMORY;
        out->vectors = values; out->capacity = count;
    }
    if (text && (count > text->capacity || (count && !text->rawbows))) {
        auto *values = static_cast<rawbow *>(std::realloc(text->rawbows, count * sizeof(rawbow)));
        if (!values) return KTAG_ERROR_OUT_OF_MEMORY;
        text->rawbows = values; text->capacity = count;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        const auto copied = copy_record(i, &out->vectors[i], text ? &text->rawbows[i] : nullptr);
        if (copied != KTAG_SUCCESS) return copied;
        ++out->count; if (text) ++text->count;
    }
    return KTAG_SUCCESS;
}
int ktag_demo_feature_session::flush(feature_vector_list *out) { return flush_records(out, nullptr); }
int ktag_demo_feature_session::flush_with_rawbow(feature_vector_list *out, rawbow_list *text) {
    return text ? flush_records(out, text) : KTAG_ERROR_INVALID_ARGUMENT;
}
