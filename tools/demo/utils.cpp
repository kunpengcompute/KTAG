/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "utils.h"
#include "pcap_labels.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/time.h>
#include <yaml-cpp/yaml.h>

namespace {
int parse_label(const std::string &text, const std::string &where) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    throw std::runtime_error(where + ": 标签不能为空");
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  int value = 0;
  for (size_t i = first; i <= last; ++i) {
    if (text[i] < '0' || text[i] > '9' ||
        value > (std::numeric_limits<int>::max() - (text[i] - '0')) / 10) {
      throw std::runtime_error(where + ": 标签必须为 0..INT_MAX 范围的十进制整数: " + text);
    }
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

std::string labels_text(const std::vector<int> &labels) {
  std::ostringstream text;
  text << '[';
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i) text << ',';
    text << labels[i];
  }
  text << ']';
  return text.str();
}
} // namespace

std::string ktag_demo_model_package_path(const char *config_path) {
  if (!config_path || !config_path[0]) throw std::runtime_error("config path is required");
  const auto config = std::filesystem::absolute(config_path);
  const auto yaml = YAML::LoadFile(config.string());
  const auto pipeline = yaml["pipeline"];
  if (!pipeline || !pipeline.IsMap() || !pipeline["model_package_path"] ||
      !pipeline["model_package_path"].IsScalar()) {
    throw std::runtime_error("pipeline.model_package_path is required");
  }
  const auto configured = pipeline["model_package_path"].as<std::string>();
  if (configured.empty()) throw std::runtime_error("model_package_path cannot be empty");
  std::filesystem::path package(configured);
  if (package.is_relative()) package = config.parent_path() / package;
  return package.lexically_normal().string();
}

int ktag_demo_pcap_labels::label_for(uint32_t source_index) const {
  if (source_index >= source_labels.size()) {
    throw std::runtime_error("预测记录的 source_index 超出 PCAP 标签列表");
  }
  return source_labels[source_index];
}

void ktag_demo_pcap_labels::validate_prediction(int64_t label) const {
  if (label < 0 || label > std::numeric_limits<int>::max() ||
      !std::binary_search(model_labels.begin(), model_labels.end(), static_cast<int>(label))) {
    throw std::runtime_error("模型返回了未在 class_labels 中声明的类别: " + std::to_string(label));
  }
}

ktag_demo_pcap_labels ktag_demo_load_pcap_labels(
    const std::string &model_package_path, const char *label_list,
    const char **pcap_files, int pcap_count) {
  if (!pcap_files || pcap_count <= 0) throw std::runtime_error("PCAP 输入不能为空");
  ktag_demo_pcap_labels plan;
  plan.explicit_labels = label_list != nullptr;
  if (label_list) {
    const std::string text(label_list);
    size_t offset = 0;
    for (;;) {
      const auto comma = text.find(',', offset);
      plan.source_labels.push_back(parse_label(
          text.substr(offset, comma == std::string::npos ? comma : comma - offset),
          "--pcap-labels"));
      if (comma == std::string::npos) break;
      offset = comma + 1;
    }
  } else {
    for (int i = 0; i < pcap_count; ++i) plan.source_labels.push_back(i);
  }
  if (plan.source_labels.size() != static_cast<size_t>(pcap_count)) {
    throw std::runtime_error("--pcap-labels 标签数 " + std::to_string(plan.source_labels.size()) +
                             " 与 PCAP 数量 " + std::to_string(pcap_count) + " 不一致");
  }

  const auto manifest_path = std::filesystem::path(model_package_path) / "manifest.json";
  const auto manifest = YAML::LoadFile(manifest_path.string());
  const auto classes = manifest["class_labels"];
  if (!classes || !classes.IsSequence() || classes.size() == 0) {
    throw std::runtime_error(manifest_path.string() +
        ": 缺少有效的 class_labels，请使用当前版本 -t 重新导出模型包");
  }
  for (const auto &item : classes) {
    if (!item.IsScalar() || item.Tag() == "!" ||
        item.Tag() == "tag:yaml.org,2002:str") {
      throw std::runtime_error("manifest.class_labels 必须是整数数组");
    }
    plan.model_labels.push_back(parse_label(item.as<std::string>(), "manifest.class_labels"));
  }
  std::sort(plan.model_labels.begin(), plan.model_labels.end());
  if (std::adjacent_find(plan.model_labels.begin(), plan.model_labels.end()) != plan.model_labels.end()) {
    throw std::runtime_error("manifest.class_labels 包含重复类别");
  }
  for (size_t i = 0; i < plan.source_labels.size(); ++i) {
    if (!std::binary_search(plan.model_labels.begin(), plan.model_labels.end(), plan.source_labels[i])) {
      throw std::runtime_error("PCAP[" + std::to_string(i) + "] 的真实标签 " +
          std::to_string(plan.source_labels[i]) + " 不属于当前模型，支持类别为 " + labels_text(plan.model_labels));
    }
  }
  plan.report_labels = plan.source_labels;
  std::sort(plan.report_labels.begin(), plan.report_labels.end());
  plan.report_labels.erase(std::unique(plan.report_labels.begin(), plan.report_labels.end()), plan.report_labels.end());
  std::ostringstream description;
  if (!plan.explicit_labels) {
    description << "WARNING: 未指定 --pcap-labels，按 PCAP 顺序假定真实标签；请确认与模型类别一致。\n";
  }
  for (int i = 0; i < pcap_count; ++i) {
    description << "PCAP[" << i << "]: " << pcap_files[i]
                << " -> ground_truth_label=" << plan.source_labels[i]
                << " (source=" << (plan.explicit_labels ? "explicit" : "pcap_order") << ")\n";
  }
  description << "报告类别: " << labels_text(plan.report_labels)
              << "；macro average 仅统计这些类别（包括已选但无有效样本的类别）。\n";
  plan.description = description.str();
  std::printf("%s", plan.description.c_str());
  return plan;
}

double get_time_in_seconds() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// 计算准确率
double calculate_accuracy(const int *y_true, const int *y_pred, int count) {
  int correct = 0;
  for (int i = 0; i < count; i++) {
    if (y_true[i] == y_pred[i]) {
      correct++;
    }
  }
  return count > 0 ? (double)correct / count : 0.0;
}

// 计算分类指标
bool calculate_class_metrics(const int *classes, int num_classes,
                             const int *y_true, const int *y_pred, int count,
                             ClassificationReport *report) {
  report->classes = (int *)malloc(num_classes * sizeof(int));
  report->precision = (double *)calloc(num_classes, sizeof(double));
  report->recall = (double *)calloc(num_classes, sizeof(double));
  report->f1_score = (double *)calloc(num_classes, sizeof(double));
  report->support = (int *)calloc(num_classes, sizeof(int));
  report->num_classes = num_classes;
  if (!report->classes || !report->precision || !report->recall ||
      !report->f1_score || !report->support) {
    return false;
  }

  memcpy(report->classes, classes, num_classes * sizeof(int));

  for (int c_idx = 0; c_idx < num_classes; c_idx++) {
    int c = classes[c_idx];
    int tp = 0, fp = 0, fn = 0;

    for (int i = 0; i < count; i++) {
      if (y_true[i] == c && y_pred[i] == c) {
        tp++;
      } else if (y_pred[i] == c && y_true[i] != c) {
        fp++;
      } else if (y_true[i] == c && y_pred[i] != c) {
        fn++;
      }

      if (y_true[i] == c) {
        report->support[c_idx]++;
      }
    }

    // 计算精确率
    if (tp + fp == 0) {
      report->precision[c_idx] = 0.0;
    } else {
      report->precision[c_idx] = (double)tp / (tp + fp);
    }

    // 计算召回率
    if (tp + fn == 0) {
      report->recall[c_idx] = 0.0;
    } else {
      report->recall[c_idx] = (double)tp / (tp + fn);
    }

    // 计算F1分数
    if (report->precision[c_idx] + report->recall[c_idx] == 0) {
      report->f1_score[c_idx] = 0.0;
    } else {
      report->f1_score[c_idx] =
          2 * (report->precision[c_idx] * report->recall[c_idx]) /
          (report->precision[c_idx] + report->recall[c_idx]);
    }
  }
  return true;
}

// 计算平均指标
void calculate_average_metrics(const ClassificationReport *report) {
  ClassificationReport *rep = (ClassificationReport *)report;
  int total_samples = 0;

  rep->macro_avg_precision = 0.0;
  rep->macro_avg_recall = 0.0;
  rep->macro_avg_f1 = 0.0;
  rep->weighted_avg_precision = 0.0;
  rep->weighted_avg_recall = 0.0;
  rep->weighted_avg_f1 = 0.0;

  // 宏平均
  for (int i = 0; i < report->num_classes; i++) {
    rep->macro_avg_precision += report->precision[i];
    rep->macro_avg_recall += report->recall[i];
    rep->macro_avg_f1 += report->f1_score[i];
  }

  if (report->num_classes > 0) {
    rep->macro_avg_precision /= report->num_classes;
    rep->macro_avg_recall /= report->num_classes;
    rep->macro_avg_f1 /= report->num_classes;
  }

  // 加权平均
  for (int i = 0; i < report->num_classes; i++) {
    rep->weighted_avg_precision += report->precision[i] * report->support[i];
    rep->weighted_avg_recall += report->recall[i] * report->support[i];
    rep->weighted_avg_f1 += report->f1_score[i] * report->support[i];
    total_samples += report->support[i];
  }

  if (total_samples > 0) {
    rep->weighted_avg_precision /= total_samples;
    rep->weighted_avg_recall /= total_samples;
    rep->weighted_avg_f1 /= total_samples;
  }
}

// 生成分类报告
ClassificationReport *calculate_classification_report(const int *y_true,
                                                      const int *y_pred,
                                                      int count,
                                                      const std::vector<int> &report_labels,
                                                      const std::string &context) {
  if (count <= 0 || !y_true || !y_pred || report_labels.empty() ||
      report_labels.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    fprintf(stderr, "错误: 无有效样本或报告类别，accuracy=N/A\n");
    return NULL;
  }
  auto classes = report_labels;
  std::sort(classes.begin(), classes.end());
  classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
  for (int i = 0; i < count; ++i) {
    if (!std::binary_search(classes.begin(), classes.end(), y_true[i])) {
      fprintf(stderr, "错误: 真实标签 %d 不在报告类别集合中\n", y_true[i]);
      return NULL;
    }
  }
  ClassificationReport *report =
      (ClassificationReport *)calloc(1, sizeof(ClassificationReport));
  if (!report) return NULL;
  report->context = static_cast<char *>(malloc(context.size() + 1));
  if (!report->context) {
    free_classification_report(report);
    return NULL;
  }
  std::memcpy(report->context, context.c_str(), context.size() + 1);
  if (!calculate_class_metrics(classes.data(), static_cast<int>(classes.size()), y_true, y_pred, count, report)) {
    free_classification_report(report);
    return NULL;
  }
  for (int i = 0; i < count; ++i) {
    if (!std::binary_search(classes.begin(), classes.end(), y_pred[i])) {
      ++report->outside_report_predictions;
    }
  }
  report->accuracy = calculate_accuracy(y_true, y_pred, count);
  calculate_average_metrics(report);
  return report;
}

// 释放分类报告
void free_classification_report(ClassificationReport *report) {
  if (report) {
    free(report->context);
    free(report->classes);
    free(report->precision);
    free(report->recall);
    free(report->f1_score);
    free(report->support);
    free(report);
  }
}

static void
print_classification_report_to_stream(FILE *fp,
                                      const ClassificationReport *report) {
  if (!fp || !report) {
    return;
  }

  if (report->context && report->context[0]) {
    fprintf(fp, "%s\n", report->context);
  }

  int total_support = 0;
  for (int i = 0; i < report->num_classes; i++) {
    total_support += report->support[i];
  }

  fprintf(fp, "%13s %6s %8s %10s %9s\n\n", "", "precision", "recall",
          "f1-score", "support");

  for (int i = 0; i < report->num_classes; i++) {
    fprintf(fp, "%12d %9.4f %9.4f %9.4f %9d\n", report->classes[i],
            report->precision[i], report->recall[i], report->f1_score[i],
            report->support[i]);
  }

  for (int i = 0; i < report->num_classes; ++i) {
    if (report->support[i] == 0) {
      fprintf(fp, "提示: 类别 %d 无有效真实样本，指标按零值规则输出并计入 macro average。\n", report->classes[i]);
    }
  }
  if (report->outside_report_predictions) {
    fprintf(fp, "预测落入报告范围外的样本数: %d（仍计入错误统计）\n", report->outside_report_predictions);
  }

  fprintf(fp, "\n");

  fprintf(fp, "%12s %9s %9s %9.4f %9d\n", "accuracy", "", "", report->accuracy,
          total_support);

  fprintf(fp, "%12s %9.4f %9.4f %9.4f %9d\n", "macro avg",
          report->macro_avg_precision, report->macro_avg_recall,
          report->macro_avg_f1, total_support);

  fprintf(fp, "%12s %9.4f %9.4f %9.4f %9d\n", "weighted avg",
          report->weighted_avg_precision, report->weighted_avg_recall,
          report->weighted_avg_f1, total_support);
}

int print_classification_report(const ClassificationReport *report) {
  if (!report) {
    fprintf(stderr, "错误: 分类报告为空\n");
    return -1;
  }

  printf("\n");
  print_classification_report_to_stream(stdout, report);
  printf("\n");

  return 0;
}

int save_classification_report(const ClassificationReport *report,
                               const char *output_file) {
  if (!report || !output_file) {
    fprintf(stderr, "错误: 分类报告为空\n");
    return -1;
  }

  const auto report_file = (std::filesystem::path(output_file).parent_path() /
                            "classification_report.txt").string();

  FILE *fp = fopen(report_file.c_str(), "w");
  if (!fp) {
    fprintf(stderr, "错误: 无法创建分类报告文件 %s: %s\n", report_file.c_str(), std::strerror(errno));
    return -1;
  }

  errno = 0;
  print_classification_report_to_stream(fp, report);
  int code = 0;
  if (ferror(fp) || fflush(fp) != 0) code = errno == 0 ? EIO : errno;
  if (fclose(fp) != 0 && code == 0) code = errno == 0 ? EIO : errno;
  if (code != 0) {
    fprintf(stderr, "错误: 写入分类报告失败 %s: %s (errno=%d)\n",
            report_file.c_str(), std::strerror(code), code);
    return -1;
  }
  printf("分类报告已保存到: %s\n", report_file.c_str());
  return 0;
}

int print_and_save_classification_report(const ClassificationReport *report,
                                         const char *output_file) {
  if (!report) {
    fprintf(stderr, "错误: 分类报告为空\n");
    return -1;
  }

  printf("\n======================= Classification Report "
         "=======================\n");
  if (print_classification_report(report) != 0) {
    return -1;
  }

  if (output_file) {
    if (save_classification_report(report, output_file) != 0) {
      return -1;
    }
  }

  return 0;
}

// 打印性能统计
void print_performance_stats(const PerformanceStats *stats) {
  print_performance_stats_ex(stats, 0);
}

void print_performance_stats_ex(const PerformanceStats *stats,
                                int stages_overlap) {
  printf("\n======================= 性能统计 =======================\n");
  printf(stages_overlap ? "PCAP读取累计耗时: %10.4f 秒\n"
                        : "PCAP读取耗时:       %10.4f 秒\n",
         stats->pcap_read_time);
  if (stages_overlap) {
    printf("特征计算耗时（最忙线程）: %10.4f 秒\n",
           stats->feature_extraction_time);
    printf("特征CPU累计计算量: %10.4f CPU秒\n",
           stats->feature_cpu_time);
    if (stats->inference_time > 0) {
      printf("推理计算耗时（最忙线程）: %10.4f 秒\n",
             stats->inference_time);
      printf("推理CPU累计计算量: %10.4f CPU秒\n",
             stats->inference_cpu_time);
    }
  } else {
    printf("特征提取耗时:         %10.4f 秒\n",
           stats->feature_extraction_time);
    printf("推理耗时:           %10.4f 秒\n", stats->inference_time);
  }
  printf("文件IO耗时:         %10.4f 秒\n", stats->file_io_time);
  printf(stages_overlap
             ? "核心累计耗时:   %10.4f 秒 (多线程阶段可并行重叠)\n"
             : "总耗时:           %10.4f 秒 (特征提取 + 推理 + 文件IO)\n",
         stats->total_time);
  printf("端到端总耗时:       %10.4f 秒 (包含PCAP读取等所有开销)\n",
         stats->end_to_end_time);
  printf("处理包数:           %10lu 个\n", stats->total_packets);
  printf("总数据量:           %10.4f MB\n",
         stats->total_bytes / (1024.0 * 1024.0));

  if (stats->feature_extraction_time > 0) {
    double mbps = (stats->total_bytes / (1024.0 * 1024.0)) /
                  stats->feature_extraction_time;
    double gbps = (stats->total_bytes * 8.0) / (1024.0 * 1024.0 * 1024.0) /
                  stats->feature_extraction_time;
    printf(stages_overlap
               ? "特征计算吞吐量: %10.4f MB/s (%10.4f Gbps)\n"
               : "特征提取吞吐量: %10.4f MB/s (%10.4f Gbps)\n",
           mbps, gbps);
  }

  if (stats->inference_time > 0) {
    double mbps =
        (stats->total_bytes / (1024.0 * 1024.0)) / stats->inference_time;
    double gbps = (stats->total_bytes * 8.0) / (1024.0 * 1024.0 * 1024.0) /
                  stats->inference_time;
    printf(stages_overlap
               ? "推理计算吞吐量: %10.4f MB/s (%10.4f Gbps)\n"
               : "推理吞吐量:     %10.4f MB/s (%10.4f Gbps)\n",
           mbps, gbps);
  }

  if (stages_overlap && stats->inference_time > 0 &&
      stats->inference_inputs > 0) {
    const double samples_per_second =
        stats->inference_inputs / stats->inference_time;
    printf("推理计算样本率: %10.4f samples/s\n", samples_per_second);
  }

  if (stats->total_time > 0) {
    double mbps = (stats->total_bytes / (1024.0 * 1024.0)) / stats->total_time;
    double gbps = (stats->total_bytes * 8.0) / (1024.0 * 1024.0 * 1024.0) /
                  stats->total_time;
    printf(stages_overlap ? "核心处理吞吐量: %10.4f MB/s (%10.4f Gbps)\n"
                          : "核心处理吞吐量: %10.4f MB/s (%10.4f Gbps)\n",
           mbps, gbps);
  }

  if (stats->end_to_end_time > 0) {
    double mbps =
        (stats->total_bytes / (1024.0 * 1024.0)) / stats->end_to_end_time;
    double gbps = (stats->total_bytes * 8.0) / (1024.0 * 1024.0 * 1024.0) /
                  stats->end_to_end_time;
    printf("端到端吞吐量:   %10.4f MB/s (%10.4f Gbps)\n", mbps, gbps);
  }
  printf("========================================================\n\n");
}
