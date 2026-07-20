/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DOMAIN_STORE_INTERNAL_H
#define KTAG_DOMAIN_STORE_INTERNAL_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ktag_feature.h"

struct ktag_domain_address_key {
    std::array<uint8_t, KTAG_IPV6_ADDRESS_SIZE> bytes{};
    uint8_t version = 0;

    bool operator==(const ktag_domain_address_key &other) const
    {
        return version == other.version && bytes == other.bytes;
    }
};

struct ktag_domain_address_hash {
    size_t operator()(const ktag_domain_address_key &key) const noexcept;
};

struct ktag_domain_entry {
    std::string domain;
    uint64_t expire_at_us = 0;
};

struct ktag_domain_change_storage {
    uint64_t version = 0;
    ktag_domain_operation operation = KTAG_DOMAIN_PUT;
    ktag_domain_address_key address;
    std::string domain;
    uint64_t expire_at_us = 0;
};

using ktag_domain_map = std::unordered_map<
    ktag_domain_address_key,
    ktag_domain_entry,
    ktag_domain_address_hash>;

struct ktag_domain_store {
    std::atomic<uint32_t> references{1U};
    std::atomic<uint64_t> version{0U};
    mutable std::mutex mutex;
    ktag_domain_map authority;
    std::vector<ktag_domain_change_storage> journal;
    size_t journal_head = 0;
    size_t journal_size = 0;
    uint32_t max_entries = KTAG_DOMAIN_DEFAULT_MAX_ENTRIES;
    std::unordered_map<uint64_t, uint64_t> cache_versions;
    uint64_t next_cache_id = 1U;
};

struct ktag_domain_cache {
    ktag_domain_store *store = nullptr;
    uint64_t cache_id = 0;
    ktag_domain_map local;
    uint64_t applied_version = 0;
    bool authority_lookup_on_miss = false;
    uint64_t local_hits = 0;
    uint64_t local_misses = 0;
    uint64_t sync_count = 0;
    uint64_t incremental_changes = 0;
    uint64_t snapshot_recoveries = 0;
    uint64_t authority_lookups = 0;
    uint64_t authority_hits = 0;
};

bool ktag_domain_address_is_valid_internal(const ktag_ip_address *address);

ktag_domain_address_key ktag_domain_make_key_internal(
    const ktag_ip_address &address);

void ktag_domain_store_retain_internal(ktag_domain_store *store);

void ktag_domain_store_release_internal(ktag_domain_store *store);

#endif /* KTAG_DOMAIN_STORE_INTERNAL_H */
