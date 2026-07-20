/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "prediction_batch_internal.h"

#include <memory>
#include <new>

extern "C" {

KTAG_API ktag_prediction_batch *ktag_prediction_batch_create(
    uint32_t initial_capacity)
{
    try {
        std::unique_ptr<ktag_prediction_batch> batch(
            new (std::nothrow) ktag_prediction_batch());
        if (!batch) {
            return nullptr;
        }
        if (initial_capacity != 0U) {
            batch->records.reserve(initial_capacity);
        }
        return batch.release();
    } catch (...) {
        return nullptr;
    }
}

KTAG_API void ktag_prediction_batch_destroy(ktag_prediction_batch *batch)
{
    delete batch;
}

KTAG_API ktag_status ktag_prediction_batch_reserve(
    ktag_prediction_batch *batch,
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

KTAG_API void ktag_prediction_batch_clear(ktag_prediction_batch *batch)
{
    if (batch != nullptr) {
        batch->records.clear();
    }
}

KTAG_API uint32_t ktag_prediction_batch_get_count(
    const ktag_prediction_batch *batch)
{
    return batch == nullptr ?
        0U : static_cast<uint32_t>(batch->records.size());
}

KTAG_API ktag_status ktag_prediction_batch_get_record(
    const ktag_prediction_batch *batch,
    uint32_t index,
    ktag_prediction_record *record)
{
    if (batch == nullptr || record == nullptr ||
        index >= batch->records.size()) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    *record = batch->records[index];
    return KTAG_SUCCESS;
}

} // extern "C"
