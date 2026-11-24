/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Megascan is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 */
#include <cstdio>
#include <cstring>
#include <new>
#include <algorithm>
#include "domain_name_hash.h"

KDADKResult domain_name_table_create(domain_name_hash **domain_name_table)
{
    if (domain_name_table == nullptr) {
        return KDADK_STS_PARAMETER_ERR;
    }

    domain_name_hash *table = nullptr;
    int init_result;

    table = new (std::nothrow) domain_name_hash();
    if (table == nullptr) {
        return KDADK_STS_MALLOC_FAILED;
    }
    table->table_max_size = DOMAIN_NAME_TABLE_MAX_SIZE;

    init_result = pthread_rwlock_init(&table->domain_name_rwlock, nullptr);
    if (init_result) {
        delete table;
        return KDADK_STS_INIT_RWLOCK_FAIL;
    }

    *domain_name_table = table;
    return KDADK_STS_NO_ERR;
}

KDADKResult domain_name_table_add(domain_name_hash *domain_name_table, uint32_t key, std::string &value)
{
    if (domain_name_table == nullptr) {
        return KDADK_STS_PARAMETER_ERR;
    }

    pthread_rwlock_wrlock(&domain_name_table->domain_name_rwlock);
    auto iter = domain_name_table->domain_name_buckets.find(key);
    if (iter == domain_name_table->domain_name_buckets.end() &&
        static_cast<int>(domain_name_table->domain_name_buckets.size()) >= domain_name_table->table_max_size) {
        pthread_rwlock_unlock(&domain_name_table->domain_name_rwlock);
        return KDADK_STS_VECTOR_INDEX_OVERFLOW;
    }
    (domain_name_table->domain_name_buckets)[key] = value;
    pthread_rwlock_unlock(&domain_name_table->domain_name_rwlock);

    return KDADK_STS_NO_ERR;
}

KDADKResult domain_name_table_destroy(domain_name_hash *domain_name_table)
{
    if (domain_name_table == nullptr) {
        return KDADK_STS_NULL_PTR_ERR;
    }

    pthread_rwlock_destroy(&domain_name_table->domain_name_rwlock);

    delete domain_name_table;
    return KDADK_STS_NO_ERR;
}

KDADKResult get_domain_by_ip(flow_hash_node *node, domain_name_hash *domain_name_table, bool &is_exist_domain)
{
    if (node == nullptr || domain_name_table == nullptr) {
        return KDADK_STS_PARAMETER_ERR;
    }

    pthread_rwlock_rdlock(&domain_name_table->domain_name_rwlock);
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
    pthread_rwlock_unlock(&domain_name_table->domain_name_rwlock);

    return KDADK_STS_NO_ERR;
}
