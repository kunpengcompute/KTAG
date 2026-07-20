/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "domain_store_internal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace {

constexpr uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV1A_PRIME = 1099511628211ULL;

size_t journal_index(const ktag_domain_store *store, size_t logical_index)
{
    return (store->journal_head + logical_index) % store->journal.size();
}

const ktag_domain_change_storage &journal_at(
    const ktag_domain_store *store,
    size_t logical_index)
{
    return store->journal[journal_index(store, logical_index)];
}

void append_change_locked(
    ktag_domain_store *store,
    ktag_domain_change_storage *change)
{
    size_t slot_index = 0;
    if (store->journal_size < store->journal.size()) {
        slot_index = journal_index(store, store->journal_size);
        ++store->journal_size;
    } else {
        slot_index = store->journal_head;
        store->journal_head = (store->journal_head + 1U) % store->journal.size();
    }

    auto &slot = store->journal[slot_index];
    slot.version = change->version;
    slot.operation = change->operation;
    slot.address = change->address;
    slot.domain.swap(change->domain);
    slot.expire_at_us = change->expire_at_us;
}

void reclaim_journal_locked(ktag_domain_store *store)
{
    if (store->journal_size == 0U) {
        return;
    }
    if (store->cache_versions.empty()) {
        store->journal_head = 0U;
        store->journal_size = 0U;
        return;
    }

    uint64_t minimum_version = std::numeric_limits<uint64_t>::max();
    for (const auto &cache : store->cache_versions) {
        minimum_version = std::min(minimum_version, cache.second);
    }
    while (store->journal_size != 0U &&
           journal_at(store, 0U).version <= minimum_version) {
        store->journal_head = (store->journal_head + 1U) % store->journal.size();
        --store->journal_size;
    }
    if (store->journal_size == 0U) {
        store->journal_head = 0U;
    }
}

bool can_apply_incrementally_locked(
    const ktag_domain_store *store,
    uint64_t applied_version,
    uint64_t target_version)
{
    if (applied_version >= target_version || store->journal_size == 0U ||
        applied_version == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    uint64_t expected = applied_version + 1U;
    bool found_first = false;
    for (size_t index = 0; index < store->journal_size; ++index) {
        const uint64_t version = journal_at(store, index).version;
        if (version <= applied_version) {
            continue;
        }
        if (!found_first) {
            if (version != expected) {
                return false;
            }
            found_first = true;
        } else if (version != expected) {
            return false;
        }
        if (version == target_version) {
            return true;
        }
        if (version > target_version || expected == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        ++expected;
    }
    return false;
}

void apply_change(
    ktag_domain_map *local,
    const ktag_domain_change_storage &change)
{
    if (change.operation == KTAG_DOMAIN_DELETE) {
        local->erase(change.address);
        return;
    }
    auto position = local->find(change.address);
    if (position == local->end()) {
        local->emplace(
            change.address,
            ktag_domain_entry{change.domain, change.expire_at_us});
        return;
    }
    position->second.domain = change.domain;
    position->second.expire_at_us = change.expire_at_us;
}

ktag_status copy_domain(
    const std::string &value,
    char *domain,
    uint32_t domain_capacity,
    uint32_t *required_capacity)
{
    const uint32_t required = static_cast<uint32_t>(value.size() + 1U);
    *required_capacity = required;
    if (domain == nullptr || domain_capacity < required) {
        return KTAG_ERROR_CAPACITY;
    }
    std::memcpy(domain, value.c_str(), required);
    return KTAG_SUCCESS;
}

bool entry_is_expired(const ktag_domain_entry &entry, uint64_t now_us)
{
    return entry.expire_at_us != 0U && now_us >= entry.expire_at_us;
}

ktag_status sync_cache_locked(
    ktag_domain_cache *cache,
    uint64_t target_version)
{
    ktag_domain_store *store = cache->store;
    ++cache->sync_count;
    try {
        if (can_apply_incrementally_locked(
                store, cache->applied_version, target_version)) {
            uint64_t applied_changes = 0U;
            for (size_t index = 0; index < store->journal_size; ++index) {
                const auto &change = journal_at(store, index);
                if (change.version <= cache->applied_version) {
                    continue;
                }
                if (change.version > target_version) {
                    break;
                }
                apply_change(&cache->local, change);
                ++applied_changes;
            }
            cache->incremental_changes += applied_changes;
        } else {
            ktag_domain_map snapshot = store->authority;
            cache->local.swap(snapshot);
            ++cache->snapshot_recoveries;
        }
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }

    cache->applied_version = target_version;
    auto registered = store->cache_versions.find(cache->cache_id);
    if (registered == store->cache_versions.end()) {
        return KTAG_ERROR_INVALID_STATE;
    }
    registered->second = target_version;
    reclaim_journal_locked(store);
    return KTAG_SUCCESS;
}

bool valid_store_config(const ktag_domain_store_config *config)
{
    return config != nullptr && config->struct_size >= sizeof(*config) &&
        config->api_version == KTAG_API_VERSION;
}

bool valid_cache_config(const ktag_domain_cache_config *config)
{
    return config != nullptr && config->struct_size >= sizeof(*config) &&
        config->api_version == KTAG_API_VERSION;
}

} // namespace

size_t ktag_domain_address_hash::operator()(
    const ktag_domain_address_key &key) const noexcept
{
    uint64_t hash = FNV1A_OFFSET_BASIS;
    hash ^= key.version;
    hash *= FNV1A_PRIME;
    const size_t address_length = key.version == 4U ? 4U : key.bytes.size();
    for (size_t index = 0; index < address_length; ++index) {
        hash ^= key.bytes[index];
        hash *= FNV1A_PRIME;
    }
    return static_cast<size_t>(hash);
}

bool ktag_domain_address_is_valid_internal(const ktag_ip_address *address)
{
    if (address == nullptr || (address->version != 4U && address->version != 6U)) {
        return false;
    }
    if (address->version == 4U) {
        for (size_t index = 4U; index < KTAG_IPV6_ADDRESS_SIZE; ++index) {
            if (address->bytes[index] != 0U) {
                return false;
            }
        }
    }
    return true;
}

ktag_domain_address_key ktag_domain_make_key_internal(
    const ktag_ip_address &address)
{
    ktag_domain_address_key result;
    result.version = address.version;
    std::copy(
        address.bytes,
        address.bytes + KTAG_IPV6_ADDRESS_SIZE,
        result.bytes.begin());
    return result;
}

void ktag_domain_store_retain_internal(ktag_domain_store *store)
{
    if (store != nullptr) {
        store->references.fetch_add(1U, std::memory_order_relaxed);
    }
}

void ktag_domain_store_release_internal(ktag_domain_store *store)
{
    if (store != nullptr &&
        store->references.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        delete store;
    }
}

extern "C" {

KTAG_API ktag_domain_store *ktag_domain_store_create(
    const ktag_domain_store_config *config)
{
    if (!valid_store_config(config)) {
        return nullptr;
    }
    try {
        std::unique_ptr<ktag_domain_store> store(
            new (std::nothrow) ktag_domain_store());
        if (!store) {
            return nullptr;
        }
        store->max_entries = config->max_entries == 0U ?
            KTAG_DOMAIN_DEFAULT_MAX_ENTRIES : config->max_entries;
        const uint32_t journal_capacity = config->journal_capacity == 0U ?
            KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY : config->journal_capacity;
        store->journal.resize(journal_capacity);
        store->authority.reserve(std::min(store->max_entries, 1024U));
        return store.release();
    } catch (...) {
        return nullptr;
    }
}

KTAG_API void ktag_domain_store_destroy(ktag_domain_store *store)
{
    ktag_domain_store_release_internal(store);
}

KTAG_API ktag_status ktag_domain_store_put(
    ktag_domain_store *store,
    const ktag_ip_address *address,
    const char *domain,
    uint64_t expire_at_us,
    uint64_t *published_version)
{
    if (store == nullptr || !ktag_domain_address_is_valid_internal(address) ||
        domain == nullptr || domain[0] == '\0') {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const size_t domain_length = std::strlen(domain);
    if (domain_length > KTAG_DOMAIN_NAME_MAX_LENGTH) {
        return KTAG_ERROR_CAPACITY;
    }

    try {
        const auto key = ktag_domain_make_key_internal(*address);
        ktag_domain_entry proposed{std::string(domain), expire_at_us};
        ktag_domain_change_storage change;
        change.operation = KTAG_DOMAIN_PUT;
        change.address = key;
        change.domain = domain;
        change.expire_at_us = expire_at_us;

        std::lock_guard<std::mutex> lock(store->mutex);
        const uint64_t current_version = store->version.load(std::memory_order_relaxed);
        auto existing = store->authority.find(key);
        if (existing != store->authority.end() &&
            existing->second.domain == proposed.domain &&
            existing->second.expire_at_us == proposed.expire_at_us) {
            if (published_version != nullptr) {
                *published_version = current_version;
            }
            return KTAG_SUCCESS;
        }
        if (existing == store->authority.end() &&
            store->authority.size() >= store->max_entries) {
            return KTAG_ERROR_CAPACITY;
        }
        if (current_version == std::numeric_limits<uint64_t>::max()) {
            return KTAG_ERROR_CAPACITY;
        }

        if (existing == store->authority.end()) {
            store->authority.emplace(key, std::move(proposed));
        } else {
            existing->second.domain.swap(proposed.domain);
            existing->second.expire_at_us = expire_at_us;
        }
        change.version = current_version + 1U;
        append_change_locked(store, &change);
        store->version.store(current_version + 1U, std::memory_order_release);
        reclaim_journal_locked(store);
        if (published_version != nullptr) {
            *published_version = current_version + 1U;
        }
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API ktag_status ktag_domain_store_delete(
    ktag_domain_store *store,
    const ktag_ip_address *address,
    uint64_t *published_version)
{
    if (store == nullptr || !ktag_domain_address_is_valid_internal(address)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const auto key = ktag_domain_make_key_internal(*address);
    std::lock_guard<std::mutex> lock(store->mutex);
    const uint64_t current_version = store->version.load(std::memory_order_relaxed);
    auto existing = store->authority.find(key);
    if (existing == store->authority.end()) {
        if (published_version != nullptr) {
            *published_version = current_version;
        }
        return KTAG_SUCCESS;
    }
    if (current_version == std::numeric_limits<uint64_t>::max()) {
        return KTAG_ERROR_CAPACITY;
    }

    store->authority.erase(existing);
    ktag_domain_change_storage change;
    change.version = current_version + 1U;
    change.operation = KTAG_DOMAIN_DELETE;
    change.address = key;
    append_change_locked(store, &change);
    store->version.store(current_version + 1U, std::memory_order_release);
    reclaim_journal_locked(store);
    if (published_version != nullptr) {
        *published_version = current_version + 1U;
    }
    return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_domain_store_expire(
    ktag_domain_store *store,
    uint64_t now_us,
    uint32_t *deleted_count)
{
    if (store == nullptr || deleted_count == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    *deleted_count = 0U;
    std::lock_guard<std::mutex> lock(store->mutex);
    uint64_t current_version = store->version.load(std::memory_order_relaxed);
    for (auto position = store->authority.begin();
         position != store->authority.end();) {
        if (!entry_is_expired(position->second, now_us)) {
            ++position;
            continue;
        }
        if (current_version == std::numeric_limits<uint64_t>::max()) {
            if (*deleted_count != 0U) {
                store->version.store(current_version, std::memory_order_release);
                reclaim_journal_locked(store);
            }
            return KTAG_ERROR_CAPACITY;
        }
        ktag_domain_change_storage change;
        change.version = ++current_version;
        change.operation = KTAG_DOMAIN_DELETE;
        change.address = position->first;
        position = store->authority.erase(position);
        append_change_locked(store, &change);
        ++(*deleted_count);
    }
    if (*deleted_count != 0U) {
        store->version.store(current_version, std::memory_order_release);
        reclaim_journal_locked(store);
    }
    return KTAG_SUCCESS;
}

KTAG_API uint64_t ktag_domain_store_get_version(
    const ktag_domain_store *store)
{
    return store == nullptr ? 0U :
        store->version.load(std::memory_order_acquire);
}

KTAG_API ktag_status ktag_domain_store_get_stats(
    const ktag_domain_store *store,
    ktag_domain_store_stats *stats)
{
    if (store == nullptr || stats == nullptr ||
        stats->struct_size < sizeof(*stats) ||
        stats->api_version != KTAG_API_VERSION) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(store->mutex);
    stats->version = store->version.load(std::memory_order_relaxed);
    stats->entry_count = static_cast<uint32_t>(store->authority.size());
    stats->journal_count = static_cast<uint32_t>(store->journal_size);
    stats->journal_oldest_version = store->journal_size == 0U ?
        0U : journal_at(store, 0U).version;
    stats->registered_cache_count =
        static_cast<uint32_t>(store->cache_versions.size());
    return KTAG_SUCCESS;
}

KTAG_API ktag_domain_cache *ktag_domain_cache_create(
    ktag_domain_store *store,
    const ktag_domain_cache_config *config)
{
    if (store == nullptr || !valid_cache_config(config)) {
        return nullptr;
    }
    ktag_domain_store_retain_internal(store);
    try {
        std::unique_ptr<ktag_domain_cache> cache(
            new (std::nothrow) ktag_domain_cache());
        if (!cache) {
            ktag_domain_store_release_internal(store);
            return nullptr;
        }
        cache->store = store;
        cache->authority_lookup_on_miss =
            config->authority_lookup_on_miss != 0U;

        bool registered = false;
        {
            std::lock_guard<std::mutex> lock(store->mutex);
            if (store->next_cache_id != 0U) {
                cache->local = store->authority;
                cache->applied_version =
                    store->version.load(std::memory_order_relaxed);
                cache->cache_id = store->next_cache_id++;
                registered = store->cache_versions.emplace(
                    cache->cache_id, cache->applied_version).second;
                if (registered) {
                    reclaim_journal_locked(store);
                }
            }
        }
        if (!registered) {
            ktag_domain_store_release_internal(store);
            return nullptr;
        }
        return cache.release();
    } catch (...) {
        ktag_domain_store_release_internal(store);
        return nullptr;
    }
}

KTAG_API void ktag_domain_cache_destroy(ktag_domain_cache *cache)
{
    if (cache == nullptr) {
        return;
    }
    ktag_domain_store *store = cache->store;
    if (store != nullptr) {
        {
            std::lock_guard<std::mutex> lock(store->mutex);
            store->cache_versions.erase(cache->cache_id);
            reclaim_journal_locked(store);
        }
        cache->store = nullptr;
    }
    delete cache;
    ktag_domain_store_release_internal(store);
}

KTAG_API ktag_status ktag_domain_cache_sync(ktag_domain_cache *cache)
{
    if (cache == nullptr || cache->store == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    ktag_domain_store *store = cache->store;
    const uint64_t published_version =
        store->version.load(std::memory_order_acquire);
    if (published_version == cache->applied_version) {
        return KTAG_SUCCESS;
    }
    std::lock_guard<std::mutex> lock(store->mutex);
    const uint64_t target_version =
        store->version.load(std::memory_order_relaxed);
    if (target_version == cache->applied_version) {
        return KTAG_SUCCESS;
    }
    return sync_cache_locked(cache, target_version);
}

KTAG_API ktag_status ktag_domain_cache_lookup(
    ktag_domain_cache *cache,
    const ktag_ip_address *address,
    uint64_t now_us,
    char *domain,
    uint32_t domain_capacity,
    uint32_t *required_capacity)
{
    if (cache == nullptr || cache->store == nullptr ||
        !ktag_domain_address_is_valid_internal(address) ||
        required_capacity == nullptr || (domain == nullptr && domain_capacity != 0U)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    *required_capacity = 0U;
    const ktag_status sync_status = ktag_domain_cache_sync(cache);
    if (sync_status != KTAG_SUCCESS) {
        return sync_status;
    }

    const auto key = ktag_domain_make_key_internal(*address);
    auto local = cache->local.find(key);
    if (local != cache->local.end() &&
        !entry_is_expired(local->second, now_us)) {
        ++cache->local_hits;
        return copy_domain(
            local->second.domain,
            domain,
            domain_capacity,
            required_capacity);
    }

    ++cache->local_misses;
    if (!cache->authority_lookup_on_miss) {
        return KTAG_ERROR_NOT_FOUND;
    }

    ++cache->authority_lookups;
    try {
        std::lock_guard<std::mutex> lock(cache->store->mutex);
        const auto authority = cache->store->authority.find(key);
        if (authority == cache->store->authority.end() ||
            entry_is_expired(authority->second, now_us)) {
            return KTAG_ERROR_NOT_FOUND;
        }
        cache->local[key] = authority->second;
        ++cache->authority_hits;
        return copy_domain(
            authority->second.domain,
            domain,
            domain_capacity,
            required_capacity);
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API uint64_t ktag_domain_cache_get_applied_version(
    const ktag_domain_cache *cache)
{
    return cache == nullptr ? 0U : cache->applied_version;
}

KTAG_API ktag_status ktag_domain_cache_get_stats(
    const ktag_domain_cache *cache,
    ktag_domain_cache_stats *stats)
{
    if (cache == nullptr || stats == nullptr ||
        stats->struct_size < sizeof(*stats) ||
        stats->api_version != KTAG_API_VERSION) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    stats->applied_version = cache->applied_version;
    stats->local_hits = cache->local_hits;
    stats->local_misses = cache->local_misses;
    stats->sync_count = cache->sync_count;
    stats->incremental_changes = cache->incremental_changes;
    stats->snapshot_recoveries = cache->snapshot_recoveries;
    stats->authority_lookups = cache->authority_lookups;
    stats->authority_hits = cache->authority_hits;
    return KTAG_SUCCESS;
}

} // extern "C"
