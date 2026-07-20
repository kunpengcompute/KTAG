/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef DOMAIN_NAME_HASH_H
#define DOMAIN_NAME_HASH_H
#include <stdint.h>
#include <unordered_map>
#include <pthread.h>
#include "kdadk.h"
#include "ktag_feature.h"

struct domain_name_hash {
    /* Compatibility mirror; new code reads through the local DomainCache. */
    std::unordered_map<uint32_t, std::string> domain_name_buckets;
    int table_max_size = 10000;
    pthread_rwlock_t domain_name_rwlock{};
    bool domain_name_rwlock_initialized = false;
    bool respect_dns_ttl = false;
    ktag_domain_store *store = nullptr;
    ktag_domain_cache *cache = nullptr;
};

const int ALIGN_SIZE = 64;
const int DOMAIN_NAME_TABLE_MAX_SIZE = 10000;

KDADKResult domain_name_table_create(domain_name_hash **domain_name_table);
KDADKResult domain_name_table_create_with_store(
    domain_name_hash **domain_name_table,
    ktag_domain_store *store);
KDADKResult domain_name_table_add(domain_name_hash *domain_name_table, uint32_t key, std::string &value);
KDADKResult domain_name_table_add_with_expiry(
    domain_name_hash *domain_name_table,
    uint32_t key,
    const std::string &value,
    uint64_t expire_at_us);
KDADKResult domain_name_table_destroy(domain_name_hash *domain_name_table);
KDADKResult get_domain_by_ip(flow_hash_node *node, domain_name_hash *domain_name_table, bool &is_exist_domain);
KDADKResult get_domain_by_ip_at(
    flow_hash_node *node,
    domain_name_hash *domain_name_table,
    uint64_t now_us,
    bool &is_exist_domain);
KDADKResult domain_name_table_refresh_flow(
    domain_name_hash *domain_name_table,
    flow_hash_node *node,
    uint64_t now_us,
    bool &is_exist_domain);
KDADKResult domain_name_table_sync(domain_name_hash *domain_name_table);

#endif
