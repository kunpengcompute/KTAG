/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <cmath>
#include <memory>
#include <map>
#include <iomanip>
#include <sstream>
#include <regex>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <unistd.h>
#include <limits.h>
#include <onnxruntime_cxx_api.h>

#define MIN_SCALE_VALUE (1e-8)
namespace fs = std::filesystem;
using namespace std;

struct Config {
    string model_path;
    string scaler_path;
    string onnx_log_level;
    vector<int> columns_to_remove;
    int filter_packets;
};

class SimpleJsonParser {
public:
    static bool parse_scaler_json(const string &filepath, vector<double> &mean, vector<double> &scale,
                                  vector<string> &feature_names)
    {
        ifstream file(filepath);
        if (!file.is_open()) {
            cerr << "无法打开文件: " << filepath << endl;
            return false;
        }

        stringstream buffer;
        buffer << file.rdbuf();
        string content = buffer.str();
        if (!parse_array(content, "mean", mean)) {
            cerr << "解析 mean 失败" << endl;
            return false;
        }

        if (!parse_array(content, "scale", scale)) {
            cerr << "解析 scale 失败" << endl;
            return false;
        }

        parse_string_array(content, "feature_names", feature_names);

        return true;
    }

private:
    static bool parse_array(const string &json, const string &key, vector<double> &result)
    {
        string pattern = "\"" + key + "\"\\s*:\\s*\\[([^\\]]+)\\]";
        regex re(pattern);
        smatch match;

        if (!regex_search(json, match, re)) {
            return false;
        }

        string array_content = match[1].str();

        stringstream ss(array_content);
        string item;
        while (getline(ss, item, ',')) {
            item.erase(remove_if(item.begin(), item.end(), ::isspace), item.end());
            if (!item.empty()) {
                try {
                    result.push_back(stod(item));
                } catch (const exception &e) {
                    cerr << "解析数值失败: " << item << ", 错误: " << e.what() << endl;
                    return false;
                }
            }
        }

        return !result.empty();
    }

    static bool parse_string_array(const string &json, const string &key, vector<string> &result)
    {
        string pattern = "\"" + key + "\"\\s*:\\s*\\[([^\\]]+)\\]";
        regex re(pattern);
        smatch match;

        if (!regex_search(json, match, re)) {
            return false;
        }

        string array_content = match[1].str();

        regex str_re("\"([^\"]+)\"");
        auto begin = sregex_iterator(array_content.begin(), array_content.end(), str_re);
        auto end = sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            result.push_back((*it)[1].str());
        }

        return !result.empty();
    }
};

class StandardScaler {
public:
    bool load(const string &filepath)
    {
        try {
            if (!SimpleJsonParser::parse_scaler_json(filepath, mean_, scale_, feature_names_)) {
                cerr << "解析标准化器参数失败: " << filepath << endl;
                return false;
            }

            for (size_t i = 0; i < scale_.size(); ++i) {
                if (abs(scale_[i]) < MIN_SCALE_VALUE) {
                    cerr << "错误: scale_[" << i << "] = " << scale_[i] << " 接近零，可能导致除零错误" << endl;
                    return false;
                }
            }

            cout << "加载标准化器参数成功，特征数量: " << mean_.size() << endl;
            return true;
        } catch (const exception &e) {
            cerr << "解析标准化器参数错误: " << e.what() << endl;
            return false;
        }
    }

    vector<vector<float>> transform(const vector<vector<float>> &data) const
    {
        if (data.empty() || data[0].size() != mean_.size()) {
            cout << "输入数据维度" << data[0].size() << " 与标准化器特征数量 " << mean_.size() << " 不匹配" << endl;
            throw runtime_error("输入数据维度与标准化器不匹配");
        }

        vector<vector<float>> result;
        result.reserve(data.size());

        for (const auto &row : data) {
            vector<float> scaled_row;
            scaled_row.reserve(row.size());

            for (size_t i = 0; i < row.size(); ++i) {
                scaled_row.push_back((row[i] - mean_[i]) / scale_[i]);
            }

            result.push_back(scaled_row);
        }

        return result;
    }

    const vector<string> &get_feature_names() const
    {
        return feature_names_;
    }

private:
    vector<double> mean_;
    vector<double> scale_;
    vector<string> feature_names_;
};

fs::path get_executable_path();

fs::path resolve_path(const std::string &path_str, const std::string &base_dir, int parent_levels);

Config load_config_common(const string &config_path, bool is_online, bool is_realtime_flow);
Config load_config(const string &config_path);

vector<int> predict_with_onnx(Ort::Session &session, const vector<vector<float>> &X_scaled);

OrtLoggingLevel get_onnx_log_level(const string &level);