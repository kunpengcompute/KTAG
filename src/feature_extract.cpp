/*
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <string>
#include "feature_extract.h"
KDADKResult statisticsInfo2Array(const statistics_info &data, double *array, int size, int &begin)
{
if (array == nullptr) {
        return KDADK_STS_NULL_PTR_ERR;
    }

    if ((begin + 4 + HISTOGRAM_NUMS * 3) >= size) {
        return KDADK_STS_VECTOR_INDEX_OVERFLOW;
    }

    array[begin++] = data.min_value;
    array[begin++] = data.max_value;
    array[begin++] = data.mean_value;
    array[begin++] = data.var_value;
    for (int I = 0; I < HISTOGRAM_NUMS; ++i) {
        array[begin++] = data.hist_array[i];
    }

    for (int I = 0; I < HISTOGRAM_NUMS; ++i) {
        array[begin++] = data.hist_array_abs[i];
    }

    for (int I = 0; I < HISTOGRAM_NUMS; ++i) {
        array[begin++] = data.hist_array_nor[i];
    }
    return KDADK_STS_NO_ERR;
}

KDADKResult streamFeatur2Array(const stream_feature &data, double *array, int size)
{
    if (array == nullptr) {
        return KDADK_STS_NULL_PTR_ERR;
    }

    if (size < FEATURE_MAX_SIZE) {
        return KDADK_STS_VECTOR_INDEX_OVERFLOW;
    }

    int index = 0;

    array[index++] = data.src_ip;
    array[index++] = data.dst_ip;
    array[index++] = data.src_port;
    array[index++] = data.dst_port;
    array[index++] = data.protocol;

    array[index++] = data.send_byte_nums;
    array[index++] = data.send_packet_nums;
    statisticsInfo2Array(data.send_hdr, array, FEATURE_MAX_SIZE, index);
    statisticsInfo2Array(data.send_pld, array, FEATURE_MAX_SIZE, index);
    statisticsInfo2Array(data.send_interval, array, FEATURE_MAX_SIZE, index);

    array[index++] = data.receive_byte_nums;
    array[index++] = data.receive_packet_nums;
    statisticsInfo2Array(data.receive_hdr, array, FEATURE_MAX_SIZE, index);
    statisticsInfo2Array(data.receive_pld, array, FEATURE_MAX_SIZE, index);
    statisticsInfo2Array(data.receive_interval, array, FEATURE_MAX_SIZE, index);

    for (int I = 0; I < RAW_BOW_HISTOGRAM_NUMS; ++i) {
        array[index++] = data.raw_bow_crc_result[i];
    }
    return KDADK_STS_NO_ERR;
}

KDADKResult write_csv_header(const std::string &csv_file_name)
{
    FILE *file = fopen(csv_file_name.c_str(), "a");
    if (file == nullptr) {
        return KDADK_STS_FOPEN_FILE_ERR;
    }

    // 五元组
    int tuple_num = 5;
    int direct_num = 2;
    int type_num = 3;
    const std::string tupleHeader[5] = {"src_ip", "dst_ip", "src_port", "dst_port", "protocol"};
    const std::string direct[2] = {"send", "receive"};
    const std::string type[3] = {"hdr", "pld", "interval"};

    for (int I = 0; I < tuple_num; ++i) {
        fprintf(file, "%s,", tupleHeader[i].c_str());
    }

    for (int I = 0; I < direct_num; ++i) {
        fprintf(file, "%s_byte_nums,", direct[i].c_str());
        fprintf(file, "%s_packet_nums,", direct[i].c_str());
        for (int j = 0; j < type_num; ++j) {
            fprintf(file, "%s_%s_min,", direct[i].c_str(), type[j].c_str());
            fprintf(file, "%s_%s_max,", direct[i].c_str(), type[j].c_str());
            fprintf(file, "%s_%s_mean,", direct[i].c_str(), type[j].c_str());
            fprintf(file, "%s_%s_var,", direct[i].c_str(), type[j].c_str());

            for (int k = 0; k < HISTOGRAM_NUMS; ++k) {
                fprintf(file, "%s_%s_hist_rel_%d,", direct[i].c_str(), type[j].c_str(), k);
            }

            for (int k = 0; k < HISTOGRAM_NUMS; ++k) {
                fprintf(file, "%s_%s_hist_abs_%d,", direct[i].c_str(), type[j].c_str(), k);
            }

            for (int k = 0; k < HISTOGRAM_NUMS; ++k) {
                fprintf(file, "%s_%s_hist_nor_%d,", direct[i].c_str(), type[j].c_str(), k);
            }
        }
    }

    for (int I = 0; I < RAW_BOW_HISTOGRAM_NUMS; ++i) {
        fprintf(file, "raw_bow_crc_hist_%d,", i);
    }
    fprintf(file, "raw_bow\n");

    fclose(file);
    return KDADK_STS_NO_ERR;
}

KDADKResult write_feature_to_csv(flow_hash_node *node, const stream_feature &data, const std::string &file_name)
{
    double array[FEATURE_MAX_SIZE];
    streamFeatur2Array(data, array, FEATURE_MAX_SIZE);

    FILE *file = fopen(file_name.c_str(), "a");
    if (file == nullptr) {
        return KDADK_STS_FOPEN_FILE_ERR;
    }

    for (int I = 0; I < FEATURE_MAX_SIZE - 1; ++i) {
        fprintf(file, "%.6f,", array[i]);
    }
    std::string combined;
    for (uint32_t I = 0; I < node->rawbow_cnt; ++i) {
        if (!combined.empty()) {
            combined += "_";
        }
        combined += node->rawbow[i].c_str();
    }
    fprintf(file, "%s\n", combined.c_str());

    fclose(file);
    return KDADK_STS_NO_ERR;
}

double calculate_mean(const std::vector<uint32_t> &nums)
{
    if (nums.empty()) {
        return 0.0;
    }
    double sum = accumulate(nums.begin(), nums.end(), 0.0);
    double mean = sum / nums.size();
    return mean;
}

double calculate_var(const std::vector<uint32_t> &nums, double mean)
{
    if (nums.empty()) {
        return 0.0;
    }
    double variance = 0.0;
    for (double num : nums) {
        variance += pow(num - mean, 2);
    }
    variance /= nums.size();
    return variance;
}

void calculate_hist(const std::vector<uint32_t> &nums, double min_val, double max_val, double *histogram,
    int histogram_size, bool isTimeInterval)
{
    if (histogram == nullptr || histogram_size == 0) {
        return;
    }

    const double epsilon = 1e-5;
    double range = max_val - min_val;
    if (range < epsilon) {
        histogram[0] += nums.size();
        return;
    }

    int bin;
    for (double num : nums) {
        if (isTimeInterval) {
            num = log2(num + 1);
        }
        bin = static_cast<int>(floor((num - min_val) / range * histogram_size));

        bin = bin < 0 ? 0 : bin;
        bin = bin >= histogram_size ? histogram_size - 1 : bin;
        histogram[bin]++;
    }
    return;
}

void calculate_hist_abs(
    const std::vector<uint32_t> &nums, double max_val, double *histogram, int histogram_size, bool isTimeInterval)
{
    if (histogram == nullptr || histogram_size == 0) {
        return;
    }

    const double epsilon = 1e-5;
    const double div_num = (max_val / histogram_size);
    if (div_num < epsilon) {
        histogram[0] += nums.size();
        return;
    }

    int bin;
    for (double num : nums) {
        if (isTimeInterval) {
            num = log2(num + 1);
        }
        bin = static_cast<int>(floor(num / div_num));

        bin = bin < 0 ? 0 : bin;
        bin = bin >= histogram_size ? histogram_size - 1 : bin;
        histogram[bin]++;
    }
    return;
}

void calculate_hist_nor(const std::vector<uint32_t> &nums, double mean_val, double var_val, double *histogram,
    int histogram_size, bool isTimeInterval)
{
    if (histogram == nullptr || histogram_size == 0) {
        return;
    }

    const double epsilon = 1e-5;
    if (var_val < epsilon) {
        histogram[0] += nums.size();
        return;
    }

    int bin;
    for (double num : nums) {
        if (isTimeInterval) {
            num = log2(num + 1);
        }
        bin = static_cast<int>(floor((num - mean_val) / var_val * histogram_size));

        bin = bin < 0 ? 0 : bin;
        bin = bin >= histogram_size ? histogram_size - 1 : bin;
        histogram[bin]++;
    }
    return;
}

void cal_diffs(const std::vector<struct timeval> &ts, std::vector<uint32_t> &time_diff)
{
    if (ts.size() <= 1 || (ts.size() - 1) != time_diff.size()) {
        return;
    }

    for (size_t I = 1; I < ts.size(); ++i) {
        int64_t current = ts[i].tv_sec * 1000000LL + ts[i].tv_usec;
        int64_t previous = ts[I - 1].tv_sec * 1000000LL + ts[I - 1].tv_usec;

        uint32_t diff_micro = current - previous;

        time_diff[I - 1] = diff_micro;
    }

    return;
}

void calculate_crc_byte(char &byte, uint16_t &crc)
{
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int I = 0; I < 8; ++i) {
        if (crc & 0x8000) {
            crc = (crc << 1) ^ 0xA001;
        } else {
            crc <<= 1;
        }
    }
}

const std::unordered_set<std::string> KEY_WORDS = {
    "iqiyi", "tencent", "deepseek", "qq", "bilibili", "bilivideo", "video", "chat", "zijie", "bytedance", "kimi", "mgtv", "hitv", "baidu", "yiyan",
    "youku", "alicdn", "taobao", "iku", "hunyuan", "yuanbao"
};

void calculate_crc(flow_hash_node *node, double *raw_bow_crc_result, int raw_bow_size)
{
    if (node == nullptr) {
        return;
    }

    if (node->rawbow_cnt == 0) {
        return;
    }

    if (raw_bow_crc_result == nullptr || raw_bow_size <= 0) {
        return;
    }

    for (uint32_t I = 0; I < node->rawbow_cnt; ++i) {
        uint16_t crc = 0xFFFF;
        for (char byte : node->rawbow[i]) {
            calculate_crc_byte(byte, crc);
        }
        crc = (crc << 8) | (crc >> 8);

        int raw_bow_index = crc % raw_bow_size;
        std::string current_word(node->rawbow[i].c_str());
        int weight = KEY_WORDS.count(current_word) ? KEY_WORD_WEIGHT : 1;

        raw_bow_crc_result[raw_bow_index] += weight;
    }
    return;
}

KDADKResult cal_statistics_info(std::vector<uint32_t> &array, statistics_info &array_si, bool isTimeInterval)
{
    if (array.size() == 0) {
        return KDADK_STS_PARAMETER_ERR;
    }

    // 计算最大值，最小值，均值和方差
    array_si.min_value = array[0];
    array_si.max_value = array[0];
    array_si.mean_value = 0.0;
    array_si.var_value = 0.0;

    double sum = 0.0;
    double sum_sq = 0.0;
    double cur_num = 0.0;
    for (uint32_t I = 0; I < array.size(); ++i) {
        array_si.min_value = array_si.min_value < array[i] ? array_si.min_value : array[i];
        array_si.max_value = array_si.max_value > array[i] ? array_si.max_value : array[i];

        cur_num = static_cast<double>(array[i]);
        sum += cur_num;
        sum_sq +=  cur_num * cur_num;
    }
    array_si.mean_value = sum / array.size();
    if (array.size() == 1) {
        array_si.var_value = 0.0;
    } else {
        array_si.var_value = (sum_sq - (sum * sum) / array.size()) / (array.size() - 1);
    }

    // 如果是统计的是时间间隔，则对结果取2为底的对数
    if (isTimeInterval) {
        array_si.min_value = log2(array_si.min_value + 1);
        array_si.max_value = log2(array_si.max_value + 1);
        array_si.mean_value = log2(array_si.mean_value + 1);
        array_si.var_value = log2(array_si.var_value + 1);  // 防止方差为0
    }

    calculate_hist(array, array_si.min_value, array_si.max_value, array_si.hist_array, HISTOGRAM_NUMS, isTimeInterval);
    calculate_hist_abs(array, array_si.max_value, array_si.hist_array_abs, HISTOGRAM_NUMS, isTimeInterval);
    calculate_hist_nor(
        array, array_si.mean_value, array_si.var_value, array_si.hist_array_nor, HISTOGRAM_NUMS, isTimeInterval);
    return KDADK_STS_NO_ERR;
}

KDADKResult cal_stream_feature(flow_hash_node *node, stream_feature &node_sf)
{
    if (node == nullptr) {
        return KDADK_STS_PARAMETER_ERR;
    }

    memset_s(&node_sf, sizeof(stream_feature), 0, sizeof(stream_feature));

    node_sf.src_ip = node->tuple.src_ip;
    node_sf.src_port = node->tuple.src_port;
    node_sf.dst_ip = node->tuple.dst_ip;
    node_sf.dst_port = node->tuple.dst_port;
    node_sf.protocol = node->tuple.protocol;

    node_sf.send_packet_nums = node->raw_data.packets_cnt;
    node_sf.send_byte_nums = node->raw_data.total_size;

    cal_statistics_info(node->raw_data.header_len, node_sf.send_hdr);
    cal_statistics_info(node->raw_data.payload_len, node_sf.send_pld);
    if (node->raw_data.ts.size() > 1) {
        std::vector<uint32_t> send_time_diff(node->raw_data.ts.size() - 1);
        cal_diffs(node->raw_data.ts, send_time_diff);
        cal_statistics_info(send_time_diff, node_sf.send_interval, true);
    }

    node_sf.receive_packet_nums = node->reverse_raw_data.packets_cnt;
    node_sf.receive_byte_nums = node->reverse_raw_data.total_size;

    cal_statistics_info(node->reverse_raw_data.header_len, node_sf.receive_hdr);
    cal_statistics_info(node->reverse_raw_data.payload_len, node_sf.receive_pld);
    if (node->reverse_raw_data.ts.size() > 1) {
        std::vector<uint32_t> receive_time_diff(node->reverse_raw_data.ts.size() - 1);
        cal_diffs(node->reverse_raw_data.ts, receive_time_diff);
        cal_statistics_info(receive_time_diff, node_sf.receive_interval, true);
    }

    calculate_crc(node, node_sf.raw_bow_crc_result, RAW_BOW_HISTOGRAM_NUMS);
    return KDADK_STS_NO_ERR;
}

KDADKResult feature_extract_node(flow_hash_node *node, const std::string &csv_file_name)
{
    if (node == nullptr) {
        return KDADK_STS_NULL_PTR_ERR;
    }

    stream_feature node_sf;
    KDADKResult ret = cal_stream_feature(node, node_sf);
    if (ret != KDADK_STS_NO_ERR) {
        return ret;
    }

    ret = write_feature_to_csv(node, node_sf, csv_file_name);
    return ret;
}
