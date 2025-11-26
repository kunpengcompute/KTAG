/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "securec.h"
#include "process_feature.h"

const int HASH_BUCKETS = 10000;
const int CREATE_NODE = 1;
const int NOT_CREATE_NODE = 0;
const int UDP_FLOW_LIMIT = 100;
const int FLOW_IS_FINISHED = 1;
const int FORCE_CLOSE = 1;
const int NORMAL_CLOSE = 0;
uint32_t g_total_packet = 0;
uint32_t g_decode_packet = 0;

void process_error(KDADKResult type)
{
switch (type) {
        case KDADK_STS_PARAMETER_ERR:
            fprintf(stderr, "Invalid parameter. Please check for nullptr or value 0.\n");
            break;
        case KDADK_STS_MALLOC_FAILED:
            fprintf(stderr, "Malloc fail. Please check if there is enough memory.\n");
            break;
        case KDADK_STS_INIT_RWLOCK_FAIL:
            fprintf(stderr, "Rwlock init fail.\n");
            break;
        case KDADK_STS_INVALID_PCAP_PACKET:
            fprintf(stderr, "The packet in the pcap file is invalid.\n");
            break;
        case KDADK_STS_DECODE_HTTP_ERROR:
            fprintf(stderr, "Error occurs while decoding HTTP packet.\n");
            break;
        case KDADK_STS_DECODE_TLS_ERROR:
            fprintf(stderr, "Error occurs while decoding TLS packet.\n");
            break;
        default:
            break;
    }
}

flow_hash_node *process_tcp_packet(flow_hash *hash_table, packet_info* info)
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

flow_hash_node *process_udp_packet(flow_hash *hash_table, packet_info* info)
{
    flow_hash_node *node = hash_table_add(hash_table, info);
    if (node && (node->status == CONNECTED) &&
       (node->raw_data.packets_cnt + node->reverse_raw_data.packets_cnt) >= UDP_FLOW_LIMIT) {
        node = change_node_status(hash_table, &info->tuple, FORCE_CLOSE);
    }
    return node;
}

void process_rawbow(flow_hash *hash_table, packet_info* info, flow_hash_node *node, domain_name_hash *domain_name_table)
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

void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr, const u_char *packet)
{
    if (!packet || !pkthdr || pkthdr->caplen == 0) {
        process_error(KDADK_STS_INVALID_PCAP_PACKET);
        return;
    }
    pcap_handler_parameters *parameters = reinterpret_cast<pcap_handler_parameters *>(user_data);
    KDADKResult ret;
    flow_hash *hash_table = parameters->hash_table;
    domain_name_hash *domain_name_table = parameters->domain_name_table;
    flow_hash_node *node = nullptr;
    packet_info info{};
    info.packet_size = pkthdr->caplen;
    info.ts = pkthdr->ts;
    g_total_packet++;
    ret = decode_link_layer(packet, &info, parameters->link_type, domain_name_table);
    if (ret != KDADK_STS_NOT_SUPPORT) {
        g_decode_packet++;
    }
    if (ret) {
        process_error(ret);
        return;
    }

    switch (info.tuple.protocol) {
        case IPPROTO_TCP:
            node = process_tcp_packet(hash_table, &info);
            break;
        case IPPROTO_UDP:
            node = process_udp_packet(hash_table, &info);
            break;
        default:
            printf("only support TCP or UDP packet\n");
            return;
    }
    if (node == nullptr) {
        return;
    }

    process_rawbow(hash_table, &info, node, domain_name_table);

    if (info.finish_flag) {
        ret = remove_node_from_hash_bucket(hash_table, node);
        if (ret) {
            fprintf(stderr, "remove node from hash bucket fail\n");
            process_error(ret);
        }
        ret = feature_extract_node(node, parameters->feature_csv_file);
        if (ret != KDADK_STS_NO_ERR) {
            fprintf(stderr, "feature extract fail\n");
            process_error(ret);
        }
        free(node);
        node = nullptr;
    }
}

KDADKResult process_remaining_node(flow_hash *hash_table, std::string feature_csv_file)
{
    uint32_t I;
    flow_hash_node *node = nullptr;
    KDADKResult ret;
    for (I = 0; I < hash_table->bucket_num; ++i) {
        node = hash_table->buckets[i].nodes;
        while (node) {
            ret = feature_extract_node(node, feature_csv_file);
            if (ret) {
                return ret;
            }
            if (node->link_next) {
                node = node->link_next;
                free(node->link_pre);
            } else {
                free(node);
                node = nullptr;
            }
        }
        hash_table->buckets[i].nodes = nullptr;
    }
    return KDADK_STS_NO_ERR;
}

std::string init_csv_result_file(const char* filename)
{
    if (filename == nullptr) {
        return {};
    }

    std::string result_file(filename);
    size_t lastDot = result_file.find_last_of('.');
    if (lastDot != std::string::npos) {
        result_file.replace(lastDot + 1, result_file.length() - lastDot - 1, "csv");
    } else {
        result_file += ".csv";
    }
    if (std::filesystem::exists(result_file)) {
        std::filesystem::remove(result_file);
    }
    write_csv_header(result_file);
    return result_file;
}

int feature_extract(const char* pcap_file)
{
    flow_hash *hash_table = nullptr;
    domain_name_hash *domain_name_table = nullptr;
    pcap_handler_parameters parameters{};
    KDADKResult ret;

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *pcap = pcap_open_offline(pcap_file, errbuf);
    if (!pcap) {
        fprintf(stderr, "Error opening pcap file: %s\n", errbuf);
        return -1;
    }

    parameters.link_type = pcap_datalink(pcap);
    if (parameters.link_type != DLT_EN10MB && parameters.link_type != DLT_LINUX_SLL) {
        printf("only supports Ethernet DLT_EN10MB or DLT_LINUX_SLL\n");
        return -1;
    }

    // Init hash table
    ret = hash_table_create(&hash_table, HASH_BUCKETS);
    if (ret || hash_table == nullptr) {
        printf("hash table create fail\n");
        process_error(ret);
        return -1;
    }
    parameters.hash_table = hash_table;

    // Init domain_name pool
    ret = domain_name_table_create(&domain_name_table);
    if (ret || domain_name_table == nullptr) {
        printf("domain name table create fail\n");
        process_error(ret);
        return -1;
    }
    parameters.domain_name_table = domain_name_table;

    // Init feature csv result file
    parameters.feature_csv_file = init_csv_result_file(pcap_file);

    // Parse each pcap packet
    pcap_loop(pcap, 0, packet_handler, (u_char *)&parameters);
    printf("Total packets: %u, decode packets: %u \n", g_total_packet, g_decode_packet);
    pcap_close(pcap);
    process_remaining_node(hash_table, parameters.feature_csv_file);
    hash_table_destroy(hash_table);
    domain_name_table_destroy(domain_name_table);
    return 0;
}
