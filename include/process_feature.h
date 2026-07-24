/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef PROCESS_FEATURE_H
#define PROCESS_FEATURE_H

#include <string>
#include <vector>

#include "decode.h"
#include "domain_name_hash.h"
#include "feature_extract.h"
#include "flow_hash.h"

typedef struct {
    flow_hash *hash_table;
    domain_name_hash *domain_name_table;
    std::string feature_csv_file;
    int link_type;
    std::vector<std::vector<float>> *data;
    std::vector<std::string> *rawbow;
} pcap_handler_parameters;

flow_hash_node *process_tcp_packet(flow_hash *hash_table, packet_info *info);
flow_hash_node *process_udp_packet(flow_hash *hash_table, packet_info *info);
void process_rawbow(flow_hash *hash_table, packet_info *info, flow_hash_node *node,
                    domain_name_hash *domain_name_table);

#endif
