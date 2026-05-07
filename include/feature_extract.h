/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef FEATURE_EXTRACT_H
#define FEATURE_EXTRACT_H
#include <math.h>
#include "kdadk.h"
#define RAWBOW_MAX_LEN 4096
const int HISTOGRAM_NUMS = 16;          // 统计信息-直方图位数
const int RAW_BOW_HISTOGRAM_NUMS = 32;  // 词袋信息-直方图位数
const int NETWORK_TUPLE_CNT = 5;        // 五元组特征
const int TOTAL_BYTES_CNT = 1;          // 总字节数特征
const int TOTAL_PACKET_CNT = 1;         // 总包数特征
const int STATISTICS_FEATURE_CNT = 4;   // 统计特征 [最大值 & 最小值 & 均值 & 方差]
const int FEATURE_TYPE_CNT = 3;         // 报文包提取特征种类 [hdr & pld & interval]
const int DIRECT_CNT = 2;               // 报文发送方向数量 [发送 & 接收]
const int RAW_BOW = 1;                  // 词袋明文信息一列
const int KEY_WORD_WEIGHT = 20;         // 关键词的权重系数

#ifndef ENABLE_HIST_REL
#define ENABLE_HIST_REL 1
#endif

#ifndef ENABLE_HIST_NOR
#define ENABLE_HIST_NOR 1
#endif

#if ENABLE_HIST_REL
#define HIST_REL_CNT 1
#else
#define HIST_REL_CNT 0
#endif

#if ENABLE_HIST_NOR
#define HIST_NOR_CNT 1
#else
#define HIST_NOR_CNT 0
#endif

const int HIST_ABS_CNT = 1;
const int HISTOGRAM_CLASS_CNT = HIST_REL_CNT + HIST_ABS_CNT + HIST_NOR_CNT;
#define SEND_PACKET_NUMS_INDEX 7
#define RECV_PACKET_NUMS_INDEX_BASE 21
#define HISTOGRAM_INDEX_STRIDE 48
#define RECV_PACKET_NUMS_INDEX (RECV_PACKET_NUMS_INDEX_BASE + HISTOGRAM_CLASS_CNT * HISTOGRAM_INDEX_STRIDE)

// 特征维度: (五元组 + (总字节数 + 总包数 + (最大值 + 最小值 + 均值 + 方差 + 直方图位数 * HISTOGRAM_CLASS_CNT) * 3[hdr &
// pld & interval]) * 2[发送&接收] + 词袋直方图位数)
const int FEATURE_MAX_SIZE = (NETWORK_TUPLE_CNT +
                              (TOTAL_BYTES_CNT + TOTAL_PACKET_CNT +
                                  (STATISTICS_FEATURE_CNT + HISTOGRAM_NUMS * HISTOGRAM_CLASS_CNT) * FEATURE_TYPE_CNT) *
                                  DIRECT_CNT + RAW_BOW +
                              RAW_BOW_HISTOGRAM_NUMS);

struct statistics_info {
    double min_value;
    double max_value;
    double mean_value;
    double var_value;
#if ENABLE_HIST_REL
    double hist_array[HISTOGRAM_NUMS];      // 相对分布 直方图
#endif
    double hist_array_abs[HISTOGRAM_NUMS];  // 绝对分布 直方图
#if ENABLE_HIST_NOR
    double hist_array_nor[HISTOGRAM_NUMS];  // 正态分布 直方图
#endif
};

struct stream_feature {
    double src_ip;
    double dst_ip;
    double src_port;
    double dst_port;
    double protocol;

    double send_byte_nums;
    double send_packet_nums;
    statistics_info send_hdr;
    statistics_info send_pld;
    statistics_info send_interval;

    double receive_byte_nums;
    double receive_packet_nums;
    statistics_info receive_hdr;
    statistics_info receive_pld;
    statistics_info receive_interval;

    double raw_bow_crc_result[RAW_BOW_HISTOGRAM_NUMS];
};

KDADKResult feature_extract_online(flow_hash_node *node, float feature_vector[FEATURE_MAX_SIZE],
                                   char rawbow[RAWBOW_MAX_LEN], uint32_t *rawbow_len);
double calculate_mean(const std::vector<uint32_t> &nums);
double calculate_var(const std::vector<uint32_t> &nums, double mean);
void calculate_hist(const std::vector<uint32_t> &nums, double min_val, double max_val, double *histogram,
                    int histogram_size, bool isTimeInterval = false);
void calculate_hist_abs(const std::vector<uint32_t> &nums, double max_val, double *histogram, int histogram_size,
                        bool isTimeInterval = false);
void calculate_hist_nor(const std::vector<uint32_t> &nums, double mean_val, double var_val, double *histogram,
                        int histogram_size, bool isTimeInterval = false);

void cal_diffs(const std::vector<struct timeval> &ts, std::vector<uint32_t> &time_diff);
void calculate_crc(flow_hash_node *node, double *raw_bow_crc_result, int raw_bow_size);
void calculate_crc_byte(char &byte, uint16_t &crc);
KDADKResult cal_statistics_info(std::vector<uint32_t> &array, statistics_info &array_si, bool isTimeInterval = false);
KDADKResult cal_stream_feature(flow_hash_node *node, stream_feature &node_sf);
KDADKResult statisticsInfo2Array(const statistics_info &data, double *array, int size, int &begin);
KDADKResult streamFeatur2Array(const stream_feature &data, double *array, int size);

#endif