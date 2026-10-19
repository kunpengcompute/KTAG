/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "flow_hash.h"
#include "rawbow_token_dfa.h"
#include <cstdio>
#include <cstring>
#include <new>
#include <netinet/ip.h>
#include <unordered_set>
#include <algorithm>

#define ALIGN(s) (((s) + 63) & ~63)
const int ALIGN_SIZE = 64;

const int DJB2_HASH_SEED = 5381;
const int DJB2_HASH_SHIFT = 5;
const int MAX_TOKEN_LENGTH = 20;

typedef struct {
uint32_t ip_addr;
    uint16_t port;
    uint8_t proto;
    uint32_t ip_addr1;
    uint16_t port1;
} flow_hash_key;

void transfer_tuple_to_hash_key(const Tuple *tuple, flow_hash_key *key)
{
    if (tuple == nullptr || key == nullptr) {
        return;
    }
    if (tuple->src_ip <= tuple->dst_ip) {
        key->ip_addr = tuple->src_ip;
        key->ip_addr1 = tuple->dst_ip;
        key->port = tuple->src_port;
        key->port1 = tuple->dst_port;
    } else {
        key->ip_addr = tuple->dst_ip;
        key->ip_addr1 = tuple->src_ip;
        key->port = tuple->dst_port;
        key->port1 = tuple->src_port;
    }
    key->proto = tuple->protocol;
}

static uint32_t hash_func_djb2_hash32(const uint8_t *key)
{
    uint32_t hash = DJB2_HASH_SEED;

    for (uint32_t i = 0; i < IPV4_TUPLE_SIZE; i++) {
        hash = ((hash << DJB2_HASH_SHIFT) + hash) + key[i];
    }

    return hash;
}

KTAGResult hash_table_create(flow_hash **hash_table, uint32_t buckets)
{
    flow_hash *table = nullptr;
    int malloc_result;
    size_t malloc_size = 0;
    uint8_t *malloc_ptr = nullptr;

    if (hash_table == nullptr || buckets == 0) {
        return KTAG_STS_PARAMETER_ERR;
    }

    malloc_result = posix_memalign(reinterpret_cast<void **>(&(table)), ALIGN_SIZE, sizeof(flow_hash));
    if (malloc_result || table == nullptr) {
        return KTAG_STS_MALLOC_FAILED;
    }

    memset(table, 0, sizeof(flow_hash));
    table->bucket_num = buckets;
    table->build_ok = 0;

    malloc_size += ALIGN(sizeof(flow_hash_bucket) * buckets);
    malloc_result = posix_memalign(reinterpret_cast<void **>(&malloc_ptr), ALIGN_SIZE, malloc_size);
    if (malloc_result || malloc_ptr == nullptr) {
        free(table);
        return KTAG_STS_MALLOC_FAILED;
    }
    memset(malloc_ptr, 0, malloc_size);
    table->buckets = (flow_hash_bucket *)malloc_ptr;

    table->hash_func = hash_func_djb2_hash32;
    table->build_ok = 1;
    *hash_table = table;

    return KTAG_STS_NO_ERR;
}

static flow_hash_node *hash_table_match_node(flow_hash_node *node, const uint8_t *key)
{
    while (node) {
        if (memcmp(key, &node->tuple, IPV4_TUPLE_SIZE) != 0) {
            node = node->link_next;
            continue;
        }
        return node;
    }
    return nullptr;
}

void add_packet_to_flow_node(flow_hash_node *node, const packet_info *info)
{
    if (info->tuple.src_ip == node->flow_src_ip) {
        node->raw_data.header_len.push_back(info->header_len);
        node->raw_data.http_uris.push_back(info->http_uri);
        node->raw_data.http_hosts.push_back(info->http_host);
        node->raw_data.http_urls.push_back(info->http_url);
        node->raw_data.payload_len.push_back(info->payload_len);
        node->raw_data.ts.push_back(info->ts);
        if (info->tls_info.client_version != 0) {
            node->raw_data.tls_infos.push_back(info->tls_info);
        }
        node->raw_data.total_size += info->packet_size;
        node->raw_data.packets_cnt++;
    } else if (info->tuple.dst_ip == node->flow_src_ip) {
        node->reverse_raw_data.header_len.push_back(info->header_len);
        node->reverse_raw_data.http_uris.push_back(info->http_uri);
        node->reverse_raw_data.http_hosts.push_back(info->http_host);
        node->reverse_raw_data.http_urls.push_back(info->http_url);
        node->reverse_raw_data.payload_len.push_back(info->payload_len);
        node->reverse_raw_data.ts.push_back(info->ts);
        if (info->tls_info.client_version != 0) {
            node->reverse_raw_data.tls_infos.push_back(info->tls_info);
        }
        node->reverse_raw_data.total_size += info->packet_size;
        node->reverse_raw_data.packets_cnt++;
    } else {
        printf("ip addr is wrong\n");
    }
}

flow_hash_node *hash_table_add(flow_hash *hash_table, const packet_info *info)
{
    uint32_t hash;
    uint32_t bucket_idx;
    flow_hash_node *node = nullptr;
    flow_hash_key key{};

    if (hash_table == nullptr || info == nullptr) {
        return nullptr;
    }

    if (hash_table->buckets == nullptr) {
        return nullptr;
    }

    transfer_tuple_to_hash_key(&info->tuple, &key);
    hash = hash_table->hash_func((uint8_t *)&key);
    bucket_idx = hash % hash_table->bucket_num;

    node = hash_table->buckets[bucket_idx].nodes;
    node = hash_table_match_node(node, (uint8_t *)&key);
    if (node) {
        if (node->status == CONNECTED) {
            add_packet_to_flow_node(node, info);
        }
        return node;
    }

    // 若 流哈希表中不存在 该五元组，则新建节点并加入，同时设置 该流的src_ip为流的首个packet的src_ip
    node = new (std::nothrow) flow_hash_node{};
    if (node == nullptr) {
        return nullptr;
    }

    node->bucket_index = bucket_idx;
    node->link_next = hash_table->buckets[bucket_idx].nodes;

    if (hash_table->buckets[bucket_idx].nodes != nullptr)
        hash_table->buckets[bucket_idx].nodes->link_pre = node;
    hash_table->buckets[bucket_idx].nodes = node;
    memcpy(&node->tuple, &key, IPV4_TUPLE_SIZE);
    node->flow_src_ip = info->tuple.src_ip;

    hash_table->buckets[bucket_idx].node_num++;
    add_packet_to_flow_node(node, info);

    return node;
}

KTAGResult hash_table_destroy(flow_hash *hash_table)
{
    if (hash_table == nullptr)
        return KTAG_STS_NULL_PTR_ERR;

    if (hash_table->buckets != nullptr) {
        for (uint32_t index = 0; index < hash_table->bucket_num; ++index) {
            flow_hash_node *node = hash_table->buckets[index].nodes;
            while (node != nullptr) {
                flow_hash_node *next = node->link_next;
                delete node;
                node = next;
            }
            hash_table->buckets[index].nodes = nullptr;
            hash_table->buckets[index].node_num = 0U;
        }
        free(hash_table->buckets);
        hash_table->buckets = nullptr;
    }

    free(hash_table);
    return KTAG_STS_NO_ERR;
}

const std::unordered_set<char> DELIMITERS = {'/', '?', ',', '=', '&', '.', ':'};

const std::unordered_set<std::string> INVALID_WORDS = {
    "http", "https", "com", "cn", "www", "net", "org", "resolve", "host", "query", "api", "app",
    "ftp", "ftps", "ssh", "telnet", "smtp", "pop3", "imap", "dns", "dhcp", "tcp", "udp", "icmp", // 网络协议相关
    "edu", "gov", "mil", "int", "info", "biz", "name", "pro", "museum", "aero", "coop", "jobs", "travel", "mobi", "asia", "cat", "tel", "xxx", "post", "geo", // 顶级域名
    "us", "uk", "de", "fr", "jp", "kr", "ru", "au", "ca", "in", "br", "mx", "it", "es", "nl", "se", "no", "dk", "fi", "pl", "tr", "th", "sg", "hk", "tw" // 国家代码顶级域名
};

std::vector<std::string> tokenize_url(const std::string &input)
{
    std::vector<std::string> tokens;
    std::string current_token;

    for (char c : input) {
        if (DELIMITERS.find(c) != DELIMITERS.end()) {
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
        } else {
            current_token += c;
        }
    }

    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }

    return tokens;
}

bool is_all_digits(const std::string &s)
{
    return all_of(s.begin(), s.end(), [](char c) { return isdigit(c); });
}

bool is_single_alpha(const std::string& str)
{
    if (str.empty()) {
        return false;
    }

    int letterCount = 0;
    for (char c : str) {
        if (std::isalpha(c)) {
            letterCount++;
            if (letterCount > 1) return false; // 提前退出优化
        } else if (!(std::isdigit(c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return letterCount == 1;
}

std::vector<std::string> filter_tokens(const std::vector<std::string> &tokens)
{
    std::vector<std::string> filtered;

    for (const std::string &token : tokens) {
        if (token.length() > MAX_TOKEN_LENGTH || is_all_digits(token) || token.length() < 2 || is_single_alpha(token)) {
            continue;
        }

        // check whether the token is invalid
        std::string lower_token;
        lower_token.resize(token.size());
        transform(token.begin(), token.end(), lower_token.begin(), ::tolower);
        if (INVALID_WORDS.find(lower_token) != INVALID_WORDS.end()) {
            continue;
        }

        filtered.push_back(token);
    }
    return filtered;
}

void add_rawbow_to_node(flow_hash *hash_table, flow_hash_node *node, std::string message)
{
    (void)hash_table;
    if (node == nullptr || node->rawbow_cnt >= MAX_RAWBOW_CNT || message.empty()) {
        return;
    }

    std::vector<std::string> filtered_tokens = rawbow_dfa::collect_filtered_tokens(message, rawbow_dfa::kUnlimitedTokens);

    for (std::string token : filtered_tokens) {
        if (node->rawbow_cnt >= MAX_RAWBOW_CNT) {
            return;
        }
        node->rawbow[node->rawbow_cnt++] = token;
    }
}

KTAGResult remove_node_from_hash_bucket(flow_hash *hash_table, flow_hash_node *node)
{
    if (hash_table == nullptr || node == nullptr) {
        return KTAG_STS_NULL_PTR_ERR;
    }
    if (hash_table->buckets == nullptr || node->bucket_index >= hash_table->bucket_num) {
        return KTAG_STS_PARAMETER_ERR;
    }
    if (node->link_pre)
        node->link_pre->link_next = node->link_next;
    else
        hash_table->buckets[node->bucket_index].nodes = node->link_next;

    if (node->link_next)
        node->link_next->link_pre = node->link_pre;
    if (hash_table->buckets[node->bucket_index].node_num != 0U) {
        hash_table->buckets[node->bucket_index].node_num--;
    }
    node->link_pre = nullptr;
    node->link_next = nullptr;
    return KTAG_STS_NO_ERR;
}

flow_hash_node *change_node_status(flow_hash *hash_table, const Tuple *tuple, int force_close)
{
    uint32_t hash;
    uint32_t bucket_idx;
    flow_hash_node *node = nullptr;
    flow_hash_key key{};

    if (hash_table == nullptr || tuple == nullptr) {
        return nullptr;
    }

    transfer_tuple_to_hash_key(tuple, &key);
    hash = hash_table->hash_func((uint8_t *)&key);
    bucket_idx = hash % hash_table->bucket_num;

    if (!hash_table->buckets[bucket_idx].nodes) {
        return nullptr;
    }
    node = hash_table->buckets[bucket_idx].nodes;
    node = hash_table_match_node(node, (uint8_t *)&key);
    if (node == nullptr) {
        return nullptr;
    }

    if (force_close) {
        node->status = CLOSED;
    } else {
        switch (node->status) {
            case CONNECTED:
                node->status = CLOSING;
                break;
            case CLOSING:
                node->status = CLOSED;
                break;
            default:
                break;
        }
    }
    return node;
}
