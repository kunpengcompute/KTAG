#include "feature_session.h"
/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "capture_adapter.h"
#include "ktag_file_writer.h"
#include "ktag_type.h"
#include "online_inference.h"
#include "pipeline/pipeline_config.h"
#include "pipeline/pipeline_runner.h"

#ifdef KTAG_DEMO_HAVE_DPDK
#include "ktag_capture_dpdk.h"
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <limits>
#include <linux/limits.h>
#include <string>
#include <unistd.h>

#ifndef KTAG_ENABLE_DPDK_UB
#define KTAG_ENABLE_DPDK_UB 0
#endif

enum {
  KTAG_DEMO_OPT_PIPELINE_MODE = 1000,
  KTAG_DEMO_OPT_INFERENCE_THREADS,
  KTAG_DEMO_OPT_FEATURE_CONFIG,
  KTAG_DEMO_OPT_DISTRIBUTED_ROLE,
  KTAG_DEMO_OPT_NODE_ID,
  KTAG_DEMO_OPT_DPDK,
  KTAG_DEMO_OPT_DPDK_EAL_ARGS,
  KTAG_DEMO_OPT_PCAP_LABELS
};

typedef struct {
  char *config_file;
  char *feature_config_file;
  char **pcap_files;
  int num_pcap_files;
  char *interface;
  char *output_file;
  file_format
      output_format; // 输出文件格式: FILE_FORMAT_CSV 或 FILE_FORMAT_JSON
  int mode;          // 0:单flow, 1:批处理, 2:文件模式
  int operation;     // 0:训练, 1:验证, 2:特征提取, 3:在线推理
  int with_rawbow;   // 是否输出rawbow (0:不输出, 1:输出)
  char *pipeline_mode;        // legacy 或四种新 pipeline mode
  uint32_t feature_threads;   // 0 表示使用 YAML
  uint32_t inference_threads; // 0 表示使用 YAML
  char *distributed_role;     // frontend 或 backend
  char *node_id;              // UB 拓扑中的本机 hostname
  int use_dpdk;               // --dpdk: 用 DPDK PMD 收网卡报文
  char *dpdk_eal_args; // --dpdk-eal-args: EAL 参数(空格分隔字符串)
  char *pcap_labels;
} CommandLineArgs;

int feature_extraction_mode(const char *pcap_file, const char *output_file,
                            int with_rawbow) {
  printf("========== 特征提取模式 ==========\n");
  printf("输入文件: %s\n", pcap_file);
  printf("输出文件: %s\n", output_file);
  printf("输出rawbow: %s\n", with_rawbow ? "是" : "否");

  const ktag_capture_config capture_config =
      make_offline_capture_config(pcap_file);
  ktag_capture_context *capture = ktag_capture_open(&capture_config);
  if (!capture) {
    fprintf(stderr, "Capture open failed: %s\n", ktag_capture_get_last_error());
    return -1;
  }

  ktag_demo_feature_session *extractor =
      ktag_demo_feature_session::create(static_cast<int>(ktag_capture_get_link_type(capture)));
  if (!extractor) {
    fprintf(stderr, "错误: 无法初始化特征提取器\n");
    ktag_capture_close(capture);
    return -1;
  }

  file_format format = FILE_FORMAT_CSV;
  if (strstr(output_file, ".json")) {
    format = FILE_FORMAT_JSON;
  }

  write_mode mode =
      with_rawbow ? WRITE_MODE_FEATURES_WITH_RAWBOW : WRITE_MODE_FEATURES_ONLY;
  file_writer_config writer_config = {
      .file_path = output_file, .format = format, .mode = mode, .append = 0};
  file_writer *writer = writer_create(&writer_config);
  if (!writer) {
    fprintf(stderr, "错误: 无法创建文件写入器\n");
    ktag_capture_close(capture);
    delete extractor;
    return -1;
  }

  ktag_packet packet{};
  ktag_packet_init(&packet);
  int packet_count = 0;
  int flow_count = 0;

  feature_vector_list remaining = {0};
  rawbow_list remaining_rawbows = {0};
  int ret_code = 0;

  feature_vector_list all_features = {0};
  all_features.capacity = 1000;
  all_features.vectors =
      (feature_vector *)malloc(all_features.capacity * sizeof(feature_vector));
  if (!all_features.vectors) {
    fprintf(stderr, "错误: 无法分配特征向量内存\n");
    writer_destroy(writer);
    ktag_capture_close(capture);
    delete extractor;
    return -1;
  }

  rawbow_list all_rawbows = {0};
  if (with_rawbow) {
    all_rawbows.capacity = 1000;
    all_rawbows.rawbows =
        (rawbow *)malloc(all_rawbows.capacity * sizeof(rawbow));
    if (!all_rawbows.rawbows) {
      fprintf(stderr, "错误: 无法分配rawbow内存\n");
      free(all_features.vectors);
      writer_destroy(writer);
      ktag_capture_close(capture);
      delete extractor;
      return -1;
    }
  }
  while (true) {
    const ktag_status capture_status = ktag_capture_next(capture, &packet);
    if (capture_status == KTAG_STATUS_TIMEOUT) {
      continue;
    }
    if (capture_status == KTAG_STATUS_END_OF_STREAM) {
      break;
    }
    if (capture_status != KTAG_SUCCESS) {
      fprintf(stderr, "Capture read failed: %s\n",
              ktag_capture_get_last_error());
      ret_code = -1;
      break;
    }
    packet_count++;

    feature_vector features;
    rawbow raw_payload;
    int has_feature = 0;
    int ret;
    const timeval timestamp = packet_timeval(packet);
    if (with_rawbow) {
      ret = extractor->process_with_rawbow(packet.data,
                                       packet.captured_length, &timestamp,
                                       &features, &raw_payload, &has_feature);
    } else {
      ret = extractor->process(packet.data, packet.captured_length,
                           &timestamp, &features, &has_feature);
    }
    ktag_packet_release(&packet);
    if (ret == KTAG_SUCCESS && has_feature) {
      if (all_features.count >= all_features.capacity) {
        size_t new_capacity = all_features.capacity * 2;
        feature_vector *new_vectors = (feature_vector *)realloc(
            all_features.vectors, new_capacity * sizeof(feature_vector));
        if (!new_vectors) {
          fprintf(stderr, "错误: 扩容特征向量列表失败\n");
          ret_code = -1;
          goto cleanup;
        }
        all_features.vectors = new_vectors;
        all_features.capacity = new_capacity;
      }
      all_features.vectors[all_features.count++] = features;
      // 扩容rawbow列表（如果需要）
      if (with_rawbow) {
        if (all_rawbows.count >= all_rawbows.capacity) {
          size_t new_capacity = all_rawbows.capacity * 2;
          rawbow *new_rawbows = (rawbow *)realloc(
              all_rawbows.rawbows, new_capacity * sizeof(rawbow));
          if (!new_rawbows) {
            fprintf(stderr, "错误: 扩容rawbow列表失败\n");
            ret_code = -1;
            goto cleanup;
          }
          all_rawbows.rawbows = new_rawbows;
          all_rawbows.capacity = new_capacity;
        }
        all_rawbows.rawbows[all_rawbows.count++] = raw_payload;
      }
      flow_count++;
    }
  }

  // 提取剩余流
  remaining.capacity = 100000;
  remaining.vectors =
      (feature_vector *)malloc(remaining.capacity * sizeof(feature_vector));
  if (!remaining.vectors) {
    fprintf(stderr, "错误: 无法分配剩余特征向量内存\n");
    ret_code = -1;
    goto cleanup;
  }

  if (with_rawbow) {
    remaining_rawbows.capacity = 100000;
    remaining_rawbows.rawbows =
        (rawbow *)malloc(remaining_rawbows.capacity * sizeof(rawbow));
    if (!remaining_rawbows.rawbows) {
      fprintf(stderr, "错误: 无法分配剩余rawbow内存\n");
      free(remaining.vectors);
      remaining.vectors = NULL;
      ret_code = -1;
      goto cleanup;
    }
  }
  int ret;
  if (with_rawbow) {
    ret = extractor->flush_with_rawbow(&remaining,
                                         &remaining_rawbows);
  } else {
    ret = extractor->flush(&remaining);
  }
  if (ret == KTAG_SUCCESS) {
    for (uint32_t i = 0; i < remaining.count; i++) {
      // 扩容特征向量列表
      if (all_features.count >= all_features.capacity) {
        size_t new_capacity = all_features.capacity * 2;
        feature_vector *new_vectors = (feature_vector *)realloc(
            all_features.vectors, new_capacity * sizeof(feature_vector));
        if (!new_vectors) {
          fprintf(stderr, "错误: 扩容特征向量列表失败\n");
          free(remaining.vectors);
          remaining.vectors = NULL;
          if (with_rawbow && remaining_rawbows.rawbows) {
            free(remaining_rawbows.rawbows);
            remaining_rawbows.rawbows = NULL;
          }
          ret_code = -1;
          goto cleanup;
        }
        all_features.vectors = new_vectors;
        all_features.capacity = new_capacity;
      }
      all_features.vectors[all_features.count++] = remaining.vectors[i];

      // 扩容rawbow列表（如果需要）
      if (with_rawbow && i < remaining_rawbows.count) {
        if (all_rawbows.count >= all_rawbows.capacity) {
          size_t new_capacity = all_rawbows.capacity * 2;
          rawbow *new_rawbows = (rawbow *)realloc(
              all_rawbows.rawbows, new_capacity * sizeof(rawbow));
          if (!new_rawbows) {
            fprintf(stderr, "错误: 扩容rawbow列表失败\n");
            free(remaining.vectors);
            remaining.vectors = NULL;
            free(remaining_rawbows.rawbows);
            remaining_rawbows.rawbows = NULL;
            ret_code = -1;
            goto cleanup;
          }
          all_rawbows.rawbows = new_rawbows;
          all_rawbows.capacity = new_capacity;
        }
        all_rawbows.rawbows[all_rawbows.count++] = remaining_rawbows.rawbows[i];
      }

      flow_count++;
    }
  }

  if (remaining.vectors) {
    free(remaining.vectors);
    remaining.vectors = NULL;
  }
  if (with_rawbow && remaining_rawbows.rawbows) {
    free(remaining_rawbows.rawbows);
    remaining_rawbows.rawbows = NULL;
  }

  // 写入文件
  if (with_rawbow) {
    if (writer_write_features_with_rawbow(writer, &all_features,
                                          &all_rawbows) != WRITER_SUCCESS) {
      fprintf(stderr, "错误: 写入特征和rawbow失败\n");
      ret_code = -1;
      goto cleanup;
    }
  } else {
    if (writer_write_features(writer, &all_features) != WRITER_SUCCESS) {
      fprintf(stderr, "错误: 写入特征失败\n");
      ret_code = -1;
      goto cleanup;
    }
  }
  if (writer_flush(writer) != WRITER_SUCCESS ||
      writer_close(writer) != WRITER_SUCCESS) {
    ret_code = -1;
    goto cleanup;
  }

  printf("处理完成:\n");
  printf("  - 总包数: %d\n", packet_count);
  printf("  - 总流数: %d\n", flow_count);
  printf("  - 输出文件: %s\n", output_file);

cleanup:
  if (remaining.vectors) {
    free(remaining.vectors);
  }
  if (with_rawbow && remaining_rawbows.rawbows) {
    free(remaining_rawbows.rawbows);
  }
  if (all_features.vectors) {
    free(all_features.vectors);
  }
  if (with_rawbow && all_rawbows.rawbows) {
    free(all_rawbows.rawbows);
  }
  writer_destroy(writer);
  ktag_packet_release(&packet);
  ktag_capture_close(capture);
  delete extractor;

  return ret_code;
}

void listNetworkDevices() {
  ktag_capture_device_list *capture_devices = ktag_capture_list_devices();
  if (capture_devices == nullptr) {
    fprintf(stderr, "Error finding devices: %s\n",
            ktag_capture_get_last_error());
    return;
  }

  std::cout << "Available network devices:" << std::endl;
  std::cout << "========================================" << std::endl;
  int display_index = 0;
  for (uint32_t device_index = 0;
       device_index < ktag_capture_device_list_get_count(capture_devices);
       ++device_index) {
    const char *device_name =
        ktag_capture_device_list_get_name(capture_devices, device_index);
    const std::string name = device_name == nullptr ? "" : device_name;
    if (name.find("usbmon") != std::string::npos ||
        name.find("bluetooth") != std::string::npos ||
        name.find("nflog") != std::string::npos ||
        name.find("nfqueue") != std::string::npos ||
        name.find("any") != std::string::npos) {
      continue;
    }

    std::cout << "[" << display_index++ << "] " << name;
    const char *description =
        ktag_capture_device_list_get_description(capture_devices, device_index);
    if (description != nullptr) {
      std::cout << " (" << description << ")";
    }
    std::cout << std::endl;
    for (uint32_t address_index = 0;
         address_index <
         ktag_capture_device_list_get_ipv4_count(capture_devices, device_index);
         ++address_index) {
      const char *address = ktag_capture_device_list_get_ipv4(
          capture_devices, device_index, address_index);
      if (address != nullptr) {
        std::cout << "    IP: " << address << std::endl;
      }
    }
  }
  std::cout << "========================================" << std::endl;
  ktag_capture_device_list_destroy(capture_devices);
}

std::string get_executable_dir() {
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);

  if (len == -1) {
    perror("readlink failed");
    return ".";
  }

  path[len] = '\0';

  // 手动查找最后一个斜杠（避免 dirname 的问题）
  char *last_slash = strrchr(path, '/');
  if (last_slash != nullptr) {
    *last_slash = '\0';
    return std::string(path);
  }

  return ".";
}

std::string get_script_path(const char *script_name) {
  if (script_name == nullptr) { // 额外的安全检查
    return "";
  }

  std::string exe_dir = get_executable_dir();
  const std::string build_tree_path = exe_dir + "/python/" + script_name;
  if (access(build_tree_path.c_str(), F_OK) == 0) {
    return build_tree_path;
  }
  return exe_dir + "/../share/KTAG/python/" + script_name;
}

const std::string &get_training_script() {
  static const std::string script = get_script_path("training.py");
  return script;
}

const std::string &get_evaluation_script() {
  static const std::string script = get_script_path("evaluation.py");
  return script;
}
bool check_file(const std::string &path) {
  if (access(path.c_str(), F_OK) != 0) {
    fprintf(stderr, "Error: File '%s' not found.\n", path.c_str());
    return false;
  }
  return true;
}

int run_python(const std::string &script, const char *config) {
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

void print_usage(const char *prog_name) {
  printf("用法: %s [选项]\n", prog_name);
  printf("\n选项:\n");
  printf("  -t <config.yaml>        模型训练模式\n");
  printf("  -e <config.yaml>        模型验证模式\n");
  printf("  -f <file.pcap>          特征提取模式 (需要配合 -c 或 -j 使用)\n");
  printf("  --config <config.yaml>  -f 可选：按现有 Pipeline/Schema 输出训练 "
         "CSV\n");
  printf("  -r <config.yaml>        在线推理模式 (需要配合 -c 或 -j 使用)\n");
  printf("  --pcap-labels LIST      各 -p 的真实标签，如 2,2,3；数量必须与 PCAP 一致\n");
  printf("                         未指定时按 0,1,2... 假定；报告仅显示本次标签的升序去重集合\n");
  printf("  -p <file.pcap>          输入PCAP文件 (可多个)\n");
  printf("  -i <interface>          网口实时抓包\n");
  printf("  -m <mode>               推理模式: 0=单flow, 1=批处理, 2=文件 "
         "(默认: 1)\n");
  printf("  --pipeline-mode <mode>  "
         "legacy/single_thread/staged/feature_parallel/full_parallel\n");
  printf("  -n, --feature-threads N Feature Worker 数量（仅新 pipeline）\n");
  printf("  --inference-threads N   Inference Worker 数量（仅新 pipeline）\n");
  printf("  --distributed-role R    UB 节点角色: frontend/backend\n");
  printf("  --node-id HOSTNAME      本机在 UB 拓扑中的 hostname\n");
  printf(
      "  --dpdk                  用 DPDK PMD 收网卡报文（仅新 pipeline，需编译"
      "支持）\n");
  printf("  --dpdk-eal-args <str>   DPDK EAL 参数，如 \"-a "
         "0000:03:00.0\"（可选）\n");
  printf("  -c <output.csv>         输出CSV文件路径 (与 -j 互斥)\n");
  printf("  -j <output.json>        输出JSON文件路径 (与 -c 互斥)\n");
  printf("  -w                      输出是否带有rawbow (仅用于 -f "
         "特征提取模式)\n");
  printf("  -l                      列出当前机器可用网口\n");
  printf("  -h                      显示帮助信息\n");
  printf("\n说明:\n");
  printf("  -c 和 -j 选项只能选择其中一个，分别指定CSV或JSON格式的输出文件\n");
  printf("  新 Pipeline 推理输出含数字 label/confidence 及完整 IPv4/IPv6 "
         "流元数据\n");
  printf("  -c 选项的文件必须以 .csv 结尾\n");
  printf("  -j 选项的文件必须以 .json 结尾\n");
  printf("  -w rawbow输出仅适用于 -f 特征提取模式, "
         "rawbow特征列信息仅用于流量打标签进行人工校验\n");
  printf("\n示例:\n");
  printf("  # 特征提取 - CSV格式\n");
  printf("  %s -f input.pcap -c output.csv\n", prog_name);
  printf("  %s -f input.pcap --config config.yaml -c schema.csv\n", prog_name);
  printf("\n");
  printf("  # 特征提取 - JSON格式，带rawbow\n");
  printf("  %s -f input.pcap -j output.json -w\n", prog_name);
  printf("\n");
  printf("  # 在线推理 - 多个PCAP文件，输出CSV\n");
  printf("  %s -r config.yaml -p file1.pcap -p file2.pcap -m 1 -c result.csv\n",
         prog_name);
  printf("  %s -r config.yaml -p file1.pcap --pipeline-mode feature_parallel "
         "-n 4 -c result.csv\n",
         prog_name);
  printf("  %s -r config.yaml -p class2.pcap --pcap-labels 2 -c result.csv\n",
         prog_name);
  printf("  %s -r config.yaml -p file1.pcap --pipeline-mode full_parallel -n 4 "
         "--inference-threads 2 -c result.csv\n",
         prog_name);
  printf("\n");
  printf("  # 在线推理 - 网口实时抓包，输出JSON\n");
  printf("  %s -r config.yaml -i eth0 -j result.json\n", prog_name);
  printf("  %s -r config.yaml -i 0000:03:00.0 --dpdk -j result.json\n",
         prog_name);
  printf("\n");
  printf("  # UB 分布式推理（所有节点使用同一份 YAML）\n");
  printf("  %s -r config.yaml -i eth0 --distributed-role frontend "
         "--node-id fe-01\n",
         prog_name);
  printf("  %s -r config.yaml --distributed-role backend --node-id be-01 "
         "-c result.csv\n",
         prog_name);
  printf("\n");
  printf("  # 列出可用网口\n");
  printf("  %s -l\n", prog_name);
}

static int check_file_extension(const char *filename, const char *ext) {
  if (!filename || !ext) {
    return 0;
  }

  size_t filename_len = strlen(filename);
  size_t ext_len = strlen(ext);

  if (filename_len < ext_len) {
    return 0;
  }

  // 比较文件名末尾是否与扩展名匹配（不区分大小写）
  const char *file_ext = filename + filename_len - ext_len;
  return strcasecmp(file_ext, ext) == 0;
}

static int parse_positive_u32(const char *text, const char *option_name,
                              uint32_t *value) {
  if (text == nullptr || value == nullptr) {
    return -1;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long long parsed = strtoull(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || parsed == 0U ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    fprintf(stderr, "错误: %s 必须是 uint32 范围内的正整数，实际为 \"%s\"\n",
            option_name, text);
    return -1;
  }
  *value = static_cast<uint32_t>(parsed);
  return 0;
}

int parse_args(int argc, char *argv[], CommandLineArgs *args) {
  memset(args, 0, sizeof(CommandLineArgs));
  args->mode = 1; // 默认批处理模式
  args->operation = -1;
  args->with_rawbow = 0; // 默认不输出raw payload
  int opt;
  int pcap_capacity = 20; // 默认最大支持20个pcap
  args->pcap_files = (char **)malloc(pcap_capacity * sizeof(char *));
  if (!args->pcap_files) {
    fprintf(stderr, "错误: 无法分配pcap文件列表内存\n");
    return -1;
  }

  int csv_specified = 0;  // 标记是否指定了 -c
  int json_specified = 0; // 标记是否指定了 -j

  static const struct option long_options[] = {
      {"pipeline-mode", required_argument, nullptr,
       KTAG_DEMO_OPT_PIPELINE_MODE},
      {"feature-threads", required_argument, nullptr, 'n'},
      {"inference-threads", required_argument, nullptr,
       KTAG_DEMO_OPT_INFERENCE_THREADS},
      {"config", required_argument, nullptr, KTAG_DEMO_OPT_FEATURE_CONFIG},
      {"distributed-role", required_argument, nullptr,
       KTAG_DEMO_OPT_DISTRIBUTED_ROLE},
      {"node-id", required_argument, nullptr, KTAG_DEMO_OPT_NODE_ID},
      {"dpdk", no_argument, nullptr, KTAG_DEMO_OPT_DPDK},
      {"dpdk-eal-args", required_argument, nullptr,
       KTAG_DEMO_OPT_DPDK_EAL_ARGS},
      {"pcap-labels", required_argument, nullptr, KTAG_DEMO_OPT_PCAP_LABELS},
      {nullptr, 0, nullptr, 0}};
  while ((opt = getopt_long(argc, argv, "t:e:f:r:p:i:m:c:j:n:wlh", long_options,
                            nullptr)) != -1) {
    switch (opt) {
    case 't':
      args->operation = 0;
      args->config_file = optarg;
      break;
    case 'e':
      args->operation = 1;
      args->config_file = optarg;
      break;
    case 'f':
      args->operation = 2;
      args->pcap_files[args->num_pcap_files++] = optarg;
      break;
    case 'r':
      args->operation = 3;
      args->config_file = optarg;
      break;
    case 'p':
      args->pcap_files[args->num_pcap_files++] = optarg;
      break;
    case KTAG_DEMO_OPT_PCAP_LABELS:
      if (args->pcap_labels != nullptr) {
        fprintf(stderr, "错误: --pcap-labels 只能指定一次\n");
        return -1;
      }
      args->pcap_labels = optarg;
      break;
    case 'i':
      args->interface = optarg;
      break;
    case 'm': {
      char *endptr;
      errno = 0;
      long val = strtol(optarg, &endptr, 10);
      if (endptr == optarg || *endptr != '\0' || errno == ERANGE || val < 0 ||
          val > 2) {
        fprintf(stderr, "错误: 无效的推理模式 \"%s\"\n", optarg);
        return -1;
      }
      args->mode = (int)val;
      break;
    }
    case KTAG_DEMO_OPT_PIPELINE_MODE: {
      ktag_demo_pipeline_mode parsed_mode;
      if (strcmp(optarg, "legacy") != 0 &&
          !ktag_demo_pipeline_mode_from_name(optarg, &parsed_mode)) {
        fprintf(stderr,
                "错误: 无效的 pipeline mode \"%s\"；"
                "可选 legacy、single_thread、staged、"
                "feature_parallel、full_parallel\n",
                optarg);
        return -1;
      }
      args->pipeline_mode = optarg;
      break;
    }
    case 'n':
      if (parse_positive_u32(optarg, "--feature-threads/-n",
                             &args->feature_threads) != 0) {
        return -1;
      }
      break;
    case KTAG_DEMO_OPT_INFERENCE_THREADS:
      if (parse_positive_u32(optarg, "--inference-threads",
                             &args->inference_threads) != 0) {
        return -1;
      }
      break;
    case KTAG_DEMO_OPT_FEATURE_CONFIG:
      args->feature_config_file = optarg;
      break;
    case KTAG_DEMO_OPT_DISTRIBUTED_ROLE: {
      ktag_demo_distributed_role parsed_role;
      if (!ktag_demo_distributed_role_from_name(optarg, &parsed_role)) {
        fprintf(stderr,
                "错误: --distributed-role 只能是 frontend 或 backend\n");
        return -1;
      }
      args->distributed_role = optarg;
      break;
    }
    case KTAG_DEMO_OPT_NODE_ID:
      if (optarg[0] == '\0') {
        fprintf(stderr, "错误: --node-id 不能为空\n");
        return -1;
      }
      args->node_id = optarg;
      break;
    case KTAG_DEMO_OPT_DPDK:
      args->use_dpdk = 1;
      break;
    case KTAG_DEMO_OPT_DPDK_EAL_ARGS:
      args->dpdk_eal_args = optarg;
      break;
    case 'c':
      if (json_specified) {
        fprintf(stderr, "错误: -c 和 -j 选项不能同时使用\n");
        print_usage(argv[0]);
        return -1;
      }
      // 检查文件扩展名
      if (!check_file_extension(optarg, ".csv")) {
        fprintf(stderr, "错误: -c 选项的文件必须以 .csv 结尾\n");
        fprintf(stderr, "提示: 您输入的文件是: %s\n", optarg);
        return -1;
      }
      args->output_file = optarg;
      args->output_format = FILE_FORMAT_CSV;
      csv_specified = 1;
      break;
    case 'j':
      if (csv_specified) {
        fprintf(stderr, "错误: -c 和 -j 选项不能同时使用\n");
        print_usage(argv[0]);
        return -1;
      }
      // 检查文件扩展名
      if (!check_file_extension(optarg, ".json")) {
        fprintf(stderr, "错误: -j 选项的文件必须以 .json 结尾\n");
        fprintf(stderr, "提示: 您输入的文件是: %s\n", optarg);
        return -1;
      }
      args->output_file = optarg;
      args->output_format = FILE_FORMAT_JSON;
      json_specified = 1;
      break;
    case 'w':
      args->with_rawbow = 1; // 启用raw payload输出
      break;
    case 'l':
      listNetworkDevices();
      exit(0);
    case 'h':
      print_usage(argv[0]);
      exit(0);
    default:
      print_usage(argv[0]);
      return -1;
    }
  }

  // ========== 参数验证 ==========

  // 1. 必须指定操作模式
  if (args->operation == -1) {
    fprintf(stderr, "错误: 必须指定操作模式 (-t/-e/-f/-r)\n");
    print_usage(argv[0]);
    return -1;
  }

  const int has_pipeline_cli =
      args->pipeline_mode != nullptr || args->feature_threads != 0U ||
      args->inference_threads != 0U || args->distributed_role != nullptr ||
      args->node_id != nullptr || args->use_dpdk != 0 ||
      args->dpdk_eal_args != nullptr;
  if (args->pcap_labels != nullptr &&
      (args->operation != 3 || args->num_pcap_files == 0 ||
       args->interface != nullptr || args->distributed_role != nullptr)) {
    fprintf(stderr, "错误: --pcap-labels 仅支持单机 -r/-p，不支持 -i 或 UB 分布式模式\n");
    return -1;
  }
  if (args->feature_config_file != nullptr && args->operation != 2) {
    fprintf(stderr, "错误: --config 只能与 Schema 特征提取模式 -f 一起使用\n");
    return -1;
  }
  if (has_pipeline_cli && args->operation != 3 &&
      !(args->operation == 2 && args->feature_config_file != nullptr)) {
    fprintf(stderr,
            "错误: pipeline 和线程参数只能与 -r，或 -f --config 一起使用\n");
    return -1;
  }
  if (args->pipeline_mode != nullptr &&
      strcmp(args->pipeline_mode, "legacy") == 0 &&
      (args->feature_threads != 0U || args->inference_threads != 0U)) {
    fprintf(stderr,
            "错误: legacy 模式不能同时指定 Feature/Inference Worker 数量\n");
    return -1;
  }
  if ((args->distributed_role == nullptr) != (args->node_id == nullptr)) {
    fprintf(stderr, "错误: --distributed-role 和 --node-id 必须同时指定\n");
    return -1;
  }
  if (args->node_id != nullptr) {
    constexpr size_t kHostnameBufferSize = 256U;
    char hostname[kHostnameBufferSize] = {};
    if (gethostname(hostname, sizeof(hostname) - 1U) != 0 ||
        strcmp(args->node_id, hostname) != 0) {
      fprintf(stderr,
              "错误: --node-id 必须与本机 gethostname() 结果完全一致"
              "（当前为 %s）\n",
              hostname[0] == '\0' ? "<unavailable>" : hostname);
      return -1;
    }
  }
  if (args->distributed_role != nullptr && args->pipeline_mode != nullptr &&
      strcmp(args->pipeline_mode, "legacy") == 0) {
    fprintf(stderr, "错误: UB 分布式模式不支持 legacy pipeline\n");
    return -1;
  }
  if (args->distributed_role != nullptr && !KTAG_ENABLE_DPDK_UB) {
    fprintf(stderr, "错误: 当前构建未启用 DPDK UB，请使用 "
                    "-DKTAG_ENABLE_DPDK_UB=ON 重新构建\n");
    return -1;
  }
  if ((args->use_dpdk != 0 || args->dpdk_eal_args != nullptr) &&
      args->interface == nullptr) {
    fprintf(stderr, "错误: --dpdk 需要指定网卡设备 (-i <PCI地址或vdev名>)\n");
    return -1;
  }

  // 2. 特征提取模式 (-f) 的验证
  if (args->operation == 2) {
    // 必须有输入PCAP文件
    if (args->num_pcap_files == 0) {
      fprintf(stderr,
              "错误: 特征提取模式需要指定输入PCAP文件 (-f <file.pcap>)\n");
      print_usage(argv[0]);
      return -1;
    }

    // 必须有输出文件 (-c 或 -j)
    if (!args->output_file) {
      fprintf(stderr, "错误: 特征提取模式需要指定输出文件 (-c <output.csv> 或 "
                      "-j <output.json>)\n");
      fprintf(stderr, "提示: 使用 -c 参数指定CSV格式输出，或使用 -j "
                      "参数指定JSON格式输出\n");
      fprintf(stderr, "示例:\n");
      fprintf(stderr, "  %s -f input.pcap -c output.csv\n",
              (argc > 0) ? argv[0] : "ktag_demo");
      fprintf(stderr, "  %s -f input.pcap -j output.json\n",
              (argc > 0) ? argv[0] : "ktag_demo");
      return -1;
    }
    if (args->feature_config_file != nullptr) {
      if (args->num_pcap_files != 1) {
        fprintf(stderr, "错误: -f --config 每次只接受一个 PCAP；请为每个 PCAP "
                        "单独生成 CSV\n");
        return -1;
      }
      if (args->output_format != FILE_FORMAT_CSV) {
        fprintf(stderr,
                "错误: -f --config 的 Schema 特征输出必须使用 -c CSV\n");
        return -1;
      }
      if (args->with_rawbow) {
        fprintf(stderr, "错误: -w 只适用于未指定 --config 的旧固定向量路径\n");
        return -1;
      }
      if (args->inference_threads != 0U) {
        fprintf(stderr, "错误: Schema 特征提取不创建 Inference Worker\n");
        return -1;
      }
      if (args->pipeline_mode != nullptr &&
          strcmp(args->pipeline_mode, "legacy") == 0) {
        fprintf(stderr, "错误: -f --config 需要选择现代 Pipeline Mode\n");
        return -1;
      }
    }
  }

  // 3. 在线推理模式 (-r) 的验证
  if (args->operation == 3) {
    const bool distributed_frontend =
        args->distributed_role != nullptr &&
        strcmp(args->distributed_role, "frontend") == 0;
    const bool distributed_backend =
        args->distributed_role != nullptr &&
        strcmp(args->distributed_role, "backend") == 0;

    if (distributed_backend &&
        (args->num_pcap_files != 0 || args->interface != nullptr)) {
      fprintf(stderr, "错误: 分布式 Backend 的输入来自 UB，不能指定 -p/-i\n");
      return -1;
    }
    if (distributed_backend && args->output_file == nullptr) {
      fprintf(stderr,
              "错误: 分布式 Backend 需要用 -c 或 -j 指定推理结果文件\n");
      return -1;
    }
    if (distributed_frontend && args->output_file != nullptr) {
      fprintf(
          stderr,
          "错误: 分布式 Frontend 不产生推理结果，请在 Backend 指定 -c/-j\n");
      return -1;
    }

    // 独立模式和 Frontend 必须有输入源。
    if (!distributed_backend && args->num_pcap_files == 0 && !args->interface) {
      fprintf(stderr, "错误: 在线推理模式需要指定输入源 (-p <file.pcap> 或 -i "
                      "<interface>)\n");
      print_usage(argv[0]);
      return -1;
    }

    // 如果是PCAP文件输入，必须有输出文件
    if (!distributed_frontend && args->num_pcap_files > 0 &&
        !args->output_file) {
      fprintf(stderr, "错误: 在线推理模式（PCAP文件输入）需要指定输出文件 (-c "
                      "<output.csv> 或 -j <output.json>)\n");
      fprintf(stderr, "提示: 使用 -c 参数指定CSV格式输出，或使用 -j "
                      "参数指定JSON格式输出\n");
      fprintf(stderr, "示例:\n");
      fprintf(stderr,
              "  %s -r config.yaml -p file1.pcap -p file2.pcap -c result.csv\n",
              (argc > 0) ? argv[0] : "ktag_demo");
      fprintf(
          stderr,
          "  %s -r config.yaml -p file1.pcap -p file2.pcap -j result.json\n",
          (argc > 0) ? argv[0] : "ktag_demo");
      return -1;
    }

    // 如果是网口实时抓包，输出文件是可选的
    if (!distributed_frontend && args->interface && !args->output_file) {
      printf(
          "提示: 网口实时抓包模式未指定输出文件，推理结果将仅打印到控制台\n");
    }

    // -w 参数只能用于特征提取模式
    if (args->with_rawbow) {
      fprintf(stderr,
              "警告: -w 参数仅用于特征提取模式 (-f)，在推理模式中将被忽略\n");
      args->with_rawbow = 0;
    }
  }

  // 4. -w 参数只能用于特征提取模式
  if (args->with_rawbow && args->operation != 2) {
    fprintf(stderr,
            "警告: -w 参数仅用于特征提取模式 (-f)，在其他模式中将被忽略\n");
    args->with_rawbow = 0;
  }

  return 0;
}

int main(int argc, char *argv[]) {
  CommandLineArgs args;

  if (parse_args(argc, argv, &args) != 0) {
    return 1;
  }

  int ret = 0;

  switch (args.operation) {
  case 0:
    printf("========== 模型训练模式 ==========\n");
    return run_python(get_training_script(), args.config_file);

  case 1:
    printf("========== 模型验证模式 ==========\n");
    return run_python(get_evaluation_script(), args.config_file);

  case 2:
    // 特征提取模式
    if (args.feature_config_file != nullptr) {
      ktag_demo_pipeline_config pipeline_config;
      ktag_demo_pipeline_config_overrides overrides;
      overrides.force_enable = true;
      overrides.feature_only = true;
      overrides.feature_threads = args.feature_threads;
      if (args.pipeline_mode != nullptr) {
        overrides.mode_set = true;
        (void)ktag_demo_pipeline_mode_from_name(args.pipeline_mode,
                                                &overrides.mode);
      }
      std::string pipeline_error;
      const auto pipeline_result =
          ktag_demo_load_pipeline_config_with_overrides(
              args.feature_config_file, &overrides, &pipeline_config,
              &pipeline_error);
      if (pipeline_result != ktag_demo_pipeline_config_result::enabled) {
        fprintf(stderr, "Invalid feature pipeline configuration: %s\n",
                pipeline_error.c_str());
        ret = -1;
        break;
      }
      ret = ktag_demo_run_pipeline(
          pipeline_config, args.feature_config_file,
          (const char **)args.pcap_files, args.num_pcap_files, nullptr,
          args.output_file, ktag_demo_pipeline_output_format::csv);
    } else if (args.num_pcap_files > 0) {
      ret = feature_extraction_mode(args.pcap_files[0], args.output_file,
                                    args.with_rawbow);
    }
    break;

  case 3:
    // 在线推理模式
    {
      const bool force_legacy = args.pipeline_mode != nullptr &&
                                strcmp(args.pipeline_mode, "legacy") == 0;
      if (force_legacy && args.use_dpdk) {
        fprintf(stderr, "错误: DPDK 收包仅支持新的 pipeline 模式，"
                        "不能与 --pipeline-mode legacy 一起使用\n");
        ret = -1;
        break;
      }
      if (force_legacy) {
        ret = online_inference_mode(
            args.config_file, (const char **)args.pcap_files,
            args.num_pcap_files, args.interface, args.mode, args.output_file,
            args.output_format, args.pcap_labels);
        break;
      }

      ktag_demo_pipeline_config pipeline_config;
      ktag_demo_pipeline_config_overrides overrides;
      overrides.force_enable =
          args.pipeline_mode != nullptr || args.feature_threads != 0U ||
          args.inference_threads != 0U || args.distributed_role != nullptr ||
          args.use_dpdk != 0 || args.dpdk_eal_args != nullptr;
      overrides.feature_threads = args.feature_threads;
      overrides.inference_threads = args.inference_threads;
      if (args.distributed_role != nullptr) {
        overrides.distributed_role_set = true;
        overrides.node_id = args.node_id;
        (void)ktag_demo_distributed_role_from_name(args.distributed_role,
                                                   &overrides.distributed_role);
      }
      overrides.force_dpdk = args.use_dpdk != 0;
      overrides.dpdk_eal_args = args.dpdk_eal_args;
      if (args.pipeline_mode != nullptr) {
        overrides.mode_set = true;
        (void)ktag_demo_pipeline_mode_from_name(args.pipeline_mode,
                                                &overrides.mode);
      }
      std::string pipeline_error;
      const auto pipeline_result =
          ktag_demo_load_pipeline_config_with_overrides(
              args.config_file, &overrides, &pipeline_config, &pipeline_error);
      if (pipeline_result == ktag_demo_pipeline_config_result::error) {
        fprintf(stderr, "Invalid pipeline configuration: %s\n",
                pipeline_error.c_str());
        ret = -1;
      } else if (pipeline_result == ktag_demo_pipeline_config_result::enabled) {
        if (args.pcap_labels != nullptr) {
          pipeline_config.pcap_labels_specified = true;
          pipeline_config.pcap_labels = args.pcap_labels;
        }
        if (pipeline_config.capture_backend ==
            ktag_demo_capture_backend::dpdk) {
#ifdef KTAG_DEMO_HAVE_DPDK
          /* EAL init is idempotent and must run on the main thread; the
           * factory also inits lazily as a fallback. */
          const ktag_status eal_status =
              ktag_dpdk_eal_init(pipeline_config.dpdk_eal_args.c_str());
          if (eal_status != KTAG_SUCCESS) {
            fprintf(stderr, "DPDK EAL init failed: %s\n",
                    ktag_dpdk_capture_get_last_error());
            ret = -1;
            break;
          }
#else
          fprintf(stderr, "错误: DPDK 收包需要以 -DKTAG_ENABLE_DPDK=ON 编译\n");
          ret = -1;
          break;
#endif
        }
        const auto format = args.output_format == FILE_FORMAT_JSON
                                ? ktag_demo_pipeline_output_format::json
                                : ktag_demo_pipeline_output_format::csv;
        ret = ktag_demo_run_pipeline(
            pipeline_config, args.config_file, (const char **)args.pcap_files,
            args.num_pcap_files, args.interface, args.output_file, format);
      } else {
        ret = online_inference_mode(
            args.config_file, (const char **)args.pcap_files,
            args.num_pcap_files, args.interface, args.mode, args.output_file,
            args.output_format, args.pcap_labels);
      }
    }
    break;

  default:
    fprintf(stderr, "错误: 未知的操作模式\n");
    ret = 1;
  }

  free(args.pcap_files);
  return ret;
}
