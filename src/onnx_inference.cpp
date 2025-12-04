/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <iostream>
#include <fstream>
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

#include <onnxruntime_cxx_api.h>
#include <yaml-cpp/yaml.h>
#include "onnx_inference.h"

#ifdef _WIN32
#include <windows.h>
#elif __linux__
#include <unistd.h>
#include <limits.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
using namespace std;

#define ANOMALY_LABEL (-1)
#define MIN_SCALE_VALUE (1e-8)

struct Config {
    vector<vector<string>> evaluation_data_paths;
    vector<int> evaluation_labels;
    string model_path;
    string scaler_path;
    int anomaly_detect_switch;
    float anomaly_detect_limit;
    string output_dir;
    string classification_report_file;
    string predictions_detail_file;
    string onnx_log_level;
};

struct ClassificationReport {
    map<int, double> precision;
    map<int, double> recall;
    map<int, double> f1_score;
    map<int, int> support;
    double accuracy;
    double macro_avg_precision;
    double macro_avg_recall;
    double macro_avg_f1;
    double weighted_avg_precision;
    double weighted_avg_recall;
    double weighted_avg_f1;
};

struct PreparedData {
    vector<vector<float>> raw_features;
    vector<vector<float>> scaled_features;
    vector<int> labels;
    vector<string> feature_names;
    vector<string> raw_bow_data;
    bool has_raw_bow;
};

fs::path get_executable_path()
{
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();

#elif __linux__
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        return fs::path(buffer).parent_path();
    }
    throw std::runtime_error("无法获取可执行文件路径");

#elif __APPLE__
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        char real_path[PATH_MAX];
        if (realpath(buffer, real_path) != NULL) {
            return fs::path(real_path).parent_path();
        }
    }
    throw std::runtime_error("无法获取可执行文件路径");
#else
    throw std::runtime_error("不支持的平台");
#endif
}

fs::path resolve_path(const std::string &path_str, const std::string &base_dir = "", int parent_levels = 1)
{
    fs::path path(path_str);
    if (path.is_absolute()) {
        return fs::absolute(path);
    }
    fs::path base;
    if (base_dir.empty()) {
        base = get_executable_path();
        for (int i = 0; i < parent_levels; ++i) {
            base = base.parent_path();
        }
    } else {
        base = fs::absolute(base_dir);
    }
    fs::path full_path = base / path;
    return fs::absolute(full_path);
}

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

class CSVReader {
public:
    struct CSVData {
        vector<vector<float>> features;
        vector<string> feature_names;
        vector<string> raw_bow_data;
        bool has_raw_bow;
    };

    CSVData read_csv(const string &filepath)
    {
        CSVData result;
        result.has_raw_bow = false;

        ifstream file(filepath);

        if (!file.is_open()) {
            throw runtime_error("无法打开CSV文件: " + filepath);
        }

        string line;
        int raw_bow_index = -1;
        bool is_header = true;
        size_t expected_col_count = 0;
        size_t skipped_rows = 0;
        size_t row_index = 0;

        while (getline(file, line)) {
            if (line.empty() || line.find_first_not_of(" \t\r\n") == string::npos) {
                continue;
            }

            vector<string> tokens = parse_csv_line(line);

            if (is_header) {
                for (size_t i = 0; i < tokens.size(); ++i) {
                    string col_name = trim(tokens[i]);
                    if (to_lower(col_name) == "raw_bow") {
                        raw_bow_index = static_cast<int>(i);
                        result.has_raw_bow = true;
                    } else {
                        result.feature_names.push_back(col_name);  // 保存特征列名
                    }
                }
                expected_col_count = (raw_bow_index >= 0) ? tokens.size() - 1 : tokens.size();
                is_header = false;
                row_index++;
                continue;
            }

            vector<float> row_data;
            string raw_bow_value;
            bool row_valid = true;

            for (size_t i = 0; i < tokens.size(); ++i) {
                if (raw_bow_index >= 0 && static_cast<int>(i) == raw_bow_index) {
                    raw_bow_value = trim(tokens[i]);
                    continue;
                }

                try {
                    row_data.push_back(stof(trim(tokens[i])));
                } catch (const exception &e) {
                    row_valid = false;
                    break;
                }
            }
            if (row_valid && row_data.size() != expected_col_count) {
                cerr << "警告: CSV列数不匹配，将跳过该行 - 文件: " << filepath << ", 期望列数: " << expected_col_count
                     << ", 数据行（第" << (row_index + 1) << "行）列数: " << row_data.size() << endl;
                row_valid = false;
            }

            if (row_valid) {
                result.features.push_back(row_data);
                if (result.has_raw_bow) {
                    result.raw_bow_data.push_back(raw_bow_value);
                }
            } else {
                skipped_rows++;
            }

            row_index++;
        }

        file.close();
        if (skipped_rows > 0) {
            cerr << "提示: 文件 " << filepath << " 共跳过 " << skipped_rows << " 行异常数据" << endl;
        } else {
            cout << "提示: 文件 " << filepath << " 所有数据行均有效" << endl;
        }
        return result;
    }

private:
    vector<string> parse_csv_line(const string &line)
    {
        vector<string> tokens;
        string token;
        bool in_quotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];

            if (c == '"') {
                in_quotes = !in_quotes;
            } else if (c == ',' && !in_quotes) {
                tokens.push_back(token);
                token.clear();
            } else {
                token += c;
            }
        }

        tokens.push_back(token);
        return tokens;
    }

    string trim(const string &str)
    {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == string::npos)
            return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }

    string to_lower(const string &str)
    {
        string result = str;
        transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }
};

Config load_config(const string &config_path)
{
    Config config;
    YAML::Node yaml_config;

    try {
        yaml_config = YAML::LoadFile(config_path);
    } catch (const YAML::Exception &e) {
        throw runtime_error(string(e.what()));
    }

    // 检查并加载 evaluation_data_paths
    if (!yaml_config["evaluation_data_paths"]) {
        throw runtime_error("YAML缺少必需字段 'evaluation_data_paths'");
    }
    
    if (!yaml_config["evaluation_data_paths"].IsSequence()) {
        throw runtime_error("'evaluation_data_paths' 必须是数组类型");
    }
    
    for (const auto &class_paths : yaml_config["evaluation_data_paths"]) {
        if (!class_paths.IsSequence()) {
            throw runtime_error("YAML 'evaluation_data_paths' 中的每个元素必须是数组类型");
        }
        
        vector<string> paths;
        for (const auto &path : class_paths) {
            try {
                paths.push_back(resolve_path(path.as<string>()));
            } catch (const YAML::Exception &e) {
                throw runtime_error("YAML 'evaluation_data_paths' 中的路径格式错误 - " + string(e.what()));
            }
        }
        config.evaluation_data_paths.push_back(paths);
    }

    // 根据 evaluation_data_paths 的大小自动生成 evaluation_labels
    // 从 0 开始递增到 evaluation_data_paths.size() - 1
    config.evaluation_labels.clear();
    for (size_t i = 0; i < config.evaluation_data_paths.size(); ++i) {
        config.evaluation_labels.push_back(static_cast<int>(i));
    }

    // 检查并加载 model_path_onnx
    if (!yaml_config["model_path_onnx"]) {
        throw runtime_error("YAML 缺少必需字段 'model_path_onnx'");
    }
    
    try {
        config.model_path = resolve_path(yaml_config["model_path_onnx"].as<string>());
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'model_path_onnx' 格式错误 - " + string(e.what()));
    }

    // 检查并加载 scaler_path_json
    if (!yaml_config["scaler_path_json"]) {
        throw runtime_error("YAML 缺少必需字段 'scaler_path_json'");
    }
    
    try {
        config.scaler_path = resolve_path(yaml_config["scaler_path_json"].as<string>());
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'scaler_path_json' 格式错误 - " + string(e.what()));
    }
    config.anomaly_detect_switch = 0;
    config.anomaly_detect_limit = 0.5;

    // 检查并加载 output_dir
    if (!yaml_config["output_dir"]) {
        throw runtime_error("YAML 缺少必需字段 'output_dir'");
    }
    
    try {
        config.output_dir = resolve_path(yaml_config["output_dir"].as<string>());
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'output_dir' 格式错误 - " + string(e.what()));
    }

    // 检查并加载 classification_report_file
    if (!yaml_config["classification_report_file"]) {
        throw runtime_error("YAML 缺少必需字段 'classification_report_file'");
    }
    
    try {
        config.classification_report_file = resolve_path(yaml_config["classification_report_file"].as<string>());
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'classification_report_file' 格式错误 - " + string(e.what()));
    }

    // 检查并加载 predictions_detail_file
    if (!yaml_config["predictions_detail_file"]) {
        throw runtime_error("YAML 缺少必需字段 'predictions_detail_file'");
    }
    
    try {
        config.predictions_detail_file = resolve_path(yaml_config["predictions_detail_file"].as<string>());
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'predictions_detail_file' 格式错误 - " + string(e.what()));
    }

    // 检查并加载 onnx_log_level
    if (!yaml_config["onnx_log_level"]) {
        throw runtime_error("YAML 缺少必需字段 'onnx_log_level'");
    }
    
    try {
        config.onnx_log_level = yaml_config["onnx_log_level"].as<string>();
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'onnx_log_level' 格式错误 - " + string(e.what()));
    }

    // 验证 onnx_log_level 的有效值
    const vector<string> valid_log_levels = {"VERBOSE", "INFO", "WARNING", "ERROR", "FATAL"};
    if (find(valid_log_levels.begin(), valid_log_levels.end(), config.onnx_log_level) == valid_log_levels.end()) {
        throw runtime_error("YAML 'onnx_log_level' 的值无效，有效值为: VERBOSE, INFO, WARNING, ERROR, FATAL");
    }

    return config;
}

PreparedData load_and_prepare_data(const vector<vector<string>> &data_paths, const vector<int> &labels,
                                   const StandardScaler &scaler)
{
    if (data_paths.size() != labels.size()) {
        throw runtime_error("data_paths和labels长度不匹配");
    }

    PreparedData result;
    result.has_raw_bow = false;

    vector<vector<float>> all_features;
    vector<int> all_labels;
    vector<string> all_raw_bow;
    CSVReader csv_reader;

    for (size_t i = 0; i < data_paths.size(); ++i) {
        if (data_paths[i].empty()) {
            cerr << "警告: 类别 " << labels[i] << " 的数据路径为空，跳过该类别" << endl;
            continue;
        }

        bool has_data = false;  // 标记当前类别是否有有效数据
        for (const auto &path : data_paths[i]) {
            if (!fs::exists(path)) {
                throw runtime_error("文件不存在: " + path);
            }

            auto csv_data = csv_reader.read_csv(path);
            if (csv_data.features.empty()) {
                continue;
            }

            has_data = true;

            if (result.feature_names.empty()) {
                result.feature_names = csv_data.feature_names;
            }

            if (csv_data.has_raw_bow) {
                result.has_raw_bow = true;
            }

            for (size_t j = 0; j < csv_data.features.size(); ++j) {
                all_features.push_back(csv_data.features[j]);
                all_labels.push_back(labels[i]);

                if (csv_data.has_raw_bow && j < csv_data.raw_bow_data.size()) {
                    all_raw_bow.push_back(csv_data.raw_bow_data[j]);
                } else if (result.has_raw_bow) {
                    all_raw_bow.push_back("");
                }
            }
        }

        if (!has_data) {
            cerr << "警告: 类别 " << labels[i] << " 的所有数据文件均为空，跳过该类别" << endl;
        }
    }

    if (all_features.empty()) {
        throw runtime_error("未加载任何数据，请检查数据路径配置");
    }

    if (all_features.size() != all_labels.size()) {
        throw runtime_error("内部错误: 特征和标签数量不匹配");
    }

    result.raw_features = all_features;
    result.labels = all_labels;
    result.raw_bow_data = all_raw_bow;
    result.scaled_features = scaler.transform(all_features);

    return result;
}

void get_anomaly(vector<int> &predictions, size_t prob_count, size_t num_classes, float *prob_data, size_t label_count,
                 float anomaly_detect_limit)
{
    if (prob_count % label_count != 0) {
        cerr << "警告：概率输出维度异常（总元素数=" << prob_count << "，标签数=" << label_count << "），跳过异常检测"
             << endl;
    } else {
        for (size_t i = 0; i < label_count; ++i) {
            float max_prob = 0.0f;
            for (size_t j = 0; j < num_classes; ++j) {
                max_prob = max(max_prob, prob_data[i * num_classes + j]);
            }
            if (max_prob < anomaly_detect_limit) {
                predictions[i] = ANOMALY_LABEL;  // 使用宏定义标记为异常
            }
        }
        cout << "ONNX推理：异常检测完成，异常阈值 = " << anomaly_detect_limit << endl;
    }
}

vector<Ort::Value> run_onnx_session(Ort::Session &session, const char *input_name, Ort::Value &input_tensor,
                                    const vector<const char *> &output_names_cstr)
{
    return session.Run(Ort::RunOptions{nullptr},
                       &input_name,               // 输入名称（const char*）
                       &input_tensor, 1,          // 输入张量 + 输入数量（单输入）
                       output_names_cstr.data(),  // 输出名称数组
                       output_names_cstr.size()   // 输出节点数量
    );
}

vector<int> process_prediction_output(vector<Ort::Value> &output_tensors, bool anomaly_detect_switch,
                                      float anomaly_detect_limit)
{
    vector<int> predictions;
    if (output_tensors.empty()) {
        throw runtime_error("ONNX推理：未获取到模型输出，请检查输入数据或模型");
    }

    // 解析预测标签
    if (!output_tensors[0].IsTensor()) {
        throw runtime_error("ONNX推理：第一个输出节点不是张量类型，无法解析标签");
    }
    int64_t *label_data = output_tensors[0].GetTensorMutableData<int64_t>();
    size_t label_count = output_tensors[0].GetTensorTypeAndShapeInfo().GetElementCount();
    predictions.assign(label_data, label_data + label_count);
    cout << "ONNX推理：成功解析标签数量 = " << label_count << endl;

    // 异常检测
    const size_t MIN_OUTPUT_TENSORS_FOR_ANOMALY_DETECTION = 2;
    if (anomaly_detect_switch && output_tensors.size() >= MIN_OUTPUT_TENSORS_FOR_ANOMALY_DETECTION) {
        if (!output_tensors[1].IsTensor()) {
            cerr << "警告：已启用异常检测，但第二个输出节点不是张量类型，跳过异常检测" << endl;
        } else {
            float *prob_data = output_tensors[1].GetTensorMutableData<float>();
            size_t prob_count = output_tensors[1].GetTensorTypeAndShapeInfo().GetElementCount();
            size_t num_classes = prob_count / label_count;
            get_anomaly(predictions, prob_count, num_classes, prob_data, label_count, anomaly_detect_limit);
        }
    }

    return predictions;
}

vector<int> predict_with_onnx(Ort::Session &session, const vector<vector<float>> &X_scaled, bool anomaly_detect_switch,
                              float anomaly_detect_limit)
{
    // 1. 输入数据空校验
    if (X_scaled.empty()) {
        throw runtime_error("ONNX推理：输入数据为空，无法进行预测");
    }
    if (X_scaled[0].empty()) {
        throw runtime_error("ONNX推理：输入数据特征数为0，无法进行预测");
    }

    // 2. 准备输入内存
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

    // 3. 展平输入数据
    vector<float> flat_data;
    for (const auto &row : X_scaled) {
        flat_data.insert(flat_data.end(), row.begin(), row.end());
    }

    // 4. 创建输入张量
    vector<int64_t> input_shape = {
        static_cast<int64_t>(X_scaled.size()),    // batch_size
        static_cast<int64_t>(X_scaled[0].size())  // feature_dim
    };
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, flat_data.data(), flat_data.size(),
                                                              input_shape.data(), input_shape.size());

    // 5. 动态获取输入节点名称
    vector<string> input_names = session.GetInputNames();
    if (input_names.empty()) {
        throw runtime_error("ONNX推理：模型没有输入节点，请检查模型完整性");
    }
    const char *input_name = input_names[0].c_str();

    // 6. 动态获取输出节点名称 + 转换为 const char* const*
    vector<string> output_names = session.GetOutputNames();
    if (output_names.empty()) {
        throw runtime_error("ONNX推理：模型没有输出节点，请检查模型完整性");
    }
    vector<const char *> output_names_cstr;
    output_names_cstr.reserve(output_names.size());
    for (const auto &name : output_names) {
        output_names_cstr.push_back(name.c_str());
    }

    // 7. 运行ONNX推理
    auto output_tensors = run_onnx_session(session, input_name, input_tensor, output_names_cstr);

    // 8. 处理预测输出
    return process_prediction_output(output_tensors, anomaly_detect_switch, anomaly_detect_limit);
}

float calculate_accuracy(const vector<int> &true_labels, const vector<int> &pred_labels)
{
    if (true_labels.size() != pred_labels.size()) {
        throw runtime_error("真实标签和预测标签数量不匹配");
    }

    int correct = 0;
    for (size_t i = 0; i < true_labels.size(); ++i) {
        if (true_labels[i] == pred_labels[i]) {
            correct++;
        }
    }

    return static_cast<float>(correct) / true_labels.size();
}

void calculate_class_metrics(const set<int> &classes, const vector<int> &y_true, const vector<int> &y_pred,
                             ClassificationReport &report)
{
    map<int, int> tp;
    map<int, int> fp;
    map<int, int> fn;

    for (int c : classes) {
        tp[c] = 0;
        fp[c] = 0;
        fn[c] = 0;

        for (size_t i = 0; i < y_true.size(); ++i) {
            if (y_true[i] == c && y_pred[i] == c) {
                tp[c]++;
            } else if (y_pred[i] == c && y_true[i] != c) {
                fp[c]++;
            } else if (y_true[i] == c && y_pred[i] != c) {
                fn[c]++;
            }
        }

        report.support[c] = count(y_true.begin(), y_true.end(), c);

        // 计算精确率、召回率和F1分数
        if (tp[c] + fp[c] == 0) {
            report.precision[c] = 0.0;
        } else {
            report.precision[c] = static_cast<double>(tp[c]) / (tp[c] + fp[c]);
        }

        if (tp[c] + fn[c] == 0) {
            report.recall[c] = 0.0;
        } else {
            report.recall[c] = static_cast<double>(tp[c]) / (tp[c] + fn[c]);
        }

        if (report.precision[c] + report.recall[c] == 0) {
            report.f1_score[c] = 0.0;
        } else {
            auto mul = 2;
            report.f1_score[c] =
                mul * (report.precision[c] * report.recall[c]) / (report.precision[c] + report.recall[c]);
        }
    }
}

void calculate_average_metrics(const set<int> &classes, const ClassificationReport &report, double &macro_avg_precision,
                               double &macro_avg_recall, double &macro_avg_f1, double &weighted_avg_precision,
                               double &weighted_avg_recall, double &weighted_avg_f1)
{
    int total_samples = 0;
    macro_avg_precision = 0.0;
    macro_avg_recall = 0.0;
    macro_avg_f1 = 0.0;
    weighted_avg_precision = 0.0;
    weighted_avg_recall = 0.0;
    weighted_avg_f1 = 0.0;

    // 计算宏平均
    for (int c : classes) {
        macro_avg_precision += report.precision.at(c);
        macro_avg_recall += report.recall.at(c);
        macro_avg_f1 += report.f1_score.at(c);
    }

    int num_classes = classes.size();
    if (num_classes > 0) {
        macro_avg_precision /= num_classes;
        macro_avg_recall /= num_classes;
        macro_avg_f1 /= num_classes;
    }

    // 计算加权平均
    for (int c : classes) {
        int support = report.support.at(c);
        weighted_avg_precision += report.precision.at(c) * support;
        weighted_avg_recall += report.recall.at(c) * support;
        weighted_avg_f1 += report.f1_score.at(c) * support;
        total_samples += support;
    }

    if (total_samples > 0) {
        weighted_avg_precision /= total_samples;
        weighted_avg_recall /= total_samples;
        weighted_avg_f1 /= total_samples;
    }
}

ClassificationReport calculate_classification_report(const vector<int> &y_true, const vector<int> &y_pred)
{
    ClassificationReport report;

    if (y_true.size() != y_pred.size()) {
        throw runtime_error("真实标签和预测标签数量不匹配");
    }

    set<int> classes;
    for (int label : y_true)
        classes.insert(label);
    for (int label : y_pred)
        classes.insert(label);

    if (classes.empty()) {
        throw runtime_error("无法计算分类报告：未发现任何类别");
    }

    calculate_class_metrics(classes, y_true, y_pred, report);

    report.accuracy = calculate_accuracy(y_true, y_pred);

    calculate_average_metrics(classes, report, report.macro_avg_precision, report.macro_avg_recall, report.macro_avg_f1,
                              report.weighted_avg_precision, report.weighted_avg_recall, report.weighted_avg_f1);

    return report;
}

void write_classification_report_to_file(const ClassificationReport &report, const string &filename)
{
    if (ifstream(filename)) {
        cout << "警告: 文件 " << filename << " 已存在，将被覆盖。" << endl;
    }

    ofstream file(filename, ios::trunc);
    if (!file.is_open()) {
        cerr << "无法打开文件: " << filename << endl;
        return;
    }

    vector<int> classes;
    for (const auto &pair : report.support) {
        classes.push_back(pair.first);
    }
    sort(classes.begin(), classes.end());

    auto width1 = 15;
    auto width2 = 9;
    auto width3 = 4;
    auto width4 = 10;

    file << "分类报告:\n";
    file << setw(width1) << " " << setw(width4) << "precision" << setw(width4) << "recall" << setw(width4) << "f1-score"
         << setw(width4) << "support\n\n";

    for (int c : classes) {
        file << setw(width1) << c << " " << setw(width2) << fixed << setprecision(width3) << report.precision.at(c)
             << " " << setw(width2) << fixed << setprecision(width3) << report.recall.at(c) << " " << setw(width2)
             << fixed << setprecision(width3) << report.f1_score.at(c) << " " << setw(width2) << report.support.at(c)
             << "\n";
    }

    file << "\n"
         << setw(width1) << "accuracy"
         << " " << setw(width2) << " "
         << " " << setw(width2) << " "
         << " " << setw(width2) << fixed << setprecision(width3) << report.accuracy << " " << setw(width2)
         << accumulate(begin(report.support), end(report.support), 0,
                       [](int value, const map<int, int>::value_type &p) { return value + p.second; })
         << "\n";
    file << setw(width1) << "macro avg"
         << " " << setw(width2) << fixed << setprecision(width3) << report.macro_avg_precision << " " << setw(width2)
         << fixed << setprecision(width3) << report.macro_avg_recall << " " << setw(width2) << fixed
         << setprecision(width3) << report.macro_avg_f1 << " " << setw(width2)
         << accumulate(begin(report.support), end(report.support), 0,
                       [](int value, const map<int, int>::value_type &p) { return value + p.second; })
         << "\n";

    file << setw(width1) << "weighted avg"
         << " " << setw(width2) << fixed << setprecision(width3) << report.weighted_avg_precision << " " << setw(width2)
         << fixed << setprecision(width3) << report.weighted_avg_recall << " " << setw(width2) << fixed
         << setprecision(width3) << report.weighted_avg_f1 << " " << setw(width2)
         << accumulate(begin(report.support), end(report.support), 0,
                       [](int value, const map<int, int>::value_type &p) { return value + p.second; })
         << "\n";

    file.close();
    cout << "分类报告已保存到: " << filename << endl;
}

void write_predictions_detail_to_file(const vector<int> &y_true, const vector<int> &y_pred,
                                      const vector<vector<float>> &X_data, const vector<string> &feature_names,
                                      const vector<string> &raw_bow_data, bool has_raw_bow, const string &filename)
{
    if (ifstream(filename)) {
        cout << "警告: 文件 " << filename << " 已存在，将被覆盖。" << endl;
    }
    ofstream file(filename, ios::trunc);
    if (!file.is_open()) {
        cerr << "无法打开文件: " << filename << endl;
        return;
    }

    if (y_true.size() != y_pred.size() || y_true.size() != X_data.size()) {
        cerr << "错误: 真实标签、预测标签和数据样本数量不一致 (" << y_true.size() << " vs " << y_pred.size() << " vs "
             << X_data.size() << ")" << endl;
        return;
    }

    if (has_raw_bow && raw_bow_data.size() != y_true.size()) {
        cerr << "警告: raw_bow数据数量与样本数量不一致 (" << raw_bow_data.size() << " vs " << y_true.size()
             << ")，将不输出raw_bow列" << endl;
        has_raw_bow = false;
    }

    if (X_data.empty() || X_data[0].empty()) {
        cerr << "错误: 输入数据为空" << endl;
        return;
    }

    size_t num_features = X_data[0].size();

    // 写入CSV表头 - 先写Index，然后是特征列，最后是raw_bow和标签预测结果
    file << "Index";
    for (size_t i = 0; i < num_features; ++i) {
        if (i < feature_names.size()) {
            file << "," << feature_names[i];
        } else {
            file << ",Feature_" << i;
        }
    }

    if (has_raw_bow) {
        file << ",raw_bow";
    }

    file << ",True_Label,Predicted_Label,Correct\n";
    for (size_t i = 0; i < y_true.size(); ++i) {
        file << i;
        for (size_t j = 0; j < X_data[i].size(); ++j) {
            file << "," << fixed << setprecision(6) << X_data[i][j];
        }
        if (has_raw_bow) {
            string raw_bow_value = (i < raw_bow_data.size()) ? raw_bow_data[i] : "";
            if (raw_bow_value.find(',') != string::npos || raw_bow_value.find('"') != string::npos ||
                raw_bow_value.find('\n') != string::npos) {
                string escaped = raw_bow_value;
                size_t pos = 0;
                while ((pos = escaped.find('"', pos)) != string::npos) {
                    escaped.insert(pos, "\"");
                    pos += 2;
                }
                file << ",\"" << escaped << "\"";
            } else {
                file << "," << raw_bow_value;
            }
        }

        file << "," << y_true[i] << "," << y_pred[i] << "," << (y_true[i] == y_pred[i] ? "1" : "0") << "\n";
    }

    file.close();
    cout << "详细预测结果已保存到: " << filename << endl;
}

OrtLoggingLevel get_onnx_log_level(const string &level)
{
    if (level == "VERBOSE")
        return ORT_LOGGING_LEVEL_VERBOSE;
    if (level == "INFO")
        return ORT_LOGGING_LEVEL_INFO;
    if (level == "WARNING")
        return ORT_LOGGING_LEVEL_WARNING;
    if (level == "ERROR")
        return ORT_LOGGING_LEVEL_ERROR;
    if (level == "FATAL")
        return ORT_LOGGING_LEVEL_FATAL;
    return ORT_LOGGING_LEVEL_WARNING;  // 默认
}

int KDADK_Inference(std::string config_file)
{
    try {
        // 1. 加载配置文件
        cout << "正在加载配置文件..." << endl;
        auto config = load_config(config_file);

        // 2. 加载标准化器
        cout << "加载标准化器参数: " << config.scaler_path << endl;
        StandardScaler scaler;
        if (!scaler.load(config.scaler_path)) {
            cerr << "错误: 无法加载标准化器参数文件: " << config.scaler_path << endl;
            return -1;
        }

        // 3. 加载和准备数据
        cout << "加载和准备数据..." << endl;
        auto prepared_data = load_and_prepare_data(config.evaluation_data_paths, config.evaluation_labels, scaler);

        cout << "加载数据完成，样本数量: " << prepared_data.scaled_features.size() << endl;
        if (prepared_data.has_raw_bow) {
            cout << "检测到 raw_bow 列，将在输出中保留" << endl;
        }

        // 4. 加载ONNX模型
        cout << "加载ONNX模型: " << config.model_path << endl;
        Ort::Env env(get_onnx_log_level(config.onnx_log_level), "ONNXInference");
        Ort::SessionOptions session_options;
        Ort::Session session(env, config.model_path.c_str(), session_options);

        // 5. 进行预测
        cout << "进行预测..." << endl;
        auto predictions = predict_with_onnx(session, prepared_data.scaled_features, config.anomaly_detect_switch != 0,
                                             config.anomaly_detect_limit);

        // 6. 计算准确率
        float accuracy = calculate_accuracy(prepared_data.labels, predictions);
        cout << "准确率: " << accuracy << endl;

        // 7. 计算分类报告
        auto report = calculate_classification_report(prepared_data.labels, predictions);

        // 8. 创建输出目录
        if (mkdir(config.output_dir.c_str(), 0755) == 0) {
            cout << "创建目录成功: " << config.output_dir << endl;
        } else if (errno == EEXIST) {
            cout << "目录已存在: " << config.output_dir << endl;
        } else {
            cerr << "创建目录失败: " << strerror(errno) << endl;
            return -1;
        }

        // 9. 将整体分类报告输出到文件
        write_classification_report_to_file(report, config.classification_report_file);

        // 10. 将每个样本的预测结果输出到文件（包含原始数据和raw_bow）
        write_predictions_detail_to_file(prepared_data.labels, predictions,
                                         prepared_data.raw_features,  // 使用原始数据而不是标准化数据
                                         prepared_data.feature_names, prepared_data.raw_bow_data,
                                         prepared_data.has_raw_bow, config.predictions_detail_file);

        cout << "\n推理完成！结果已保存到 " << config.output_dir << " 目录" << endl;
    } catch (const exception &e) {
        cerr << "错误: " << e.what() << endl;
        return -1;
    }
    return 0;
}