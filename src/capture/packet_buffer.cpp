/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "capture_internal.h"

#include <limits>
#include <memory>
#include <new>

ktag_packet_pool *ktag_packet_pool_create_internal(uint32_t capacity)
{
    if (capacity == 0) {
        return nullptr;
    }
    try {
        std::unique_ptr<ktag_packet_pool> pool(new (std::nothrow) ktag_packet_pool());
        if (!pool) {
            return nullptr;
        }
        pool->capacity = capacity;
        pool->free_buffers.reserve(capacity);
        return pool.release();
    } catch (...) {
        return nullptr;
    }
}

ktag_status ktag_packet_pool_acquire_internal(
    ktag_packet_pool *pool,
    ktag_packet_buffer **buffer)
{
    if (pool == nullptr || buffer == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    *buffer = nullptr;

    std::lock_guard<std::mutex> lock(pool->mutex);
    if (pool->closing) {
        return KTAG_ERROR_INVALID_STATE;
    }

    ktag_packet_buffer *selected = nullptr;
    if (!pool->free_buffers.empty()) {
        selected = pool->free_buffers.back();
        pool->free_buffers.pop_back();
    } else {
        if (pool->allocated >= pool->capacity) {
            return KTAG_ERROR_CAPACITY;
        }
        selected = new (std::nothrow) ktag_packet_buffer();
        if (selected == nullptr) {
            return KTAG_ERROR_OUT_OF_MEMORY;
        }
        selected->pool = pool;
        ++pool->allocated;
    }

    selected->references.store(1U, std::memory_order_release);
    ++pool->outstanding;
    *buffer = selected;
    return KTAG_SUCCESS;
}

void ktag_packet_buffer_release_internal(ktag_packet_buffer *buffer)
{
    if (buffer == nullptr) {
        return;
    }
    if (buffer->references.fetch_sub(1U, std::memory_order_acq_rel) != 1U) {
        return;
    }

    ktag_packet_pool *pool = buffer->pool;
    if (pool == nullptr) {
        delete buffer;
        return;
    }

    bool delete_buffer = false;
    bool delete_pool = false;
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        if (pool->outstanding > 0) {
            --pool->outstanding;
        }
        if (pool->closing) {
            if (pool->allocated > 0) {
                --pool->allocated;
            }
            delete_buffer = true;
            delete_pool = pool->outstanding == 0;
        } else {
            pool->free_buffers.push_back(buffer);
        }
    }

    if (delete_buffer) {
        delete buffer;
    }
    if (delete_pool) {
        delete pool;
    }
}

void ktag_packet_pool_close_internal(ktag_packet_pool *pool)
{
    if (pool == nullptr) {
        return;
    }

    std::vector<ktag_packet_buffer *> free_buffers;
    bool delete_pool = false;
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->closing = true;
        free_buffers.swap(pool->free_buffers);
        pool->allocated -= static_cast<uint32_t>(free_buffers.size());
        delete_pool = pool->outstanding == 0;
    }

    for (auto *buffer : free_buffers) {
        delete buffer;
    }
    if (delete_pool) {
        delete pool;
    }
}

extern "C" {

KTAG_API void ktag_packet_init(ktag_packet *packet)
{
    if (packet == nullptr) {
        return;
    }
    *packet = {};
    packet->struct_size = sizeof(*packet);
    packet->api_version = KTAG_API_VERSION;
}

KTAG_API ktag_status ktag_packet_make_owned(ktag_packet *packet)
{
    if (packet == nullptr || packet->struct_size < sizeof(*packet) ||
        packet->api_version != KTAG_API_VERSION ||
        (packet->data == nullptr && packet->captured_length != 0)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (packet->owner != nullptr) {
        return KTAG_SUCCESS;
    }

    try {
        std::unique_ptr<ktag_packet_buffer> buffer(new (std::nothrow) ktag_packet_buffer());
        if (!buffer) {
            return KTAG_ERROR_OUT_OF_MEMORY;
        }
        if (packet->captured_length == 0U) {
            buffer->bytes.clear();
        } else {
            buffer->bytes.assign(packet->data, packet->data + packet->captured_length);
        }
        buffer->references.store(1U, std::memory_order_release);
        packet->data = buffer->bytes.empty() ? nullptr : buffer->bytes.data();
        packet->owner = buffer.release();
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API ktag_status ktag_packet_retain(ktag_packet *packet)
{
    if (packet == nullptr || packet->struct_size < sizeof(*packet) ||
        packet->api_version != KTAG_API_VERSION || packet->owner == nullptr) {
        return KTAG_ERROR_INVALID_STATE;
    }

    uint32_t references = packet->owner->references.load(std::memory_order_relaxed);
    while (references != 0 && references != std::numeric_limits<uint32_t>::max()) {
        if (packet->owner->references.compare_exchange_weak(
                references, references + 1U,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return KTAG_SUCCESS;
        }
    }
    return KTAG_ERROR_CAPACITY;
}

KTAG_API void ktag_packet_release(ktag_packet *packet)
{
    if (packet == nullptr) {
        return;
    }
    if (packet->struct_size < sizeof(*packet) ||
        packet->api_version != KTAG_API_VERSION) {
        return;
    }
    ktag_packet_buffer *owner = packet->owner;
    ktag_packet_init(packet);
    if (owner != nullptr) {
        ktag_packet_buffer_release_internal(owner);
    }
}

KTAG_API uint32_t ktag_packet_is_owned(const ktag_packet *packet)
{
    return packet != nullptr && packet->struct_size >= sizeof(*packet) &&
        packet->api_version == KTAG_API_VERSION && packet->owner != nullptr ? 1U : 0U;
}

} // extern "C"
