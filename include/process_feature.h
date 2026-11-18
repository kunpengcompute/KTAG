/*
 * Copyright (c) 2025-2025 Huawei Technologies Co., Ltd.
 */
#ifndef PROCESS_FEATURE_H
#define PROCESS_FEATURE_H
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <pcap.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include "decode.h"
#include "flow_hash.h"
#include "domain_name_hash.h"
#include "feature_extract.h"

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
