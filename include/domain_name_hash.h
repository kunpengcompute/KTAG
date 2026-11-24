/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Megascan is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 */
#ifndef DOMAIN_NAME_HASH_H
#define DOMAIN_NAME_HASH_H
#include <stdint.h>
#include <unordered_map>
#include <pthread.h>
#include "kdadk.h"
struct domain_name_hash {
std::unordered_map<uint32_t, std::string> domain_name_buckets;
    int table_max_size;
    pthread_rwlock_t domain_name_rwlock;
};

const int ALIGN_SIZE = 64;
const int DOMAIN_NAME_TABLE_MAX_SIZE = 10000;

KDADKResult domain_name_table_create(domain_name_hash **domain_name_table);
KDADKResult domain_name_table_add(domain_name_hash *domain_name_table, uint32_t key, std::string &value);
KDADKResult domain_name_table_destroy(domain_name_hash *domain_name_table);
KDADKResult get_domain_by_ip(flow_hash_node *node, domain_name_hash *domain_name_table, bool &is_exist_domain);

#endif
