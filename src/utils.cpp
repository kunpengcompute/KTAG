/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "utils.h"

fs::path get_executable_path()
{
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        throw std::runtime_error("无法获取可执行文件路径");
    }
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

fs::path resolve_path(const std::string &path_str, const std::string &base_dir = "", int parent_levels = 2)
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

Config load_config_common(const string &config_path, bool is_online)
{
    Config config;
    YAML::Node yaml_config;

    try {
        yaml_config = YAML::LoadFile(config_path);
    } catch (const YAML::Exception &e) {
        throw runtime_error(string(e.what()));
    }

    if (!yaml_config["model_path_onnx"]) {
        throw runtime_error("YAML 缺少必需字段 'model_path_onnx'");
    }

    try {
        string model_path = resolve_path(yaml_config["model_path_onnx"].as<string>());
        if (!fs::exists(model_path)) {
            throw runtime_error("文件不存在: " + model_path);
        }
        config.model_path = model_path;
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'model_path_onnx' 格式错误 - " + string(e.what()));
    }

    if (!yaml_config["scaler_path_json"]) {
        throw runtime_error("YAML 缺少必需字段 'scaler_path_json'");
    }

    try {
        string scaler_path = resolve_path(yaml_config["scaler_path_json"].as<string>());
        if (!fs::exists(scaler_path)) {
            throw runtime_error("文件不存在: " + scaler_path);
        }
        config.scaler_path = scaler_path;
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'scaler_path_json' 格式错误 - " + string(e.what()));
    }

    if (!yaml_config["onnx_log_level"]) {
        throw runtime_error("YAML 缺少必需字段 'onnx_log_level'");
    }

    try {
        config.onnx_log_level = yaml_config["onnx_log_level"].as<string>();
    } catch (const YAML::Exception &e) {
        throw runtime_error("YAML 'onnx_log_level' 格式错误 - " + string(e.what()));
    }

    const vector<string> valid_log_levels = {"VERBOSE", "INFO", "WARNING", "ERROR", "FATAL"};
    if (find(valid_log_levels.begin(), valid_log_levels.end(), config.onnx_log_level) == valid_log_levels.end()) {
        throw runtime_error("YAML 'onnx_log_level' 的值无效，有效值为: VERBOSE, INFO, WARNING, ERROR, FATAL");
    }

    if (is_online) {
        if (yaml_config["columns_to_remove"]) {
            if (!yaml_config["columns_to_remove"].IsSequence()) {
                throw runtime_error("YAML 'columns_to_remove' 必须是数组类型");
            }

            for (const auto &column : yaml_config["columns_to_remove"]) {
                try {
                    int value = column.as<int>();
                    if (value < 0 || value > 355) {
                        throw runtime_error("YAML 'columns_to_remove' 中的元素必须是0-355之间的整数，当前值: " + to_string(value));
                    }
                    config.columns_to_remove.push_back(value);
                } catch (const YAML::Exception &e) {
                    throw runtime_error("YAML 'columns_to_remove' 中的元素格式错误 - " + string(e.what()));
                }
            }
        } else {
            config.columns_to_remove.clear();  // 或者不操作，如果默认已经是空的
        }

        if (yaml_config["filter_packets"]) {
            if (yaml_config["filter_packets"].IsNull()) {
                throw runtime_error("YAML 'filter_packets' 不能为空");
            }
            
            try {
                if (!yaml_config["filter_packets"].IsScalar()) {
                    throw runtime_error("YAML 'filter_packets' 必须是标量类型");
                }

                config.filter_packets = yaml_config["filter_packets"].as<int>();

                if (config.filter_packets < 0) {
                    throw runtime_error("YAML 'filter_packets' 必须为非负整数，当前值: " + to_string(config.filter_packets));
                }
            } catch (const YAML::Exception &e) {
                throw runtime_error("YAML 'filter_packets' 格式错误 - " + string(e.what()));
            }
        } else {
            config.filter_packets = 0;
        }
    }

    return config;
}

Config load_config(const string &config_path)
{
    return load_config_common(config_path, true);
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

vector<int> process_prediction_output(vector<Ort::Value> &output_tensors)
{
    vector<int> predictions;
    if (output_tensors.empty()) {
        throw runtime_error("ONNX推理：未获取到模型输出，请检查输入数据或模型");
    }

    if (!output_tensors[0].IsTensor()) {
        throw runtime_error("ONNX推理：第一个输出节点不是张量类型，无法解析标签");
    }
    int64_t *label_data = output_tensors[0].GetTensorMutableData<int64_t>();
    if (label_data == nullptr) {
        throw runtime_error("ONNX推理：获取张量数据指针失败，数据为空");
    }
    size_t label_count = output_tensors[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (label_count == 0) {
        return predictions;
    }
    predictions.assign(label_data, label_data + label_count);

    return predictions;
}

vector<int> predict_with_onnx(Ort::Session &session, const vector<vector<float>> &X_scaled)
{
    if (X_scaled.empty()) {
        throw runtime_error("ONNX推理：输入数据为空，无法进行预测");
    }
    if (X_scaled[0].empty()) {
        throw runtime_error("ONNX推理：输入数据特征数为0，无法进行预测");
    }

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

    vector<float> flat_data;
    for (const auto &row : X_scaled) {
        flat_data.insert(flat_data.end(), row.begin(), row.end());
    }

    vector<int64_t> input_shape = {
        static_cast<int64_t>(X_scaled.size()),    // batch_size
        static_cast<int64_t>(X_scaled[0].size())  // feature_dim
    };
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, flat_data.data(), flat_data.size(),
                                                              input_shape.data(), input_shape.size());

    vector<string> input_names = session.GetInputNames();
    if (input_names.empty()) {
        throw runtime_error("ONNX推理：模型没有输入节点，请检查模型完整性");
    }
    const char *input_name = input_names[0].c_str();

    vector<string> output_names = session.GetOutputNames();
    if (output_names.empty()) {
        throw runtime_error("ONNX推理：模型没有输出节点，请检查模型完整性");
    }
    vector<const char *> output_names_cstr;
    output_names_cstr.reserve(output_names.size());
    for (const auto &name : output_names) {
        output_names_cstr.push_back(name.c_str());
    }

    auto output_tensors = run_onnx_session(session, input_name, input_tensor, output_names_cstr);

    return process_prediction_output(output_tensors);
}

OrtLoggingLevel get_onnx_log_level(const string &level)
{
    if (level == "VERBOSE")
        return ORT_LOGGING_LEVEL_VERBOSE;  // 详细信息
    if (level == "INFO")
        return ORT_LOGGING_LEVEL_INFO;  // 一般信息
    if (level == "WARNING")
        return ORT_LOGGING_LEVEL_WARNING;  // 警告
    if (level == "ERROR")
        return ORT_LOGGING_LEVEL_ERROR;  // 错误
    if (level == "FATAL")
        return ORT_LOGGING_LEVEL_FATAL;  // 致命错误
    return ORT_LOGGING_LEVEL_WARNING;    // 默认
}
