/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef KTAG_TEST_H
#define KTAG_TEST_H

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
#include <pcap.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip.h>
#include <net/ethernet.h>
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

#include "ktag_core.h"
#include <sys/stat.h>

#endif
