/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef KDADK_TEST_H
#define KDADK_TEST_H

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <math.h>
#include <filesystem>
#include "domain_name_hash.h"
#include "decode.h"
#include "flow_hash.h"
#include <pcap.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip.h>
#include <net/ethernet.h>
#include "kdadk.h"
#include <random>
#include <ctime>
#include <dirent.h>
#include <string>
#include <cstring>
#include <libgen.h>
#include <linux/limits.h>

#define NUM_TESTS 1000000

// 获取可执行文件所在目录
inline std::string GetExecutableDir() {
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len != -1) {
        exePath[len] = '\0';
        return std::string(dirname(exePath));
    }
    return ".";
}

// 获取第一个 pcap 文件
inline std::string GetFirstPcapFile() {
    // 从可执行文件目录向上查找 test/traces
    std::string exeDir = GetExecutableDir();
    std::string tracesDir = exeDir + "/../../../test/traces";
    
    // 规范化路径
    char realPath[PATH_MAX];
    if (realpath(tracesDir.c_str(), realPath) == nullptr) {
        // 如果相对路径失败，尝试从当前工作目录
        tracesDir = "./test/traces";
        if (realpath(tracesDir.c_str(), realPath) == nullptr) {
            return "";
        }
    }
    
    DIR* dir = opendir(realPath);
    if (!dir) {
        return "";
    }
    
    struct dirent* entry;
    std::string firstPcap;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len > 5 && strcmp(name + len - 5, ".pcap") == 0) {
            firstPcap = std::string(realPath) + "/" + name;
            break;
        }
    }
    closedir(dir);
    return firstPcap;
}
#define TEST_TRACE GetFirstPcapFile().c_str()

inline double calculate_time_ms(struct timespec start, struct timespec end)
{
    return (end.tv_sec - start.tv_sec) * 1e3 + (end.tv_nsec - start.tv_nsec) / 1e6;
}

const int HASH_BUCKETS = 10000;
const int CREATE_NODE = 1;
const int NOT_CREATE_NODE = 0;
const int UDP_FLOW_LIMIT = 50;
const int FLOW_IS_FINISHED = 1;
const int FORCE_CLOSE = 1;
const int NORMAL_CLOSE = 0;
const int PARAMETERS_NUM = 2;

typedef struct {
    flow_hash *hash_table;
    domain_name_hash *domain_name_table;
    std::string feature_csv_file;
    int link_type;
    std::vector<std::vector<float>> *data;
    std::vector<std::string> *rawbow;
} pcap_handler_parameters;

typedef struct {
    const u_char *packet;
    packet_info *info;
    int link_type;
    domain_name_hash *domain_name_table;
} packet_test;

typedef struct {
    packet_test *packet_ret;
    flow_hash *hash_table;
    pcap_t *pcap;
} pcap_info;

static void process_error_test(KDADKResult type)
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

static packet_test packet_handler_get(u_char *user_data, const struct pcap_pkthdr *pkthdr, const u_char *packet)
{
    pcap_handler_parameters *parameters = reinterpret_cast<pcap_handler_parameters *>(user_data);
    domain_name_hash *domain_name_table = parameters->domain_name_table;
    packet_info info{};
    info.packet_size = pkthdr->caplen;
    info.ts = pkthdr->ts;
    packet_test packet_ret;
    packet_ret.packet = packet;
    packet_ret.info = &info;
    packet_ret.link_type = parameters->link_type;
    packet_ret.domain_name_table = domain_name_table;
    return packet_ret;
}

static inline pcap_info get_packet()
{   
    flow_hash *hash_table = nullptr;
    domain_name_hash *domain_name_table = nullptr;
    pcap_handler_parameters parameters{};
    KDADKResult ret;

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *pcap = pcap_open_offline(TEST_TRACE, errbuf);
    if (!pcap) {
        fprintf(stderr, "Error opening pcap file: %s\n", errbuf);
    }
    struct pcap_pkthdr* header;
    const u_char* packet;
    int result = pcap_next_ex(pcap, &header, &packet);
    
    if (result == 1) {
        std::cout << "成功获取第一个数据包!" << std::endl;
        std::cout << "数据包长度: " << header->len << " 字节" << std::endl;
        std::cout << "捕获长度: " << header->caplen << " 字节" << std::endl;
        std::cout << "时间戳: " << header->ts.tv_sec << "." << header->ts.tv_usec << std::endl;
        
        // 这里可以添加对packet数据的解析代码
    } else if (result == 0) {
        std::cerr << "读取超时，没有获取到数据包" << std::endl;
    } else {
        std::cerr << "读取数据包失败" << std::endl;
    }

    parameters.link_type = pcap_datalink(pcap);
    if (parameters.link_type != DLT_EN10MB && parameters.link_type != DLT_LINUX_SLL) {
        printf("only supports Ethernet DLT_EN10MB or DLT_LINUX_SLL\n");
    }

    // Init hash table
    ret = hash_table_create(&hash_table, HASH_BUCKETS);
    if (ret || hash_table == nullptr) {
        printf("hash table create fail\n");
        process_error_test(ret);

    }
    parameters.hash_table = hash_table;

    // Init domain_name pool
    ret = domain_name_table_create(&domain_name_table);
    if (ret || domain_name_table == nullptr) {
        printf("domain name table create fail\n");
        process_error_test(ret);
    }
    parameters.domain_name_table = domain_name_table;

    packet_test packet_ret =  packet_handler_get((u_char *)&parameters, header, packet);
    pcap_info pcap_ret;
    pcap_ret.pcap = pcap;
    pcap_ret.packet_ret = &packet_ret;
    pcap_ret.hash_table = hash_table;
    return pcap_ret;
}

#endif
