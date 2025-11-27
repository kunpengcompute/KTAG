/*
	Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <libgen.h>
#include <linux/limits.h>
#include "process_feature.h"
#include "onnx_inference.h"
std::string get_executable_dir()
{
char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        char *dir = dirname(path);
        return std::string(dir);
    }
    return ".";
}

std::string get_script_path(const char *script_name)
{
    std::string exe_dir = get_executable_dir();
    return exe_dir + "/../src/py/" + script_name;
}

const std::string TRAINING_SCRIPT = get_script_path("training.py");
const std::string EVALUATION_SCRIPT = get_script_path("evaluation.py");

void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s -f <pcap_file>              Extract features from PCAP\n", prog);
    printf("  %s -t <config.yaml>            Train model\n", prog);
    printf("  %s -e <config.yaml>            Evaluate model\n", prog);
    printf("  %s -i <config.yaml>            Run inference\n", prog);
    printf("  %s -h                          Show this help\n\n", prog);
}

bool check_file(const std::string &path)
{
    if (access(path.c_str(), F_OK) != 0) {
        fprintf(stderr, "Error: File '%s' not found.\n", path.c_str());
        return false;
    }
    return true;
}

int run_python(const std::string &script, const char *config)
{
    if (!check_file(script) || !check_file(config)) {
        return -1;
    }

    std::string cmd = "python3 " + script + " " + config;
    int ret = system(cmd.c_str());
    if (ret != 0) {
        fprintf(stderr, "Error: Script execution failed.\n");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    int opt = getopt(argc, argv, "f:t:e:i:h");

    switch (opt) {
        case 'f':  // 特征提取
            if (!check_file(optarg))
                return -1;
            printf("Extracting features from: %s\n", optarg);
            if (feature_extract(optarg) != 0) {
                fprintf(stderr, "Error: Feature extraction failed.\n");
                return -1;
            }
            printf("Feature extraction completed.\n");
            break;

        case 't':  // 训练
            printf("Training model with: %s\n", optarg);
            return run_python(TRAINING_SCRIPT, optarg);

        case 'e':  // Python推理
            printf("Evaluating model with: %s\n", optarg);
            return run_python(EVALUATION_SCRIPT, optarg);

        case 'i':  // C++推理
            if (!check_file(optarg))
                return -1;
            printf("Running inference with: %s\n", optarg);
            if (KDADK_Inference(optarg) != 0) {
                fprintf(stderr, "Error: Inference failed.\n");
                return -1;
            }
            printf("Inference completed.\n");
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : -1;
    }

    return 0;
}
