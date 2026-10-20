/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ktag_feature.h"

namespace {

ktag_ip_address ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    ktag_ip_address address{};
    address.bytes[0] = a;
    address.bytes[1] = b;
    address.bytes[2] = c;
    address.bytes[3] = d;
    address.version = 4U;
    return address;
}

ktag_ip_address ipv6(uint8_t last_byte)
{
    ktag_ip_address address{};
    address.bytes[0] = 0x20U;
    address.bytes[1] = 0x01U;
    address.bytes[2] = 0x0dU;
    address.bytes[3] = 0xb8U;
    address.bytes[15] = last_byte;
    address.version = 6U;
    return address;
}


ktag_domain_store *make_store(uint32_t max_entries, uint32_t journal_capacity)
{
    ktag_domain_store_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.max_entries = max_entries;
    config.journal_capacity = journal_capacity;
    return ktag_domain_store_create(&config);
}

ktag_domain_cache *make_cache(
    ktag_domain_store *store,
    bool authority_lookup_on_miss = true)
{
    ktag_domain_cache_config config{};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.authority_lookup_on_miss = authority_lookup_on_miss ? 1U : 0U;
    return ktag_domain_cache_create(store, &config);
}

ktag_status lookup(
    ktag_domain_cache *cache,
    const ktag_ip_address &address,
    uint64_t now_us,
    std::string *domain)
{
    char value[KTAG_DOMAIN_NAME_MAX_LENGTH + 1U] = {};
    uint32_t required_capacity = 0U;
    const ktag_status status = ktag_domain_cache_lookup(
        cache,
        &address,
        now_us,
        value,
        sizeof(value),
        &required_capacity);
    if (status == KTAG_SUCCESS) {
        *domain = value;
    }
    return status;
}

ktag_domain_cache_stats cache_stats(ktag_domain_cache *cache)
{
    ktag_domain_cache_stats stats{};
    stats.struct_size = sizeof(stats);
    stats.api_version = KTAG_API_VERSION;
    EXPECT_EQ(KTAG_SUCCESS, ktag_domain_cache_get_stats(cache, &stats));
    return stats;
}

ktag_domain_store_stats store_stats(ktag_domain_store *store)
{
    ktag_domain_store_stats stats{};
    stats.struct_size = sizeof(stats);
    stats.api_version = KTAG_API_VERSION;
    EXPECT_EQ(KTAG_SUCCESS, ktag_domain_store_get_stats(store, &stats));
    return stats;
}

} // namespace

TEST(KTAGDomainStore, PutOverwriteDeleteDeduplicateAndCapacity)
{
    ktag_domain_store *store = make_store(2U, 8U);
    ASSERT_NE(nullptr, store);
    ktag_domain_cache *cache = make_cache(store);
    ASSERT_NE(nullptr, cache);
    const auto first = ipv4(10U, 0U, 0U, 1U);
    const auto second = ipv6(2U);
    const auto third = ipv4(10U, 0U, 0U, 3U);

    uint64_t version = 0U;
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &first, "first.example.", 0U, &version));
    EXPECT_EQ(1U, version);
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &first, "first.example.", 0U, &version));
    EXPECT_EQ(1U, version);
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &first, "updated.example.", 0U, &version));
    EXPECT_EQ(2U, version);
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &second, "second.example.", 0U, &version));
    EXPECT_EQ(3U, version);
    EXPECT_EQ(KTAG_ERROR_CAPACITY, ktag_domain_store_put(
        store, &third, "third.example.", 0U, &version));
    EXPECT_EQ(3U, ktag_domain_store_get_version(store));

    std::string domain;
    EXPECT_EQ(KTAG_SUCCESS, lookup(cache, first, 0U, &domain));
    EXPECT_EQ("updated.example.", domain);
    auto stats = cache_stats(cache);
    EXPECT_EQ(3U, stats.applied_version);
    EXPECT_EQ(3U, stats.incremental_changes);
    EXPECT_EQ(0U, stats.snapshot_recoveries);

    uint32_t required_capacity = 0U;
    EXPECT_EQ(KTAG_ERROR_CAPACITY, ktag_domain_cache_lookup(
        cache, &second, 0U, nullptr, 0U, &required_capacity));
    EXPECT_EQ(std::strlen("second.example.") + 1U, required_capacity);

    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_delete(
        store, &first, &version));
    EXPECT_EQ(4U, version);
    EXPECT_EQ(KTAG_ERROR_NOT_FOUND, lookup(cache, first, 0U, &domain));
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_delete(
        store, &first, &version));
    EXPECT_EQ(4U, version);

    const auto store_state = store_stats(store);
    EXPECT_EQ(4U, store_state.version);
    EXPECT_EQ(1U, store_state.entry_count);
    EXPECT_EQ(1U, store_state.registered_cache_count);

    ktag_domain_cache_destroy(cache);
    ktag_domain_store_destroy(store);
}

TEST(KTAGDomainStore, ConcurrentWritersPublishAllEntries)
{
    constexpr uint32_t writer_count = 4U;
    constexpr uint32_t entries_per_writer = 32U;
    ktag_domain_store *store = make_store(256U, 8U);
    ASSERT_NE(nullptr, store);
    std::atomic<uint32_t> failures{0U};
    std::vector<std::thread> writers;
    for (uint32_t writer = 0U; writer < writer_count; ++writer) {
        writers.emplace_back([store, writer, &failures]() {
            for (uint32_t index = 0U; index < entries_per_writer; ++index) {
                const auto address = ipv4(
                    10U,
                    static_cast<uint8_t>(writer + 1U),
                    0U,
                    static_cast<uint8_t>(index + 1U));
                const std::string domain = "writer-" + std::to_string(writer) +
                    "-" + std::to_string(index) + ".example.";
                if (ktag_domain_store_put(
                        store, &address, domain.c_str(), 0U, nullptr) !=
                    KTAG_SUCCESS) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &writer : writers) {
        writer.join();
    }
    EXPECT_EQ(0U, failures.load(std::memory_order_relaxed));
    const auto state = store_stats(store);
    EXPECT_EQ(writer_count * entries_per_writer, state.entry_count);
    EXPECT_EQ(writer_count * entries_per_writer, state.version);
    EXPECT_EQ(0U, state.journal_count);

    ktag_domain_cache *cache = make_cache(store);
    ASSERT_NE(nullptr, cache);
    const auto address = ipv4(10U, 4U, 0U, 32U);
    std::string domain;
    EXPECT_EQ(KTAG_SUCCESS, lookup(cache, address, 0U, &domain));
    EXPECT_EQ("writer-3-31.example.", domain);

    ktag_domain_cache_destroy(cache);
    ktag_domain_store_destroy(store);
}

TEST(KTAGDomainCache, MultipleCachesApplyTtlDeleteIncrementally)
{
    ktag_domain_store *store = make_store(16U, 16U);
    ASSERT_NE(nullptr, store);
    ktag_domain_cache *first_cache = make_cache(store);
    ktag_domain_cache *second_cache = make_cache(store);
    ASSERT_NE(nullptr, first_cache);
    ASSERT_NE(nullptr, second_cache);
    const auto address = ipv4(192U, 0U, 2U, 10U);

    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &address, "ttl.example.", 100U, nullptr));
    std::string domain;
    EXPECT_EQ(KTAG_SUCCESS, lookup(first_cache, address, 99U, &domain));
    EXPECT_EQ("ttl.example.", domain);
    EXPECT_EQ(KTAG_ERROR_NOT_FOUND, lookup(first_cache, address, 100U, &domain));

    uint32_t deleted_count = 0U;
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_expire(
        store, 100U, &deleted_count));
    EXPECT_EQ(1U, deleted_count);
    EXPECT_EQ(KTAG_SUCCESS, ktag_domain_cache_sync(second_cache));
    EXPECT_EQ(KTAG_ERROR_NOT_FOUND, lookup(second_cache, address, 100U, &domain));
    const auto second_stats = cache_stats(second_cache);
    EXPECT_EQ(2U, second_stats.applied_version);
    EXPECT_EQ(2U, second_stats.incremental_changes);
    EXPECT_EQ(0U, second_stats.snapshot_recoveries);
    EXPECT_EQ(KTAG_SUCCESS, ktag_domain_cache_sync(first_cache));

    const auto state = store_stats(store);
    EXPECT_EQ(0U, state.entry_count);
    EXPECT_EQ(0U, state.journal_count);

    ktag_domain_cache_destroy(first_cache);
    ktag_domain_cache_destroy(second_cache);
    ktag_domain_store_destroy(store);
}

TEST(KTAGDomainCache, JournalOverwriteFallsBackToSnapshotThenReturnsToIncremental)
{
    ktag_domain_store *store = make_store(16U, 2U);
    ASSERT_NE(nullptr, store);
    ktag_domain_cache *slow_cache = make_cache(store, false);
    ASSERT_NE(nullptr, slow_cache);
    const auto first = ipv4(203U, 0U, 113U, 1U);
    const auto second = ipv4(203U, 0U, 113U, 2U);
    const auto third = ipv4(203U, 0U, 113U, 3U);

    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &first, "one.example.", 0U, nullptr));
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &second, "two.example.", 0U, nullptr));
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &third, "three.example.", 0U, nullptr));
    EXPECT_EQ(3U, ktag_domain_store_get_version(store));

    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_cache_sync(slow_cache));
    auto stats = cache_stats(slow_cache);
    EXPECT_EQ(3U, stats.applied_version);
    EXPECT_EQ(1U, stats.snapshot_recoveries);
    EXPECT_EQ(0U, stats.incremental_changes);

    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_store_put(
        store, &first, "one-new.example.", 0U, nullptr));
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_cache_sync(slow_cache));
    stats = cache_stats(slow_cache);
    EXPECT_EQ(4U, stats.applied_version);
    EXPECT_EQ(1U, stats.snapshot_recoveries);
    EXPECT_EQ(1U, stats.incremental_changes);
    EXPECT_EQ(0U, store_stats(store).journal_count);

    std::string domain;
    EXPECT_EQ(KTAG_SUCCESS, lookup(slow_cache, first, 0U, &domain));
    EXPECT_EQ("one-new.example.", domain);

    ktag_domain_cache_destroy(slow_cache);
    ktag_domain_store_destroy(store);
}

TEST(KTAGDomainCache, ConcurrentWriterAndOwnedReaderConverge)
{
    constexpr uint32_t entry_count = 128U;
    ktag_domain_store *store = make_store(256U, 16U);
    ASSERT_NE(nullptr, store);
    ktag_domain_cache *cache = make_cache(store, false);
    ASSERT_NE(nullptr, cache);

    std::atomic<uint32_t> write_failures{0U};
    std::atomic<bool> writer_done{false};
    std::thread writer([store, &write_failures, &writer_done]() {
        for (uint32_t index = 0U; index < entry_count; ++index) {
            const auto address = ipv4(
                198U, 51U, 100U, static_cast<uint8_t>(index + 1U));
            const std::string domain =
                "concurrent-" + std::to_string(index) + ".example.";
            if (ktag_domain_store_put(
                    store, &address, domain.c_str(), 0U, nullptr) !=
                KTAG_SUCCESS) {
                write_failures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    ktag_status sync_status = KTAG_SUCCESS;
    while (!writer_done.load(std::memory_order_acquire)) {
        sync_status = ktag_domain_cache_sync(cache);
        if (sync_status != KTAG_SUCCESS) {
            break;
        }
        std::this_thread::yield();
    }
    writer.join();
    ASSERT_EQ(KTAG_SUCCESS, sync_status);
    ASSERT_EQ(0U, write_failures.load(std::memory_order_relaxed));
    ASSERT_EQ(KTAG_SUCCESS, ktag_domain_cache_sync(cache));
    EXPECT_EQ(entry_count, ktag_domain_cache_get_applied_version(cache));

    for (uint32_t index = 0U; index < entry_count; ++index) {
        const auto address = ipv4(
            198U, 51U, 100U, static_cast<uint8_t>(index + 1U));
        const std::string expected =
            "concurrent-" + std::to_string(index) + ".example.";
        std::string actual;
        ASSERT_EQ(KTAG_SUCCESS, lookup(cache, address, 0U, &actual));
        EXPECT_EQ(expected, actual);
    }

    ktag_domain_cache_destroy(cache);
    ktag_domain_store_destroy(store);
}
