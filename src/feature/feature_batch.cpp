/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature.h"

#include <memory>
#include <new>
#include <utility>
#include <vector>

struct ktag_feature_record_storage {
    ktag_feature_record_metadata metadata{};
    std::vector<float> values;
};

struct ktag_feature_batch {
    uint64_t schema_hash = 0;
    uint32_t value_count = 0;
    std::vector<ktag_feature_record_storage> records;
};

extern "C" {

KTAG_API ktag_feature_batch *ktag_feature_batch_create(
    const ktag_feature_schema *schema,
    uint32_t initial_capacity)
{
    if (schema == nullptr || ktag_feature_schema_is_frozen(schema) == 0 ||
        ktag_feature_schema_get_value_count(schema) == 0) {
        return nullptr;
    }
    try {
        std::unique_ptr<ktag_feature_batch> batch(new (std::nothrow) ktag_feature_batch());
        if (batch == nullptr) {
            return nullptr;
        }
        batch->schema_hash = ktag_feature_schema_get_hash(schema);
        batch->value_count = ktag_feature_schema_get_value_count(schema);
        if (initial_capacity > 0) {
            batch->records.reserve(initial_capacity);
        }
        return batch.release();
    } catch (...) {
        return nullptr;
    }
}

KTAG_API void ktag_feature_batch_destroy(ktag_feature_batch *batch)
{
    delete batch;
}

KTAG_API ktag_status ktag_feature_batch_reserve(
    ktag_feature_batch *batch,
    uint32_t capacity)
{
    if (batch == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    try {
        batch->records.reserve(capacity);
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API ktag_status ktag_feature_batch_append(
    ktag_feature_batch *batch,
    const ktag_feature_record_metadata *metadata,
    const float *values,
    uint32_t value_count)
{
    if (batch == nullptr || metadata == nullptr || values == nullptr ||
        metadata->struct_size < sizeof(ktag_feature_record_metadata) ||
        metadata->api_version != KTAG_API_VERSION) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (value_count != batch->value_count) {
        return KTAG_ERROR_SCHEMA_MISMATCH;
    }
    try {
        ktag_feature_record_storage record;
        record.metadata = *metadata;
        record.values.assign(values, values + value_count);
        batch->records.push_back(std::move(record));
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API void ktag_feature_batch_clear(ktag_feature_batch *batch)
{
    if (batch != nullptr) {
        batch->records.clear();
    }
}

KTAG_API uint32_t ktag_feature_batch_get_count(const ktag_feature_batch *batch)
{
    return batch == nullptr ? 0U : static_cast<uint32_t>(batch->records.size());
}

KTAG_API uint32_t ktag_feature_batch_get_value_count(const ktag_feature_batch *batch)
{
    return batch == nullptr ? 0U : batch->value_count;
}

KTAG_API uint64_t ktag_feature_batch_get_schema_hash(const ktag_feature_batch *batch)
{
    return batch == nullptr ? 0U : batch->schema_hash;
}

KTAG_API ktag_status ktag_feature_batch_get_record(
    const ktag_feature_batch *batch,
    uint32_t index,
    ktag_feature_record *record)
{
    if (batch == nullptr || record == nullptr || index >= batch->records.size()) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const auto &stored = batch->records[index];
    record->struct_size = sizeof(ktag_feature_record);
    record->api_version = KTAG_API_VERSION;
    record->schema_hash = batch->schema_hash;
    record->metadata = stored.metadata;
    record->values = stored.values.data();
    record->value_count = batch->value_count;
    return KTAG_SUCCESS;
}

} // extern "C"
