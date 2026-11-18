/*
 * Copyright (c) 2025-2025 Huawei Technologies Co., Ltd.
 */
#include "process_feature.h"

const int HASH_BUCKETS = 10000;
const int CREATE_NODE = 1;
const int NOT_CREATE_NODE = 0;
const int UDP_FLOW_LIMIT = 100;
const int FLOW_IS_FINISHED = 1;
const int FORCE_CLOSE = 1;
const int NORMAL_CLOSE = 0;

flow_hash_node *process_tcp_packet(flow_hash *hash_table, packet_info *info)
{
    flow_hash_node *node = nullptr;
    if (info->tcp_flag & (TH_RST)) {
        node = change_node_status(hash_table, &info->tuple, FORCE_CLOSE);
        info->finish_flag = FLOW_IS_FINISHED;
    } else if (info->tcp_flag & (TH_FIN)) {
        node = change_node_status(hash_table, &info->tuple, NORMAL_CLOSE);
    } else {
        node = hash_table_add(hash_table, info);
        if (node && node->status == CLOSED && (info->tcp_flag & (TH_ACK)))
            info->finish_flag = FLOW_IS_FINISHED;
    }
    return node;
}

flow_hash_node *process_udp_packet(flow_hash *hash_table, packet_info *info)
{
    flow_hash_node *node = hash_table_add(hash_table, info);
    if (node && (node->status == CONNECTED) &&
       (node->raw_data.packets_cnt + node->reverse_raw_data.packets_cnt) >= UDP_FLOW_LIMIT) {
        node = change_node_status(hash_table, &info->tuple, FORCE_CLOSE);
    }
    return node;
}

void process_rawbow(flow_hash *hash_table, packet_info *info, flow_hash_node *node,
                    domain_name_hash *domain_name_table)
{
    KDADKResult ret;
    if (node->rawbow_cnt < MAX_RAWBOW_CNT) {
        if (node->src_ip_domain_name.empty() && node->dst_ip_domain_name.empty()) {
            bool is_exist_domain = false;
            ret = get_domain_by_ip(node, domain_name_table, is_exist_domain);
            if (ret == KDADK_STS_NO_ERR && is_exist_domain == true) {
                add_rawbow_to_node(hash_table, node, node->src_ip_domain_name);
                add_rawbow_to_node(hash_table, node, node->dst_ip_domain_name);
            }
        }
        if (!info->http_url.empty()) {
            add_rawbow_to_node(hash_table, node, info->http_url);
        }
    }
}