/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "pipeline_runner.h"

#include "bounded_queue.h"
#include "feature_plugin_loader.h"
#include "../utils.h"

#include "ktag_capture.h"
#include "ktag_feature.h"
#include "ktag_inference.h"

#include <arpa/inet.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

volatile sig_atomic_t pipeline_interrupted = 0;

void pipeline_signal_handler(int)
{
    pipeline_interrupted = 1;
}

class signal_scope {
public:
    signal_scope()
    {
        struct sigaction action{};
        action.sa_handler = pipeline_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        installed_int_ = sigaction(SIGINT, &action, &old_int_) == 0;
        installed_term_ = sigaction(SIGTERM, &action, &old_term_) == 0;
    }

    ~signal_scope()
    {
        if (installed_int_) {
            sigaction(SIGINT, &old_int_, nullptr);
        }
        if (installed_term_) {
            sigaction(SIGTERM, &old_term_, nullptr);
        }
    }

private:
    struct sigaction old_int_{};
    struct sigaction old_term_{};
    bool installed_int_ = false;
    bool installed_term_ = false;
};

struct capture_deleter {
    void operator()(ktag_capture_context *value) const
    {
        ktag_capture_close(value);
    }
};

struct schema_deleter {
    void operator()(ktag_feature_schema *value) const
    {
        ktag_feature_schema_destroy(value);
    }
};

struct registry_deleter {
    void operator()(ktag_feature_registry *value) const
    {
        ktag_feature_registry_destroy(value);
    }
};

struct domain_store_deleter {
    void operator()(ktag_domain_store *value) const
    {
        ktag_domain_store_destroy(value);
    }
};

struct feature_context_deleter {
    void operator()(ktag_feature_context *value) const
    {
        ktag_feature_destroy(value);
    }
};

struct feature_batch_deleter {
    void operator()(ktag_feature_batch *value) const
    {
        ktag_feature_batch_destroy(value);
    }
};

struct inference_context_deleter {
    void operator()(ktag_inference_context *value) const
    {
        ktag_inference_destroy(value);
    }
};

struct prediction_batch_deleter {
    void operator()(ktag_prediction_batch *value) const
    {
        ktag_prediction_batch_destroy(value);
    }
};

using capture_ptr = std::unique_ptr<ktag_capture_context, capture_deleter>;
using schema_ptr = std::unique_ptr<ktag_feature_schema, schema_deleter>;
using registry_ptr = std::unique_ptr<ktag_feature_registry, registry_deleter>;
using domain_store_ptr = std::unique_ptr<ktag_domain_store, domain_store_deleter>;
using feature_context_ptr =
    std::unique_ptr<ktag_feature_context, feature_context_deleter>;
using feature_batch_ptr =
    std::unique_ptr<ktag_feature_batch, feature_batch_deleter>;
using inference_context_ptr =
    std::unique_ptr<ktag_inference_context, inference_context_deleter>;
using prediction_batch_ptr =
    std::unique_ptr<ktag_prediction_batch, prediction_batch_deleter>;

class owned_packet {
public:
    owned_packet()
    {
        ktag_packet_init(&packet_);
    }

    explicit owned_packet(ktag_packet *source)
    {
        packet_ = *source;
        ktag_packet_init(source);
    }

    ~owned_packet()
    {
        ktag_packet_release(&packet_);
    }

    owned_packet(const owned_packet &) = delete;
    owned_packet &operator=(const owned_packet &) = delete;

    owned_packet(owned_packet &&other) noexcept
    {
        packet_ = other.packet_;
        ktag_packet_init(&other.packet_);
    }

    owned_packet &operator=(owned_packet &&other) noexcept
    {
        if (this != &other) {
            ktag_packet_release(&packet_);
            packet_ = other.packet_;
            ktag_packet_init(&other.packet_);
        }
        return *this;
    }

    const ktag_packet *get() const
    {
        return &packet_;
    }

private:
    ktag_packet packet_{};
};

struct feature_record_owned {
    ktag_feature_record_metadata metadata{};
    std::vector<float> values;
};

struct pipeline_prediction {
    uint32_t source_index = 0U;
    ktag_prediction_record record{};
};

struct capture_statistics {
    uint64_t packets = 0U;
    uint64_t bytes = 0U;
    uint64_t unsupported = 0U;
    uint64_t pool_waits = 0U;
    uint64_t read_nanoseconds = 0U;
};

struct feature_worker_statistics {
    uint64_t packets = 0U;
    uint64_t emitted_flows = 0U;
    uint64_t rejected_packets = 0U;
    uint64_t elapsed_nanoseconds = 0U;
    uint64_t work_nanoseconds = 0U;
};

struct inference_worker_statistics {
    uint64_t batches = 0U;
    uint64_t input_records = 0U;
    uint64_t predictions = 0U;
    uint64_t elapsed_nanoseconds = 0U;
    uint64_t work_nanoseconds = 0U;
};

struct pipeline_source_statistics {
    uint64_t packets = 0U;
    uint64_t bytes = 0U;
    uint64_t unsupported_packets = 0U;
    uint64_t pool_waits = 0U;
    uint64_t emitted_flows = 0U;
    uint64_t inference_inputs = 0U;
    uint64_t predictions = 0U;
    uint64_t capture_nanoseconds = 0U;
    uint64_t feature_nanoseconds = 0U;
    uint64_t inference_nanoseconds = 0U;
    uint64_t elapsed_nanoseconds = 0U;
    uint64_t queue_drops = 0U;
    uint64_t queue_failures = 0U;
};

struct pipeline_run_statistics {
    uint64_t packets = 0U;
    uint64_t bytes = 0U;
    uint64_t unsupported_packets = 0U;
    uint64_t pool_waits = 0U;
    uint64_t emitted_flows = 0U;
    uint64_t inference_inputs = 0U;
    uint64_t predictions = 0U;
    uint64_t capture_nanoseconds = 0U;
    uint64_t feature_nanoseconds = 0U;
    uint64_t inference_nanoseconds = 0U;
    uint64_t file_io_nanoseconds = 0U;
    uint64_t elapsed_nanoseconds = 0U;
    uint64_t queue_drops = 0U;
    uint64_t queue_failures = 0U;
};

uint64_t elapsed_nanoseconds(
    const std::chrono::steady_clock::time_point &start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
}

void add_source_statistics(
    pipeline_run_statistics *total,
    const pipeline_source_statistics &source)
{
    total->packets += source.packets;
    total->bytes += source.bytes;
    total->unsupported_packets += source.unsupported_packets;
    total->pool_waits += source.pool_waits;
    total->emitted_flows += source.emitted_flows;
    total->inference_inputs += source.inference_inputs;
    total->predictions += source.predictions;
    total->capture_nanoseconds += source.capture_nanoseconds;
    total->feature_nanoseconds += source.feature_nanoseconds;
    total->inference_nanoseconds += source.inference_nanoseconds;
    total->queue_drops += source.queue_drops;
    total->queue_failures += source.queue_failures;
}

uint64_t timestamp_us(const ktag_timestamp &timestamp)
{
    return static_cast<uint64_t>(timestamp.seconds) * 1000000ULL +
        static_cast<uint32_t>(timestamp.microseconds);
}

bool flow_key_less(const ktag_flow_key &left, const ktag_flow_key &right)
{
    if (left.address_a.version != right.address_a.version) {
        return left.address_a.version < right.address_a.version;
    }
    int comparison = std::memcmp(
        left.address_a.bytes,
        right.address_a.bytes,
        KTAG_IPV6_ADDRESS_SIZE);
    if (comparison != 0) {
        return comparison < 0;
    }
    if (left.port_a != right.port_a) {
        return left.port_a < right.port_a;
    }
    if (left.address_b.version != right.address_b.version) {
        return left.address_b.version < right.address_b.version;
    }
    comparison = std::memcmp(
        left.address_b.bytes,
        right.address_b.bytes,
        KTAG_IPV6_ADDRESS_SIZE);
    if (comparison != 0) {
        return comparison < 0;
    }
    if (left.port_b != right.port_b) {
        return left.port_b < right.port_b;
    }
    return left.protocol < right.protocol;
}

bool prediction_less(
    const pipeline_prediction &left,
    const pipeline_prediction &right)
{
    if (left.source_index != right.source_index) {
        return left.source_index < right.source_index;
    }
    const auto &left_metadata = left.record.metadata;
    const auto &right_metadata = right.record.metadata;
    if (flow_key_less(left_metadata.flow_key, right_metadata.flow_key)) {
        return true;
    }
    if (flow_key_less(right_metadata.flow_key, left_metadata.flow_key)) {
        return false;
    }
    if (left_metadata.first_packet_sequence !=
        right_metadata.first_packet_sequence) {
        return left_metadata.first_packet_sequence <
            right_metadata.first_packet_sequence;
    }
    if (left_metadata.last_packet_sequence !=
        right_metadata.last_packet_sequence) {
        return left_metadata.last_packet_sequence <
            right_metadata.last_packet_sequence;
    }
    return left.record.label < right.record.label;
}

std::string address_text(const ktag_ip_address &address)
{
    char buffer[INET6_ADDRSTRLEN] = {};
    const int family = address.version == 4U ? AF_INET : AF_INET6;
    if (address.version != 4U && address.version != 6U) {
        return "unknown";
    }
    return inet_ntop(family, address.bytes, buffer, sizeof(buffer)) == nullptr ?
        "invalid" : std::string(buffer);
}

std::string flow_text(const ktag_flow_key &key)
{
    std::ostringstream output;
    output << address_text(key.address_a) << ':' << key.port_a << '-'
           << address_text(key.address_b) << ':' << key.port_b << '/'
           << static_cast<uint32_t>(key.protocol);
    return output.str();
}

bool append_predictions(
    const ktag_prediction_batch *batch,
    uint32_t source_index,
    std::vector<pipeline_prediction> *predictions,
    std::string *error)
{
    const uint32_t count = ktag_prediction_batch_get_count(batch);
    try {
        for (uint32_t index = 0U; index < count; ++index) {
            pipeline_prediction result;
            result.source_index = source_index;
            const ktag_status status = ktag_prediction_batch_get_record(
                batch, index, &result.record);
            if (status != KTAG_SUCCESS) {
                *error = "failed to read Prediction Batch record";
                return false;
            }
            predictions->push_back(result);
        }
        return true;
    } catch (...) {
        *error = "out of memory collecting predictions";
        return false;
    }
}

bool copy_feature_records(
    const ktag_feature_batch *batch,
    std::vector<feature_record_owned> *records,
    std::string *error)
{
    const uint32_t count = ktag_feature_batch_get_count(batch);
    try {
        for (uint32_t index = 0U; index < count; ++index) {
            ktag_feature_record view{};
            const ktag_status status =
                ktag_feature_batch_get_record(batch, index, &view);
            if (status != KTAG_SUCCESS) {
                *error = "failed to read Feature Batch record";
                return false;
            }
            feature_record_owned record;
            record.metadata = view.metadata;
            record.values.assign(view.values, view.values + view.value_count);
            records->push_back(std::move(record));
        }
        return true;
    } catch (...) {
        *error = "out of memory copying Feature Batch records";
        return false;
    }
}

ktag_capture_config make_capture_config(
    const char *source,
    bool live,
    bool owned,
    uint32_t packet_pool_size)
{
    ktag_capture_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.source_type = live ?
        KTAG_CAPTURE_SOURCE_LIVE_INTERFACE : KTAG_CAPTURE_SOURCE_PCAP_FILE;
    config.source = source;
    config.packet_mode = owned ?
        KTAG_CAPTURE_PACKET_OWNED : KTAG_CAPTURE_PACKET_BORROWED;
    config.snapshot_length = 65535U;
    config.promiscuous = live ? 1U : 0U;
    config.read_timeout_ms = live ? 10U : 0U;
    config.live_buffer_size = live ? 1024U * 1024U * 1024U : 0U;
    config.packet_buffer_capacity = packet_pool_size;
    return config;
}

domain_store_ptr create_domain_store()
{
    ktag_domain_store_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.max_entries = KTAG_DOMAIN_DEFAULT_MAX_ENTRIES;
    config.journal_capacity = KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY;
    return domain_store_ptr(ktag_domain_store_create(&config));
}

feature_context_ptr create_feature_context(
    const ktag_demo_pipeline_config &pipeline_config,
    const ktag_feature_schema *schema,
    const ktag_feature_registry *registry,
    ktag_domain_store *domain_store)
{
    ktag_feature_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.schema = schema;
    config.registry = registry;
    config.shared_domain_store = domain_store;
    config.flow_capacity = pipeline_config.flow_capacity;
    config.tcp_idle_timeout_us = pipeline_config.tcp_idle_timeout_us;
    config.udp_idle_timeout_us = pipeline_config.udp_idle_timeout_us;
    config.udp_packet_limit = pipeline_config.udp_packet_limit;
    config.domain_authority_lookup_on_miss = 1U;
    config.lifecycle_mode =
        pipeline_config.lifecycle_mode ==
                ktag_demo_feature_lifecycle_mode::legacy_compat ?
            KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT :
            KTAG_FEATURE_LIFECYCLE_MODERN;
    return feature_context_ptr(ktag_feature_create(&config));
}

inference_context_ptr create_inference_context(
    const ktag_demo_pipeline_config &pipeline_config,
    const ktag_feature_schema *schema)
{
    ktag_inference_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.model_package_path = pipeline_config.model_package_path.c_str();
    config.feature_schema = schema;
    config.intra_op_threads = pipeline_config.inference_intra_op_threads;
    config.inter_op_threads = pipeline_config.inference_inter_op_threads;
    return inference_context_ptr(ktag_inference_create(&config));
}

schema_ptr compose_schema(
    const ktag_demo_pipeline_config &config,
    const ktag_feature_registry *registry,
    std::string *error)
{
    schema_ptr result(ktag_feature_schema_create(
        config.schema_name.c_str(), config.schema_version));
    if (!result) {
        *error = "failed to create configured Feature Schema";
        return {};
    }

    std::vector<std::string> features;
    if (!config.feature_order.empty()) {
        features = config.feature_order;
    } else {
        schema_ptr legacy(ktag_feature_schema_create_legacy_v1());
        if (!legacy) {
            *error = "failed to create Legacy Schema template";
            return {};
        }
        const std::unordered_set<std::string> removed(
            config.remove_features.cbegin(), config.remove_features.cend());
        std::unordered_set<std::string> found_removed;
        for (uint32_t index = 0U;
             index < ktag_feature_schema_get_feature_count(legacy.get());
             ++index) {
            ktag_feature_descriptor descriptor{};
            uint32_t offset = 0U;
            if (ktag_feature_schema_get_descriptor(
                    legacy.get(), index, &descriptor, &offset) !=
                KTAG_SUCCESS) {
                *error = "failed to read Legacy Schema template";
                return {};
            }
            if (removed.find(descriptor.feature_id) == removed.end()) {
                features.emplace_back(descriptor.feature_id);
            } else {
                found_removed.emplace(descriptor.feature_id);
            }
        }
        if (found_removed.size() != removed.size()) {
            for (const auto &feature_id : removed) {
                if (found_removed.find(feature_id) == found_removed.end()) {
                    *error = "remove_features contains an unknown legacy "
                        "feature: " + feature_id;
                    return {};
                }
            }
        }
        features.insert(
            features.end(),
            config.add_features.cbegin(),
            config.add_features.cend());
    }

    for (const auto &feature_id : features) {
        const ktag_status status = ktag_feature_schema_add_registered(
            result.get(), registry, feature_id.c_str());
        if (status != KTAG_SUCCESS) {
            *error = "configured feature is missing, duplicated or invalid: " +
                feature_id;
            return {};
        }
    }
    if (ktag_feature_schema_freeze(result.get()) != KTAG_SUCCESS) {
        *error = "failed to freeze configured Feature Schema";
        return {};
    }
    return result;
}

bool run_inference_batch(
    ktag_inference_context *inference,
    const ktag_feature_batch *features,
    ktag_prediction_batch *prediction_batch,
    uint32_t source_index,
    std::vector<pipeline_prediction> *predictions,
    std::string *error)
{
    const ktag_status status = ktag_inference_predict(
        inference, features, prediction_batch);
    if (status != KTAG_SUCCESS) {
        *error = std::string("inference failed: ") +
            ktag_inference_get_last_error();
        return false;
    }
    return append_predictions(
        prediction_batch, source_index, predictions, error);
}

bool process_feature_output_single(
    ktag_inference_context *inference,
    ktag_feature_batch *features,
    ktag_prediction_batch *prediction_batch,
    uint32_t source_index,
    std::vector<pipeline_prediction> *predictions,
    pipeline_source_statistics *statistics,
    std::string *error)
{
    const uint32_t input_count = ktag_feature_batch_get_count(features);
    if (input_count == 0U) {
        return true;
    }
    statistics->emitted_flows += input_count;
    statistics->inference_inputs += input_count;
    const size_t prediction_start = predictions->size();
    const auto inference_start = std::chrono::steady_clock::now();
    const bool success = run_inference_batch(
        inference,
        features,
        prediction_batch,
        source_index,
        predictions,
        error);
    statistics->inference_nanoseconds += elapsed_nanoseconds(inference_start);
    statistics->predictions += predictions->size() - prediction_start;
    return success;
}

bool run_single_source(
    const ktag_demo_pipeline_config &config,
    const ktag_feature_schema *schema,
    const ktag_feature_registry *registry,
    const char *source,
    bool live,
    uint32_t source_index,
    std::vector<pipeline_prediction> *predictions,
    pipeline_source_statistics *statistics,
    std::string *error)
{
    *statistics = pipeline_source_statistics{};
    const auto execution_start = std::chrono::steady_clock::now();
    const ktag_capture_config capture_config = make_capture_config(
        source, live, false, config.packet_pool_size);
    capture_ptr capture(ktag_capture_open(&capture_config));
    if (!capture) {
        *error = std::string("capture open failed: ") +
            ktag_capture_get_last_error();
        return false;
    }
    domain_store_ptr domain_store = create_domain_store();
    feature_context_ptr feature = create_feature_context(
        config, schema, registry, domain_store.get());
    inference_context_ptr inference = create_inference_context(config, schema);
    feature_batch_ptr feature_batch(ktag_feature_batch_create(schema, 64U));
    prediction_batch_ptr prediction_batch(
        ktag_prediction_batch_create(config.inference_batch_size));
    if (!domain_store || !feature || !inference || !feature_batch ||
        !prediction_batch) {
        *error = std::string("failed to create single-thread pipeline contexts: ") +
            ktag_inference_get_last_error();
        return false;
    }

    uint64_t packet_count = 0U;
    uint64_t last_expire_us = 0U;
    ktag_packet packet{};
    ktag_packet_init(&packet);
    while (pipeline_interrupted == 0) {
        const auto capture_start = std::chrono::steady_clock::now();
        const ktag_status capture_status =
            ktag_capture_next(capture.get(), &packet);
        statistics->capture_nanoseconds += elapsed_nanoseconds(capture_start);
        if (capture_status == KTAG_STATUS_TIMEOUT) {
            continue;
        }
        if (capture_status == KTAG_STATUS_END_OF_STREAM) {
            break;
        }
        if (capture_status != KTAG_SUCCESS) {
            *error = std::string("capture read failed: ") +
                ktag_capture_get_last_error();
            ktag_packet_release(&packet);
            return false;
        }
        ++packet_count;
        statistics->bytes += packet.captured_length;
        const uint64_t now_us = timestamp_us(packet.timestamp);
        const auto feature_start = std::chrono::steady_clock::now();
        const ktag_status feature_status = ktag_feature_process_packet(
            feature.get(), &packet, feature_batch.get());
        statistics->feature_nanoseconds += elapsed_nanoseconds(feature_start);
        ktag_packet_release(&packet);
        if (feature_status == KTAG_ERROR_NOT_SUPPORTED ||
            feature_status == KTAG_ERROR_INVALID_ARGUMENT) {
            ++statistics->unsupported_packets;
            continue;
        }
        if (feature_status != KTAG_SUCCESS ||
            !process_feature_output_single(
                inference.get(),
                feature_batch.get(),
                prediction_batch.get(),
                source_index,
                predictions,
                statistics,
                error)) {
            if (feature_status != KTAG_SUCCESS) {
                *error = "feature processing failed with status " +
                    std::to_string(feature_status);
            }
            return false;
        }
        const uint64_t interval_us =
            static_cast<uint64_t>(config.expire_interval_ms) * 1000U;
        if (interval_us != 0U &&
            (last_expire_us == 0U ||
             (now_us >= last_expire_us &&
              now_us - last_expire_us >= interval_us))) {
            const auto expire_start = std::chrono::steady_clock::now();
            const ktag_status expire_status = ktag_feature_expire(
                feature.get(), now_us, feature_batch.get());
            statistics->feature_nanoseconds +=
                elapsed_nanoseconds(expire_start);
            if (expire_status != KTAG_SUCCESS ||
                !process_feature_output_single(
                    inference.get(),
                    feature_batch.get(),
                    prediction_batch.get(),
                    source_index,
                    predictions,
                    statistics,
                    error)) {
                if (expire_status != KTAG_SUCCESS) {
                    *error = "feature expiration failed";
                }
                return false;
            }
            last_expire_us = now_us;
        }
    }

    const auto flush_start = std::chrono::steady_clock::now();
    const ktag_status flush_status = ktag_feature_flush(
        feature.get(), feature_batch.get());
    statistics->feature_nanoseconds += elapsed_nanoseconds(flush_start);
    if (flush_status != KTAG_SUCCESS ||
        !process_feature_output_single(
            inference.get(),
            feature_batch.get(),
            prediction_batch.get(),
            source_index,
            predictions,
            statistics,
            error)) {
        if (flush_status != KTAG_SUCCESS) {
            *error = "feature flush failed";
        }
        return false;
    }
    statistics->packets = packet_count;
    statistics->elapsed_nanoseconds = elapsed_nanoseconds(execution_start);
    return true;
}
