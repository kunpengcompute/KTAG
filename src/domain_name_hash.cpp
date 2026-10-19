/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
*/
#include "domain_name_hash.h"

#include <cstring>
#include <memory>
#include <new>

namespace {

ktag_ip_address make_ipv4_address(uint32_t network_order_address)
{
    ktag_ip_address address{};
    std::memcpy(address.bytes, &network_order_address, sizeof(network_order_address));
    address.version = 4U;
    return address;
}

KTAGResult translate_status(ktag_status status)
{
    switch (status) {
        case KTAG_SUCCESS:
            return KTAG_STS_NO_ERR;
        case KTAG_ERROR_INVALID_ARGUMENT:
            return KTAG_STS_PARAMETER_ERR;
        case KTAG_ERROR_OUT_OF_MEMORY:
            return KTAG_STS_MALLOC_FAILED;
        case KTAG_ERROR_CAPACITY:
            return KTAG_STS_VECTOR_INDEX_OVERFLOW;
        default:
            return KTAG_STS_UNKNOWN_ERR;
    }
}

bool legacy_table_has_capacity(
    const domain_name_hash *table,
    uint32_t key)
{
    return table->domain_name_buckets.find(key) != table->domain_name_buckets.end() ||
        static_cast<int>(table->domain_name_buckets.size()) < table->table_max_size;
}

void legacy_read_lock(domain_name_hash *table)
{
    if (table->domain_name_rwlock_initialized) {
        pthread_rwlock_rdlock(&table->domain_name_rwlock);
    }
}

void legacy_write_lock(domain_name_hash *table)
{
    if (table->domain_name_rwlock_initialized) {
        pthread_rwlock_wrlock(&table->domain_name_rwlock);
    }
}

void legacy_unlock(domain_name_hash *table)
{
    if (table->domain_name_rwlock_initialized) {
        pthread_rwlock_unlock(&table->domain_name_rwlock);
    }
}

KTAGResult lookup_cache(
    ktag_domain_cache *cache,
    uint32_t network_order_address,
    uint64_t now_us,
    std::string *domain,
    bool *found)
{
    *found = false;
    const ktag_ip_address address = make_ipv4_address(network_order_address);
    char value[KTAG_DOMAIN_NAME_MAX_LENGTH + 1U] = {};
    uint32_t required_capacity = 0U;
    const ktag_status status = ktag_domain_cache_lookup(
        cache,
        &address,
        now_us,
        value,
        sizeof(value),
        &required_capacity);
    if (status == KTAG_ERROR_NOT_FOUND) {
        return KTAG_STS_NO_ERR;
    }
    if (status != KTAG_SUCCESS) {
        return translate_status(status);
    }
    try {
        *domain = value;
        *found = true;
        return KTAG_STS_NO_ERR;
    } catch (const std::bad_alloc &) {
        return KTAG_STS_MALLOC_FAILED;
    } catch (...) {
        return KTAG_STS_UNKNOWN_ERR;
    }
}

} // namespace

KTAGResult domain_name_table_create_with_store(
    domain_name_hash **domain_name_table,
    ktag_domain_store *store)
{
    if (domain_name_table == nullptr || store == nullptr) {
        return KTAG_STS_PARAMETER_ERR;
    }
    *domain_name_table = nullptr;

    std::unique_ptr<domain_name_hash> table(
        new (std::nothrow) domain_name_hash());
    if (!table) {
        return KTAG_STS_MALLOC_FAILED;
    }
    table->table_max_size = DOMAIN_NAME_TABLE_MAX_SIZE;
    if (pthread_rwlock_init(&table->domain_name_rwlock, nullptr) != 0) {
        return KTAG_STS_INIT_RWLOCK_FAIL;
    }
    table->domain_name_rwlock_initialized = true;
    table->respect_dns_ttl = true;

    ktag_domain_cache_config cache_config{};
    cache_config.struct_size = sizeof(cache_config);
    cache_config.api_version = KTAG_API_VERSION;
    cache_config.authority_lookup_on_miss = 1U;
    table->cache = ktag_domain_cache_create(store, &cache_config);
    if (table->cache == nullptr) {
        pthread_rwlock_destroy(&table->domain_name_rwlock);
        table->domain_name_rwlock_initialized = false;
        return KTAG_STS_MALLOC_FAILED;
    }
    table->store = store;
    *domain_name_table = table.release();
    return KTAG_STS_NO_ERR;
}

KTAGResult domain_name_table_create(domain_name_hash **domain_name_table)
{
    if (domain_name_table == nullptr) {
        return KTAG_STS_PARAMETER_ERR;
    }

    ktag_domain_store_config store_config{};
    store_config.struct_size = sizeof(store_config);
    store_config.api_version = KTAG_API_VERSION;
    store_config.max_entries = DOMAIN_NAME_TABLE_MAX_SIZE;
    store_config.journal_capacity = KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY;
    ktag_domain_store *store = ktag_domain_store_create(&store_config);
    if (store == nullptr) {
        return KTAG_STS_MALLOC_FAILED;
    }
    const KTAGResult result =
        domain_name_table_create_with_store(domain_name_table, store);
    ktag_domain_store_destroy(store);
    if (result == KTAG_STS_NO_ERR && *domain_name_table != nullptr) {
        /* Preserve the legacy extractor's never-expire behavior. */
        (*domain_name_table)->respect_dns_ttl = false;
    }
    return result;
}

KTAGResult domain_name_table_add_with_expiry(
    domain_name_hash *domain_name_table,
    uint32_t key,
    const std::string &value,
    uint64_t expire_at_us)
{
    if (domain_name_table == nullptr) {
        return KTAG_STS_PARAMETER_ERR;
    }

    legacy_read_lock(domain_name_table);
    const bool has_capacity = legacy_table_has_capacity(domain_name_table, key);
    legacy_unlock(domain_name_table);
    if (!has_capacity) {
        return KTAG_STS_VECTOR_INDEX_OVERFLOW;
    }

    if (domain_name_table->store != nullptr) {
        const ktag_ip_address address = make_ipv4_address(key);
        const uint64_t effective_expire_at_us =
            domain_name_table->respect_dns_ttl ? expire_at_us : 0U;
        const ktag_status put_status = ktag_domain_store_put(
            domain_name_table->store,
            &address,
            value.c_str(),
            effective_expire_at_us,
            nullptr);
        if (put_status != KTAG_SUCCESS) {
            return translate_status(put_status);
        }
        if (domain_name_table->cache != nullptr) {
            const ktag_status sync_status =
                ktag_domain_cache_sync(domain_name_table->cache);
            if (sync_status != KTAG_SUCCESS) {
                return translate_status(sync_status);
            }
        }
    }

    try {
        legacy_write_lock(domain_name_table);
        domain_name_table->domain_name_buckets[key] = value;
        legacy_unlock(domain_name_table);
        return KTAG_STS_NO_ERR;
    } catch (const std::bad_alloc &) {
        legacy_unlock(domain_name_table);
        return KTAG_STS_MALLOC_FAILED;
    } catch (...) {
        legacy_unlock(domain_name_table);
        return KTAG_STS_UNKNOWN_ERR;
    }
}

KTAGResult domain_name_table_add(
    domain_name_hash *domain_name_table,
    uint32_t key,
    std::string &value)
{
    return domain_name_table_add_with_expiry(
        domain_name_table, key, value, 0U);
}

KTAGResult domain_name_table_destroy(domain_name_hash *domain_name_table)
{
    if (domain_name_table == nullptr) {
        return KTAG_STS_NULL_PTR_ERR;
    }
    if (domain_name_table->cache != nullptr) {
        ktag_domain_cache_destroy(domain_name_table->cache);
        domain_name_table->cache = nullptr;
        domain_name_table->store = nullptr;
    }
    if (domain_name_table->domain_name_rwlock_initialized) {
        pthread_rwlock_destroy(&domain_name_table->domain_name_rwlock);
        domain_name_table->domain_name_rwlock_initialized = false;
    }
    delete domain_name_table;
    return KTAG_STS_NO_ERR;
}

KTAGResult get_domain_by_ip_at(
    flow_hash_node *node,
    domain_name_hash *domain_name_table,
    uint64_t now_us,
    bool &is_exist_domain)
{
    if (node == nullptr || domain_name_table == nullptr) {
        return KTAG_STS_PARAMETER_ERR;
    }

    if (domain_name_table->cache != nullptr) {
        bool found = false;
        KTAGResult result = lookup_cache(
            domain_name_table->cache,
            node->tuple.dst_ip,
            now_us,
            &node->dst_ip_domain_name,
            &found);
        if (result != KTAG_STS_NO_ERR) {
            return result;
        }
        if (found) {
            is_exist_domain = true;
        }
        result = lookup_cache(
            domain_name_table->cache,
            node->tuple.src_ip,
            now_us,
            &node->src_ip_domain_name,
            &found);
        if (result != KTAG_STS_NO_ERR) {
            return result;
        }
        if (found) {
            is_exist_domain = true;
        }
        return KTAG_STS_NO_ERR;
    }

    legacy_read_lock(domain_name_table);
    auto iter_dst = domain_name_table->domain_name_buckets.find(node->tuple.dst_ip);
    if (iter_dst != domain_name_table->domain_name_buckets.end()) {
        node->dst_ip_domain_name = iter_dst->second;
        is_exist_domain = true;
    }
    auto iter_src = domain_name_table->domain_name_buckets.find(node->tuple.src_ip);
    if (iter_src != domain_name_table->domain_name_buckets.end()) {
        node->src_ip_domain_name = iter_src->second;
        is_exist_domain = true;
    }
    legacy_unlock(domain_name_table);
    return KTAG_STS_NO_ERR;
}

KTAGResult get_domain_by_ip(
    flow_hash_node *node,
    domain_name_hash *domain_name_table,
    bool &is_exist_domain)
{
    return get_domain_by_ip_at(
        node, domain_name_table, 0U, is_exist_domain);
}

KTAGResult domain_name_table_refresh_flow(
    domain_name_hash *domain_name_table,
    flow_hash_node *node,
    uint64_t now_us,
    bool &is_exist_domain)
{
    if (node == nullptr || domain_name_table == nullptr) {
        return KTAG_STS_PARAMETER_ERR;
    }
    node->src_ip_domain_name.clear();
    node->dst_ip_domain_name.clear();
    is_exist_domain = false;
    return get_domain_by_ip_at(
        node, domain_name_table, now_us, is_exist_domain);
}

KTAGResult domain_name_table_sync(domain_name_hash *domain_name_table)
{
    if (domain_name_table == nullptr) {
        return KTAG_STS_PARAMETER_ERR;
    }
    if (domain_name_table->cache == nullptr) {
        return KTAG_STS_NO_ERR;
    }
    return translate_status(ktag_domain_cache_sync(domain_name_table->cache));
}
