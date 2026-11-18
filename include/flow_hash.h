/*
 * Copyright (c) 2025-2025 Huawei Technologies Co., Ltd.
 */
#ifndef FLOW_HASH_H
#define FLOW_HASH_H
#include <stdint.h>
#include <pthread.h>
#include "kdadk.h"
typedef uint32_t (*hash_calc_func)(const uint8_t *key);

typedef struct {
flow_hash_node *nodes;
    uint32_t node_num;
    pthread_rwlock_t rwlock;
} flow_hash_bucket;

typedef struct {
    uint32_t bucket_num;
    flow_hash_bucket *buckets;
    hash_calc_func hash_func;
    int build_ok;
} flow_hash;

KDADKResult hash_table_create(flow_hash **hash_table, uint32_t buckets);
flow_hash_node *hash_table_add(flow_hash *hash_table, const packet_info *info);
KDADKResult hash_table_destroy(flow_hash *hash_table);
void add_rawbow_to_node(flow_hash *hash_table, flow_hash_node *node, std::string message);
KDADKResult remove_node_from_hash_bucket(flow_hash *hash_table, flow_hash_node *node);
flow_hash_node *change_node_status(flow_hash *hash_table, const Tuple *tuple, int force_close);
void add_packet_to_flow_node(flow_hash_node *node, const packet_info *info);
bool is_all_digits(const std::string &s);
std::vector<std::string> filter_tokens(const std::vector<std::string> &tokens);
std::vector<std::string> tokenize_url(const std::string &input);

#endif
