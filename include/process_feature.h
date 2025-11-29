/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
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
} pcap_handler_parameters;

extern uint32_t g_total_packet;
extern uint32_t g_decode_packet;

void process_error(KDADKResult type);
flow_hash_node *process_tcp_packet(flow_hash *hash_table, packet_info *info);
flow_hash_node *process_udp_packet(flow_hash *hash_table, packet_info *info);
void process_rawbow(flow_hash *hash_table, packet_info *info, flow_hash_node *node,
                    domain_name_hash *domain_name_table);
void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr, const u_char *packet);
KDADKResult process_remaining_node(flow_hash *hash_table, std::string feature_csv_file);
std::string init_csv_result_file(const char *filename);
int feature_extract(const char *pcap_file);

#endif
