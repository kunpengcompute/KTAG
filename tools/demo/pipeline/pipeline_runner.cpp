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
    config.domain_authority_lookup_on_miss = 0U;
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

class parallel_source_executor {
public:
    parallel_source_executor(
        const ktag_demo_pipeline_config &config,
        const ktag_feature_schema *schema,
        const ktag_feature_registry *registry,
        uint32_t source_index)
        : config_(config),
          schema_(schema),
          registry_(registry),
          source_index_(source_index),
          feature_queue_(config.feature_queue_capacity),
          active_feature_workers_(config.feature_threads),
          feature_stats_(config.feature_threads),
          inference_stats_(config.inference_threads)
    {
        packet_queues_.reserve(config.feature_threads);
        for (uint32_t index = 0U; index < config.feature_threads; ++index) {
            packet_queues_.emplace_back(
                new ktag_demo_bounded_queue<owned_packet>(
                    config.packet_queue_capacity));
        }
    }

    bool run(
        const char *source,
        bool live,
        std::vector<pipeline_prediction> *predictions,
        pipeline_source_statistics *statistics,
        std::string *error)
    {
        *statistics = pipeline_source_statistics{};
        predictions_ = predictions;
        const ktag_capture_config capture_config = make_capture_config(
            source, live, true, config_.packet_pool_size);
        capture_.reset(ktag_capture_open(&capture_config));
        domain_store_ = create_domain_store();
        if (!capture_ || !domain_store_ || !create_contexts()) {
            *error = error_message_.empty() ?
                std::string("parallel pipeline initialization failed: ") +
                    ktag_capture_get_last_error() : error_message_;
            return false;
        }

        const auto execution_start = std::chrono::steady_clock::now();
        try {
            for (uint32_t index = 0U;
                 index < config_.inference_threads;
                 ++index) {
                inference_threads_.emplace_back(
                    &parallel_source_executor::inference_worker,
                    this,
                    index);
            }
            for (uint32_t index = 0U;
                 index < config_.feature_threads;
                 ++index) {
                feature_threads_.emplace_back(
                    &parallel_source_executor::feature_worker,
                    this,
                    index);
            }
            capture_thread_ = std::thread(
                &parallel_source_executor::capture_worker,
                this);
        } catch (...) {
            fail("failed to create pipeline threads");
        }

        if (capture_thread_.joinable()) {
            capture_thread_.join();
        } else {
            close_packet_queues();
        }
        for (auto &thread : feature_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        feature_queue_.close();
        for (auto &thread : inference_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        const auto execution_end = std::chrono::steady_clock::now();

        if (failed_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(error_mutex_);
            *error = error_message_;
            return false;
        }
        collect_statistics(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                execution_end - execution_start).count()), statistics);
        return true;
    }

private:
    bool create_contexts()
    {
        try {
            feature_contexts_.reserve(config_.feature_threads);
            inference_contexts_.reserve(config_.inference_threads);
            for (uint32_t index = 0U;
                 index < config_.feature_threads;
                 ++index) {
                auto context = create_feature_context(
                    config_, schema_, registry_, domain_store_.get());
                if (!context) {
                    error_message_ = "failed to create Feature Context";
                    return false;
                }
                feature_contexts_.push_back(std::move(context));
            }
            for (uint32_t index = 0U;
                 index < config_.inference_threads;
                 ++index) {
                auto context = create_inference_context(config_, schema_);
                if (!context) {
                    error_message_ = std::string(
                        "failed to create Inference Context: ") +
                        ktag_inference_get_last_error();
                    return false;
                }
                inference_contexts_.push_back(std::move(context));
            }
            return true;
        } catch (...) {
            error_message_ = "out of memory creating pipeline contexts";
            return false;
        }
    }

    void fail(const std::string &message)
    {
        bool expected = false;
        if (failed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_message_ = message;
        }
        close_packet_queues();
        feature_queue_.close();
    }

    void close_packet_queues()
    {
        for (auto &queue : packet_queues_) {
            queue->close();
        }
    }

    void capture_worker()
    {
        ktag_packet packet{};
        ktag_packet_init(&packet);
        while (!failed_.load(std::memory_order_acquire) &&
               pipeline_interrupted == 0) {
            const auto read_start = std::chrono::steady_clock::now();
            const ktag_status status =
                ktag_capture_next(capture_.get(), &packet);
            capture_stats_.read_nanoseconds +=
                elapsed_nanoseconds(read_start);
            if (status == KTAG_STATUS_TIMEOUT) {
                continue;
            }
            if (status == KTAG_ERROR_CAPACITY) {
                ++capture_stats_.pool_waits;
                std::this_thread::yield();
                continue;
            }
            if (status == KTAG_STATUS_END_OF_STREAM) {
                break;
            }
            if (status != KTAG_SUCCESS) {
                fail(std::string("capture read failed: ") +
                    ktag_capture_get_last_error());
                break;
            }

            ++capture_stats_.packets;
            capture_stats_.bytes += packet.captured_length;
            ktag_flow_key key{};
            const ktag_status key_status =
                ktag_packet_get_flow_key(&packet, &key);
            if (key_status != KTAG_SUCCESS) {
                ++capture_stats_.unsupported;
                ktag_packet_release(&packet);
                continue;
            }
            const uint32_t worker = ktag_flow_select_worker(
                &key, config_.feature_threads, config_.flow_hash_seed);
            if (worker == KTAG_WORKER_INDEX_INVALID ||
                worker >= packet_queues_.size()) {
                ktag_packet_release(&packet);
                fail("dispatcher returned an invalid Feature worker");
                break;
            }

            owned_packet item(&packet);
            const auto result = packet_queues_[worker]->push(
                std::move(item), config_.backpressure);
            if (result == ktag_demo_queue_push_result::failed) {
                fail("packet queue reached capacity with fail policy");
                break;
            }
            if (result == ktag_demo_queue_push_result::closed) {
                break;
            }
        }
        ktag_packet_release(&packet);
        close_packet_queues();
    }

    bool enqueue_feature_batch(ktag_feature_batch *batch)
    {
        std::vector<feature_record_owned> records;
        std::string error;
        if (!copy_feature_records(batch, &records, &error)) {
            fail(error);
            return false;
        }
        for (auto &record : records) {
            const auto result = feature_queue_.push(
                std::move(record), config_.backpressure);
            if (result == ktag_demo_queue_push_result::failed) {
                fail("feature queue reached capacity with fail policy");
                return false;
            }
            if (result == ktag_demo_queue_push_result::closed) {
                if (!failed_.load(std::memory_order_acquire)) {
                    fail("feature queue closed before Feature workers drained");
                }
                return false;
            }
        }
        return true;
    }

    void feature_worker(uint32_t worker_index)
    {
        const auto start = std::chrono::steady_clock::now();
        feature_batch_ptr output(ktag_feature_batch_create(schema_, 64U));
        if (!output) {
            fail("failed to create Feature worker output batch");
            finish_feature_worker(worker_index, start);
            return;
        }

        uint64_t last_expire_us = 0U;
        owned_packet packet;
        while (packet_queues_[worker_index]->pop(&packet)) {
            if (failed_.load(std::memory_order_acquire)) {
                break;
            }
            ++feature_stats_[worker_index].packets;
            const uint64_t now_us = timestamp_us(packet.get()->timestamp);
            const auto process_start = std::chrono::steady_clock::now();
            const ktag_status status = ktag_feature_process_packet(
                feature_contexts_[worker_index].get(),
                packet.get(),
                output.get());
            feature_stats_[worker_index].work_nanoseconds +=
                elapsed_nanoseconds(process_start);
            packet = owned_packet();
            if (status == KTAG_ERROR_NOT_SUPPORTED ||
                status == KTAG_ERROR_INVALID_ARGUMENT) {
                ++feature_stats_[worker_index].rejected_packets;
                continue;
            }
            if (status != KTAG_SUCCESS || !enqueue_feature_batch(output.get())) {
                if (status != KTAG_SUCCESS) {
                    fail("Feature worker processing failed with status " +
                        std::to_string(status));
                }
                break;
            }
            feature_stats_[worker_index].emitted_flows +=
                ktag_feature_batch_get_count(output.get());

            const uint64_t interval_us =
                static_cast<uint64_t>(config_.expire_interval_ms) * 1000U;
            if (interval_us != 0U &&
                (last_expire_us == 0U ||
                 (now_us >= last_expire_us &&
                  now_us - last_expire_us >= interval_us))) {
                const auto expire_start = std::chrono::steady_clock::now();
                const ktag_status expire_status = ktag_feature_expire(
                    feature_contexts_[worker_index].get(),
                    now_us,
                    output.get());
                feature_stats_[worker_index].work_nanoseconds +=
                    elapsed_nanoseconds(expire_start);
                if (expire_status != KTAG_SUCCESS ||
                    !enqueue_feature_batch(output.get())) {
                    if (expire_status != KTAG_SUCCESS) {
                        fail("Feature worker expiration failed");
                    }
                    break;
                }
                feature_stats_[worker_index].emitted_flows +=
                    ktag_feature_batch_get_count(output.get());
                last_expire_us = now_us;
            }
        }

        if (!failed_.load(std::memory_order_acquire)) {
            const auto flush_start = std::chrono::steady_clock::now();
            const ktag_status flush_status = ktag_feature_flush(
                feature_contexts_[worker_index].get(), output.get());
            feature_stats_[worker_index].work_nanoseconds +=
                elapsed_nanoseconds(flush_start);
            if (flush_status != KTAG_SUCCESS ||
                !enqueue_feature_batch(output.get())) {
                if (flush_status != KTAG_SUCCESS) {
                    fail("Feature worker flush failed");
                }
            } else {
                feature_stats_[worker_index].emitted_flows +=
                    ktag_feature_batch_get_count(output.get());
            }
        }
        finish_feature_worker(worker_index, start);
    }

    void finish_feature_worker(
        uint32_t worker_index,
        const std::chrono::steady_clock::time_point &start)
    {
        feature_stats_[worker_index].elapsed_nanoseconds =
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
        if (active_feature_workers_.fetch_sub(
                1U, std::memory_order_acq_rel) == 1U) {
            feature_queue_.close();
        }
    }

    void inference_worker(uint32_t worker_index)
    {
        const auto start = std::chrono::steady_clock::now();
        feature_batch_ptr input(ktag_feature_batch_create(
            schema_, config_.inference_batch_size));
        prediction_batch_ptr output(ktag_prediction_batch_create(
            config_.inference_batch_size));
        if (!input || !output) {
            fail("failed to create Inference worker batches");
            return;
        }

        feature_record_owned record;
        while (feature_queue_.pop(&record)) {
            if (failed_.load(std::memory_order_acquire)) {
                break;
            }
            ktag_feature_batch_clear(input.get());
            uint32_t batch_count = 0U;
            while (true) {
                const ktag_status append_status = ktag_feature_batch_append(
                    input.get(),
                    &record.metadata,
                    record.values.data(),
                    static_cast<uint32_t>(record.values.size()));
                if (append_status != KTAG_SUCCESS) {
                    fail("failed to build Inference worker batch");
                    break;
                }
                ++batch_count;
                if (batch_count >= config_.inference_batch_size ||
                    !feature_queue_.try_pop(&record)) {
                    break;
                }
            }
            if (failed_.load(std::memory_order_acquire)) {
                break;
            }

            const auto inference_start = std::chrono::steady_clock::now();
            const ktag_status status = ktag_inference_predict(
                inference_contexts_[worker_index].get(),
                input.get(),
                output.get());
            inference_stats_[worker_index].work_nanoseconds +=
                elapsed_nanoseconds(inference_start);
            if (status != KTAG_SUCCESS) {
                fail(std::string("Inference worker failed: ") +
                    ktag_inference_get_last_error());
                break;
            }
            std::vector<pipeline_prediction> local;
            std::string error;
            if (!append_predictions(
                    output.get(), source_index_, &local, &error)) {
                fail(error);
                break;
            }
            {
                std::lock_guard<std::mutex> lock(prediction_mutex_);
                try {
                    predictions_->insert(
                        predictions_->end(),
                        local.begin(),
                        local.end());
                } catch (...) {
                    fail("out of memory merging Inference results");
                    break;
                }
            }
            ++inference_stats_[worker_index].batches;
            inference_stats_[worker_index].input_records += batch_count;
            inference_stats_[worker_index].predictions += local.size();
        }
        inference_stats_[worker_index].elapsed_nanoseconds =
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
    }

    void collect_statistics(
        uint64_t source_elapsed_nanoseconds,
        pipeline_source_statistics *statistics) const
    {
        statistics->packets = capture_stats_.packets;
        statistics->bytes = capture_stats_.bytes;
        statistics->unsupported_packets = capture_stats_.unsupported;
        statistics->pool_waits = capture_stats_.pool_waits;
        statistics->capture_nanoseconds = capture_stats_.read_nanoseconds;
        statistics->elapsed_nanoseconds = source_elapsed_nanoseconds;

        for (uint32_t index = 0U; index < packet_queues_.size(); ++index) {
            const auto queue = packet_queues_[index]->statistics();
            const auto &worker = feature_stats_[index];
            statistics->unsupported_packets += worker.rejected_packets;
            statistics->emitted_flows += worker.emitted_flows;
            statistics->feature_nanoseconds = std::max(
                statistics->feature_nanoseconds,
                worker.work_nanoseconds);
            statistics->queue_drops +=
                queue.dropped_newest + queue.dropped_oldest;
            statistics->queue_failures += queue.failures;
        }
        const auto feature_queue = feature_queue_.statistics();
        statistics->queue_drops +=
            feature_queue.dropped_newest + feature_queue.dropped_oldest;
        statistics->queue_failures += feature_queue.failures;
        for (uint32_t index = 0U; index < inference_stats_.size(); ++index) {
            const auto &worker = inference_stats_[index];
            statistics->inference_inputs += worker.input_records;
            statistics->predictions += worker.predictions;
            statistics->inference_nanoseconds += worker.work_nanoseconds;
        }
    }

    const ktag_demo_pipeline_config &config_;
    const ktag_feature_schema *schema_;
    const ktag_feature_registry *registry_;
    uint32_t source_index_;
    capture_ptr capture_;
    domain_store_ptr domain_store_;
    std::vector<feature_context_ptr> feature_contexts_;
    std::vector<inference_context_ptr> inference_contexts_;
    std::vector<std::unique_ptr<
        ktag_demo_bounded_queue<owned_packet>>> packet_queues_;
    ktag_demo_bounded_queue<feature_record_owned> feature_queue_;
    std::atomic<uint32_t> active_feature_workers_;
    std::atomic<bool> failed_{false};
    std::mutex error_mutex_;
    std::string error_message_;
    capture_statistics capture_stats_;
    std::vector<feature_worker_statistics> feature_stats_;
    std::vector<inference_worker_statistics> inference_stats_;
    std::thread capture_thread_;
    std::vector<std::thread> feature_threads_;
    std::vector<std::thread> inference_threads_;
    std::mutex prediction_mutex_;
    std::vector<pipeline_prediction> *predictions_ = nullptr;
};

bool run_parallel_source(
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
    parallel_source_executor executor(
        config, schema, registry, source_index);
    return executor.run(source, live, predictions, statistics, error);
}

bool write_predictions(
    const char *path,
    ktag_demo_pipeline_output_format format,
    const std::vector<pipeline_prediction> &predictions,
    std::string *error)
{
    if (path == nullptr || path[0] == '\0') {
        std::printf(
            "source_index,flow_key,first_packet_sequence,"
            "last_packet_sequence,label,confidence\n");
        for (const auto &prediction : predictions) {
            std::printf(
                "%u,\"%s\",%llu,%llu,%lld,%.9g\n",
                prediction.source_index,
                flow_text(prediction.record.metadata.flow_key).c_str(),
                static_cast<unsigned long long>(
                    prediction.record.metadata.first_packet_sequence),
                static_cast<unsigned long long>(
                    prediction.record.metadata.last_packet_sequence),
                static_cast<long long>(prediction.record.label),
                static_cast<double>(prediction.record.confidence));
        }
        return true;
    }
    std::ofstream output(path);
    if (!output) {
        *error = std::string("failed to open pipeline output: ") + path;
        return false;
    }
    output << std::setprecision(9);
    if (format == ktag_demo_pipeline_output_format::csv) {
        output << "source_index,flow_key,first_packet_sequence,"
                  "last_packet_sequence,label,confidence\n";
        for (const auto &prediction : predictions) {
            output << prediction.source_index << ",\""
                   << flow_text(prediction.record.metadata.flow_key) << "\","
                   << prediction.record.metadata.first_packet_sequence << ','
                   << prediction.record.metadata.last_packet_sequence << ','
                   << prediction.record.label << ','
                   << prediction.record.confidence << '\n';
        }
    } else {
        output << "[\n";
        for (size_t index = 0U; index < predictions.size(); ++index) {
            const auto &prediction = predictions[index];
            output << "  {\"source_index\":" << prediction.source_index
                   << ",\"flow_key\":\""
                   << flow_text(prediction.record.metadata.flow_key)
                   << "\",\"first_packet_sequence\":"
                   << prediction.record.metadata.first_packet_sequence
                   << ",\"last_packet_sequence\":"
                   << prediction.record.metadata.last_packet_sequence
                   << ",\"label\":" << prediction.record.label
                   << ",\"confidence\":";
            if (std::isfinite(prediction.record.confidence)) {
                output << prediction.record.confidence;
            } else {
                output << "null";
            }
            output << '}' << (index + 1U == predictions.size() ? "\n" : ",\n");
        }
        output << "]\n";
    }
    if (!output) {
        *error = "failed while writing pipeline output";
        return false;
    }
    return true;
}

} // namespace

int ktag_demo_run_pipeline_internal(
    const ktag_demo_pipeline_config &config,
    const char *config_file,
    const char **pcap_files,
    int pcap_file_count,
    const char *interface_name,
    const char *output_file,
    ktag_demo_pipeline_output_format output_format)
{
    pipeline_interrupted = 0;
    signal_scope signals;
    std::printf("========== 在线推理模式 ==========\n");
    std::printf("配置文件: %s\n", config_file == nullptr ? "" : config_file);
    std::printf("管线模式: %s\n", ktag_demo_pipeline_mode_name(config.mode));
    std::printf("特征线程数: %u\n", config.feature_threads);
    std::printf("推理线程数: %u\n", config.inference_threads);
    std::printf(
        "流生命周期: %s\n",
        ktag_demo_feature_lifecycle_mode_name(config.lifecycle_mode));
    std::printf("背压策略: %s\n", ktag_demo_backpressure_name(config.backpressure));
    std::printf("模型包: %s\n", config.model_package_path.c_str());

    ktag_demo_feature_plugin_loader plugin_loader;
    registry_ptr registry(ktag_feature_registry_create());
    if (!registry ||
        ktag_feature_registry_register_legacy_builtins(registry.get()) !=
            KTAG_SUCCESS) {
        std::fprintf(stderr, "pipeline: failed to register built-in Providers\n");
        return -1;
    }

    std::string error;
    for (const auto &plugin : config.plugin_paths) {
        if (!plugin_loader.load(plugin, registry.get(), &error)) {
            std::fprintf(stderr, "pipeline: %s\n", error.c_str());
            registry.reset();
            plugin_loader.unload_all();
            return -1;
        }
    }
    schema_ptr schema = compose_schema(config, registry.get(), &error);
    if (!schema) {
        std::fprintf(stderr, "pipeline: %s\n", error.c_str());
        registry.reset();
        plugin_loader.unload_all();
        return -1;
    }

    ktag_model_package_validation validation{};
    validation.struct_size = sizeof(validation);
    validation.api_version = KTAG_API_VERSION;
    if (ktag_inference_validate_model_package(
            config.model_package_path.c_str(),
            schema.get(),
            &validation) != KTAG_SUCCESS) {
        std::fprintf(
            stderr,
            "pipeline: model package validation failed: %s\n",
            ktag_inference_get_last_error());
        schema.reset();
        registry.reset();
        plugin_loader.unload_all();
        return -1;
    }

    std::printf(
        "Schema: %s v%u (hash=0x%016llx, 模型输入=%u, 插件=%zu)\n",
        config.schema_name.c_str(),
        config.schema_version,
        static_cast<unsigned long long>(validation.schema_hash),
        validation.input_value_count,
        plugin_loader.size());

    std::vector<pipeline_prediction> predictions;
    pipeline_run_statistics run_statistics;
    const auto run_start = std::chrono::steady_clock::now();
    bool success = true;
    if (interface_name != nullptr && interface_name[0] != '\0') {
        std::printf("\n========== 网口实时抓包模式 ==========\n");
        std::printf("网口: %s\n", interface_name);
        std::printf("输出: %s\n", output_file == nullptr ?
            "仅控制台显示" : output_file);
        std::printf("开始实时抓包 (Ctrl+C 退出)...\n\n");
        pipeline_source_statistics source_statistics;
        success = config.mode == ktag_demo_pipeline_mode::single_thread ?
            run_single_source(
                config,
                schema.get(),
                registry.get(),
                interface_name,
                true,
                0U,
                &predictions,
                &source_statistics,
                &error) :
            run_parallel_source(
                config,
                schema.get(),
                registry.get(),
                interface_name,
                true,
                0U,
                &predictions,
                &source_statistics,
                &error);
        if (success) {
            add_source_statistics(&run_statistics, source_statistics);
        }
    } else {
        std::printf("\n========== PCAP文件推理模式 ==========\n");
        std::printf("文件数量: %d\n", pcap_file_count);
        std::printf("推理模式: %s\n", ktag_demo_pipeline_mode_name(config.mode));
        if (output_file != nullptr) {
            std::printf(
                "输出文件: %s (格式: %s)\n",
                output_file,
                output_format == ktag_demo_pipeline_output_format::csv ?
                    "CSV" : "JSON");
        }
        for (int index = 0;
             index < pcap_file_count && success && pipeline_interrupted == 0;
             ++index) {
            std::printf(
                "\n处理文件 %d/%d: %s (Label=%d)\n",
                index + 1,
                pcap_file_count,
                pcap_files[index],
                index);
            pipeline_source_statistics source_statistics;
            success = config.mode == ktag_demo_pipeline_mode::single_thread ?
                run_single_source(
                    config,
                    schema.get(),
                    registry.get(),
                    pcap_files[index],
                    false,
                    static_cast<uint32_t>(index),
                    &predictions,
                    &source_statistics,
                    &error) :
                run_parallel_source(
                    config,
                    schema.get(),
                    registry.get(),
                    pcap_files[index],
                    false,
                    static_cast<uint32_t>(index),
                    &predictions,
                    &source_statistics,
                    &error);
            if (success) {
                add_source_statistics(&run_statistics, source_statistics);
            }
        }
    }

    if (success) {
        std::sort(predictions.begin(), predictions.end(), prediction_less);
        const auto io_start = std::chrono::steady_clock::now();
        success = write_predictions(
            output_file, output_format, predictions, &error);
        run_statistics.file_io_nanoseconds += elapsed_nanoseconds(io_start);
    }
    run_statistics.elapsed_nanoseconds = elapsed_nanoseconds(run_start);
    if (!success) {
        std::fprintf(stderr, "pipeline: %s\n", error.c_str());
    } else {
        PerformanceStats performance{};
        constexpr double nanoseconds_per_second = 1000000000.0;
        performance.pcap_read_time =
            run_statistics.capture_nanoseconds / nanoseconds_per_second;
        performance.feature_extraction_time =
            run_statistics.feature_nanoseconds / nanoseconds_per_second;
        performance.inference_time =
            run_statistics.inference_nanoseconds / nanoseconds_per_second;
        performance.file_io_time =
            run_statistics.file_io_nanoseconds / nanoseconds_per_second;
        performance.total_time = performance.feature_extraction_time +
            performance.inference_time + performance.file_io_time;
        performance.end_to_end_time =
            run_statistics.elapsed_nanoseconds / nanoseconds_per_second;
        performance.total_packets = run_statistics.packets;
        performance.total_bytes = run_statistics.bytes;
        print_performance_stats_ex(
            &performance,
            config.mode == ktag_demo_pipeline_mode::single_thread ? 0 : 1);

        const uint64_t kept = run_statistics.predictions;
        const uint64_t filtered = run_statistics.inference_inputs >= kept ?
            run_statistics.inference_inputs - kept : 0U;
        std::printf(
            "过滤统计: 保留 %llu 个样本, 过滤掉 %llu 个样本\n",
            static_cast<unsigned long long>(kept),
            static_cast<unsigned long long>(filtered));
        std::printf(
            "\n======================= 管线统计 "
            "=======================\n");
        std::printf(
            "特征提取流数:     %10llu 个\n"
            "推理输入流数:     %10llu 个\n"
            "不支持数据包:     %10llu 个\n"
            "Packet Pool等待:%10llu 次\n",
            static_cast<unsigned long long>(run_statistics.emitted_flows),
            static_cast<unsigned long long>(run_statistics.inference_inputs),
            static_cast<unsigned long long>(
                run_statistics.unsupported_packets),
            static_cast<unsigned long long>(run_statistics.pool_waits));
        std::printf(
            "队列丢弃:       %10llu 个\n"
            "队列失败:       %10llu 次\n"
            "是否中断:         %10s\n"
            "========================================================\n",
            static_cast<unsigned long long>(run_statistics.queue_drops),
            static_cast<unsigned long long>(run_statistics.queue_failures),
            pipeline_interrupted != 0 ? "是" : "否");

        if (interface_name == nullptr && !predictions.empty()) {
            std::vector<int> labels;
            std::vector<int> predicted_labels;
            labels.reserve(predictions.size());
            predicted_labels.reserve(predictions.size());
            for (const auto &prediction : predictions) {
                labels.push_back(static_cast<int>(prediction.source_index));
                predicted_labels.push_back(
                    static_cast<int>(prediction.record.label));
            }
            ClassificationReport *report = calculate_classification_report(
                labels.data(),
                predicted_labels.data(),
                static_cast<int>(predictions.size()));
            if (report != nullptr) {
                (void)print_and_save_classification_report(
                    report, output_file);
                free_classification_report(report);
            }
        }
        if (output_file != nullptr) {
            std::printf("推理结果已保存到: %s\n", output_file);
        }
    }

    /* Required order: Contexts are local and already gone, then Schema and
     * Registry, finally plugin state and dlclose. */
    schema.reset();
    registry.reset();
    plugin_loader.unload_all();
    return success ? 0 : -1;
}

int ktag_demo_run_pipeline(
    const ktag_demo_pipeline_config &config,
    const char *config_file,
    const char **pcap_files,
    int pcap_file_count,
    const char *interface_name,
    const char *output_file,
    ktag_demo_pipeline_output_format output_format)
{
    try {
        return ktag_demo_run_pipeline_internal(
            config,
            config_file,
            pcap_files,
            pcap_file_count,
            interface_name,
            output_file,
            output_format);
    } catch (const std::exception &exception) {
        std::fprintf(
            stderr,
            "pipeline: unhandled runtime failure: %s\n",
            exception.what());
    } catch (...) {
        std::fprintf(stderr, "pipeline: unknown runtime failure\n");
    }
    return -1;
}
