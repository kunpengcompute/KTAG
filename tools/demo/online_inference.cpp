#include "feature_session.h"
/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "online_inference.h"
#include "ktag_file_writer.h"
#include "capture_adapter.h"
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static volatile sig_atomic_t g_interrupted = 0;
static ktag_capture_context *volatile g_active_capture = nullptr;

static void signal_handler(int signum)
{
    if (signum == SIGINT) {
        g_interrupted = 1;
        ktag_capture_interrupt(g_active_capture);
    }
}

static void setup_signal_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

int filter_features(const inference_engine *engine, const feature_vector_list *features, filtered_features *filtered)
{
    if (!engine || !features || !filtered) {
        return -1;
    }

    filtered->capacity = features->count;
    filtered->vectors = (feature_vector *)malloc(filtered->capacity * sizeof(feature_vector));
    if (!filtered->vectors) {
        fprintf(stderr, "错误: 无法分配过滤特征内存\n");
        return -1;
    }
    filtered->count = 0;
    filtered->filtered_count = 0;

    int filter_packets = engine->filter_packets;

    for (uint32_t j = 0; j < features->count; j++) {
        const feature_vector *fv = &features->vectors[j];

        if (fv->features[7] + fv->features[165] >= filter_packets) {
            filtered->vectors[filtered->count++] = *fv;
        } else {
            filtered->filtered_count++;
        }
    }
    return 0;
}

void free_filtered_features(filtered_features *filtered)
{
    if (filtered && filtered->vectors) {
        free(filtered->vectors);
        filtered->vectors = NULL;
        filtered->count = 0;
        filtered->capacity = 0;
    }
}

int online_inference_from_interface(inference_engine *engine, const char *interface, const char *output_file,
                                    file_format format)
{
    printf("\n========== 网口实时抓包模式 ==========\n");
    printf("网口: %s\n", interface);
    if (output_file) {
        printf("输出文件: %s (格式: %s)\n", output_file, format == FILE_FORMAT_CSV ? "CSV" : "JSON");
    } else {
        printf("输出: 仅控制台显示\n");
    }

    // 注册信号处理函数
    g_interrupted = 0;
    setup_signal_handler();

    char errbuf[1024] = {};
    const ktag_capture_config capture_config = make_live_capture_config(interface);
    ktag_capture_context *capture = ktag_capture_open(&capture_config);
    if (!capture) {
        snprintf(errbuf, sizeof(errbuf), "%s", ktag_capture_get_last_error());
        fprintf(stderr, "错误: 无法打开网口: %s\n", errbuf);
        return -1;
    }
    ktag_demo_feature_session *extractor = ktag_demo_feature_session::create(
        static_cast<int>(ktag_capture_get_link_type(capture)));
    if (!extractor) {
        fprintf(stderr, "错误: 无法初始化特征提取器\n");
        ktag_capture_close(capture);
        return -1;
    }

    feature_vector_list remaining = {0};
    double t1_final = 0.0;
    double end_time = 0.0;

    PerformanceStats stats = {0};
    double start_time = get_time_in_seconds();

    filtered_features all_filtered = {0};
    all_filtered.capacity = 1000;
    all_filtered.vectors = (feature_vector *)malloc(all_filtered.capacity * sizeof(feature_vector));
    if (!all_filtered.vectors) {
        fprintf(stderr, "错误: 无法分配过滤特征内存\n");
        delete extractor;
        ktag_capture_close(capture);
        return -1;
    }
    all_filtered.count = 0;
    all_filtered.filtered_count = 0;

    int *all_predictions = NULL;
    int predictions_capacity = 1000;
    int predictions_count = 0;
    all_predictions = (int *)malloc(predictions_capacity * sizeof(int));
    if (!all_predictions) {
        fprintf(stderr, "错误: 无法分配预测结果内存\n");
        free(all_filtered.vectors);
        delete extractor;
        ktag_capture_close(capture);
        return -1;
    }

    ktag_packet packet{};
    ktag_packet_init(&packet);
    int flow_count = 0;

    printf("开始实时抓包 (Ctrl+C 退出)...\n\n");

    g_active_capture = capture;
    while (!g_interrupted) {
        double t0 = get_time_in_seconds();
        const ktag_status capture_status = ktag_capture_next(capture, &packet);
        double t1 = get_time_in_seconds();

        if (capture_status == KTAG_STATUS_END_OF_STREAM)
            break;  // 错误
        if (capture_status == KTAG_STATUS_TIMEOUT)
            continue;  // 超时

        if (capture_status != KTAG_SUCCESS) {
            fprintf(stderr, "Capture read failed: %s\n", ktag_capture_get_last_error());
            break;
        }

        stats.pcap_read_time += (t1 - t0);
        stats.total_packets++;
        stats.total_bytes += packet.captured_length;

        feature_vector features;
        int has_feature = 0;

        const timeval timestamp = packet_timeval(packet);
        const int extract_status = extractor->process(packet.data, packet.captured_length, &timestamp,
            &features, &has_feature);
        ktag_packet_release(&packet);
        if (extract_status ==
            KTAG_SUCCESS) {
            double t2 = get_time_in_seconds();
            stats.feature_extraction_time += (t2 - t1);

            if (has_feature) {
                feature_vector_list fv_list = {&features, 1, 1};

                filtered_features filtered = {0};
                if (filter_features(engine, &fv_list, &filtered) == 0 && filtered.count > 0) {
                    if (all_filtered.count >= all_filtered.capacity) {
                        size_t new_capacity = all_filtered.capacity * 2;
                        feature_vector *new_vectors =
                            (feature_vector *)realloc(all_filtered.vectors, new_capacity * sizeof(feature_vector));
                        if (!new_vectors) {
                            fprintf(stderr, "错误: 扩容过滤特征列表失败\n");
                            free_filtered_features(&filtered);
                            goto cleanup;
                        }
                        all_filtered.vectors = new_vectors;
                        all_filtered.capacity = new_capacity;
                    }
                    all_filtered.vectors[all_filtered.count++] = filtered.vectors[0];

                    // 推理
                    inference_result result;
                    result.predictions = (int *)malloc(sizeof(int));
                    if (!result.predictions) {
                        fprintf(stderr, "错误: 无法分配推理结果内存\n");
                        free_filtered_features(&filtered);
                        goto cleanup;
                    }
                    result.count = 1;

                    double t3 = get_time_in_seconds();
                    if (inference_predict(engine, &fv_list, &result) == INFERENCE_SUCCESS) {
                        double t4 = get_time_in_seconds();
                        stats.inference_time += (t4 - t3);

                        if (predictions_count >= predictions_capacity) {
                            size_t new_capacity = predictions_capacity * 2;
                            int *new_predictions = (int *)realloc(all_predictions, new_capacity * sizeof(int));
                            if (!new_predictions) {
                                fprintf(stderr, "错误: 扩容预测结果列表失败\n");
                                free(result.predictions);
                                free_filtered_features(&filtered);
                                goto cleanup;
                            }
                            all_predictions = new_predictions;
                            predictions_capacity = new_capacity;
                        }
                        all_predictions[predictions_count++] = result.predictions[0];

                        printf("Flow %d: 预测类别 = %d\n", flow_count++, result.predictions[0]);
                    }
                    free(result.predictions);
                    free_filtered_features(&filtered);
                } else {
                    all_filtered.filtered_count++;
                }
            }
        }
    }

    // 处理中断信号后的清理工作
    if (g_interrupted) {
        printf("\n正在处理剩余流...\n");
    }

    remaining.capacity = 100000;
    remaining.vectors = (feature_vector *)malloc(remaining.capacity * sizeof(feature_vector));
    if (!remaining.vectors) {
        fprintf(stderr, "错误: 无法分配剩余流内存\n");
        goto cleanup;
    }

    t1_final = get_time_in_seconds();
    if (extractor->flush(&remaining) == KTAG_SUCCESS) {
        double t2 = get_time_in_seconds();
        stats.feature_extraction_time += (t2 - t1_final);

        if (remaining.count > 0) {
            printf("处理 %u 个剩余流...\n", remaining.count);

            // 过滤特征
            filtered_features filtered = {0};
            if (filter_features(engine, &remaining, &filtered) == 0 && filtered.count > 0) {
                // 保存过滤后的特征
                for (uint32_t i = 0; i < filtered.count; i++) {
                    if (all_filtered.count >= all_filtered.capacity) {
                        size_t new_capacity = all_filtered.capacity * 2;
                        feature_vector *new_vectors =
                            (feature_vector *)realloc(all_filtered.vectors, new_capacity * sizeof(feature_vector));
                        if (!new_vectors) {
                            fprintf(stderr, "错误: 扩容过滤特征列表失败\n");
                            free_filtered_features(&filtered);
                            if (remaining.vectors) {
                                free(remaining.vectors);
                                remaining.vectors = NULL;
                            }
                            goto cleanup;
                        }
                        all_filtered.vectors = new_vectors;
                        all_filtered.capacity = new_capacity;
                    }
                    all_filtered.vectors[all_filtered.count++] = filtered.vectors[i];
                }
                all_filtered.filtered_count += filtered.filtered_count;

                // 推理
                inference_result result;
                result.predictions = (int *)malloc(filtered.count * sizeof(int));
                if (!result.predictions) {
                    fprintf(stderr, "错误: 无法分配推理结果内存\n");
                    free_filtered_features(&filtered);
                    if (remaining.vectors) {
                        free(remaining.vectors);
                        remaining.vectors = NULL;
                    }
                    goto cleanup;
                }
                result.count = filtered.count;

                double t3 = get_time_in_seconds();
                if (inference_predict(engine, &remaining, &result) == INFERENCE_SUCCESS) {
                    double t4 = get_time_in_seconds();
                    stats.inference_time += (t4 - t3);

                    for (uint32_t i = 0; i < result.count; i++) {
                        if (predictions_count >= predictions_capacity) {
                            size_t new_capacity = predictions_capacity * 2;
                            int *new_predictions = (int *)realloc(all_predictions, new_capacity * sizeof(int));
                            if (!new_predictions) {
                                fprintf(stderr, "错误: 扩容预测结果列表失败\n");
                                free(result.predictions);
                                free_filtered_features(&filtered);
                                if (remaining.vectors) {
                                    free(remaining.vectors);
                                    remaining.vectors = NULL;
                                }
                                goto cleanup;
                            }
                            all_predictions = new_predictions;
                            predictions_capacity = new_capacity;
                        }
                        all_predictions[predictions_count++] = result.predictions[i];
                        printf("Flow %d: 预测类别 = %d\n", flow_count++, result.predictions[i]);
                    }
                }
                free(result.predictions);
                free_filtered_features(&filtered);
            }
        }
    }

    if (remaining.vectors) {
        free(remaining.vectors);
        remaining.vectors = NULL;
    }

    end_time = get_time_in_seconds();
    stats.total_time = stats.feature_extraction_time + stats.inference_time;
    stats.end_to_end_time = end_time - start_time;

    // 打印性能统计
    print_performance_stats(&stats);

    printf("\n过滤统计: 保留 %u 个样本, 过滤掉 %u 个样本\n", all_filtered.count, all_filtered.filtered_count);
    printf("总共处理: %d 个流\n", flow_count);

    // 保存结果到文件
    if (output_file && all_filtered.count > 0) {
        printf("\n正在保存结果到文件...\n");

        file_writer_config writer_config = {
            .file_path = output_file, .format = format, .mode = WRITE_MODE_WITH_PREDICTIONS, .append = 0};

        file_writer *writer = writer_create(&writer_config);
        if (writer) {
            feature_vector_list filtered_list = {all_filtered.vectors, all_filtered.count, all_filtered.count};

            double io_start = get_time_in_seconds();
            if (writer_write_features_with_predictions(writer, &filtered_list, all_predictions, predictions_count) ==
                WRITER_SUCCESS) {
                writer_flush(writer);
                double io_end = get_time_in_seconds();
                printf("推理结果已保存到: %s (耗时: %.4f 秒)\n", output_file, io_end - io_start);
            } else {
                fprintf(stderr, "错误: 保存结果失败\n");
            }
            writer_destroy(writer);
        } else {
            fprintf(stderr, "错误: 无法创建文件写入器\n");
        }
    }

cleanup:
    // 清理资源
    if (remaining.vectors) {
        free(remaining.vectors);
    }
    if (all_filtered.vectors) {
        free_filtered_features(&all_filtered);
    }
    if (all_predictions) {
        free(all_predictions);
    }
    g_active_capture = nullptr;
    ktag_packet_release(&packet);
    ktag_capture_close(capture);
    delete extractor;

    return 0;
}


/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include "online_inference.h"
#include "capture_adapter.h"
#include "feature_session.h"
#include "inference_session.h"
#include "utils.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
volatile sig_atomic_t interrupted = 0;
ktag_capture_context *volatile active_capture = nullptr;
void interrupt_capture(int signum) {
    if (signum == SIGINT) {
        interrupted = 1;
        ktag_capture_interrupt(active_capture);
    }
}
struct signal_guard {
    struct sigaction previous{};
    signal_guard() {
        struct sigaction action{};
        action.sa_handler = interrupt_capture;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &previous) != 0)
            throw std::runtime_error("cannot install capture interrupt handler");
        interrupted = 0;
    }
    ~signal_guard() {
        active_capture = nullptr;
        sigaction(SIGINT, &previous, nullptr);
    }
};
struct capture_deleter {
    void operator()(ktag_capture_context *capture) const {
        active_capture = nullptr;
        ktag_capture_close(capture);
    }
};
struct packet_guard {
    ktag_packet packet{};
    packet_guard() { ktag_packet_init(&packet); }
    ~packet_guard() { ktag_packet_release(&packet); }
};
struct feature_list_guard {
    feature_vector_list list{};
    ~feature_list_guard() { std::free(list.vectors); }
};
struct results {
    std::vector<feature_vector> features;
    std::vector<int> predictions;
    std::vector<int> labels;
    uint64_t rejected = 0;
};
feature_vector_list view(std::vector<feature_vector> &features) {
    if (features.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("too many feature records");
    const auto count = static_cast<uint32_t>(features.size());
    return {features.data(), count, count};
}
void predict_and_append(ktag_demo_inference_session &session,
                       const feature_vector_list &input, int label, bool live,
                       results &output, PerformanceStats &stats) {
    std::vector<feature_vector> selected;
    std::vector<int> predictions;
    std::string error;
    const auto start = get_time_in_seconds();
    if (!session.predict(input, &selected, &predictions, &error))
        throw std::runtime_error("inference failed: " + error);
    stats.inference_time += get_time_in_seconds() - start;
    stats.inference_inputs += input.count;
    stats.predictions += predictions.size();
    if (predictions.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - output.predictions.size())
        throw std::runtime_error("too many predictions for the Demo report format");
    output.rejected += input.count - selected.size();
    if (live) {
        for (size_t i = 0; i < predictions.size(); ++i)
            std::printf("Flow %zu: 预测类别 = %d\n", output.predictions.size() + i, predictions[i]);
    }
    output.features.insert(output.features.end(), selected.begin(), selected.end());
    output.predictions.insert(output.predictions.end(), predictions.begin(), predictions.end());
    if (!live) output.labels.insert(output.labels.end(), predictions.size(), label);
}

/* Preserve -m 2's disk round trip without a shared temp.csv filename. */
void roundtrip_features(std::vector<feature_vector> &features, PerformanceStats &stats) {
    if (features.empty()) return;
    struct temporary_file {
        std::string path;
        temporary_file() {
            auto pattern = (std::filesystem::temp_directory_path() / "ktag-inference-XXXXXX").string();
            const int fd = mkstemp(pattern.data());
            if (fd < 0) throw std::runtime_error("cannot create temporary feature file");
            path = std::move(pattern);
            close(fd);
        }
        ~temporary_file() { std::remove(path.c_str()); }
    } file;
    const auto start = get_time_in_seconds();
    file_writer_config config{};
    config.file_path = file.path.c_str();
    config.format = FILE_FORMAT_CSV;
    config.mode = WRITE_MODE_FEATURES_ONLY;
    std::unique_ptr<file_writer, decltype(&writer_destroy)> writer(writer_create(&config), writer_destroy);
    auto input = view(features);
    if (!writer || writer_write_features(writer.get(), &input) != WRITER_SUCCESS || writer_flush(writer.get()) != WRITER_SUCCESS)
        throw std::runtime_error("cannot write temporary feature file");
    writer.reset();
    std::ifstream stream(file.path);
    std::string row;
    if (!std::getline(stream, row)) throw std::runtime_error("missing feature CSV header");
    std::vector<feature_vector> loaded;
    loaded.reserve(features.size());
    while (std::getline(stream, row)) {
        std::istringstream columns(row);
        std::string field;
        feature_vector record{};
        for (uint32_t i = 0; i < KTAG_LEGACY_V1_VALUE_COUNT; ++i) {
            if (!std::getline(columns, field, ',')) throw std::runtime_error("incomplete feature CSV row");
            char *end = nullptr;
            errno = 0;
            record.features[i] = std::strtof(field.c_str(), &end);
            if (end == field.c_str() || errno == ERANGE) throw std::runtime_error("invalid feature CSV value");
            while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
            if (*end) throw std::runtime_error("invalid feature CSV value");
        }
        if (std::getline(columns, field, ',')) throw std::runtime_error("extra feature CSV column");
        loaded.push_back(record);
    }
    if (!stream.eof() || loaded.size() != features.size()) throw std::runtime_error("cannot read complete temporary feature file");
    features.swap(loaded);
    stats.file_io_time += get_time_in_seconds() - start;
}

void run_source(ktag_demo_inference_session &inference, const char *source,
                bool live, int mode, int label, results &output, PerformanceStats &stats) {
    const auto config = live ? make_live_capture_config(source) : make_offline_capture_config(source);
    std::unique_ptr<ktag_capture_context, capture_deleter> capture(ktag_capture_open(&config));
    if (!capture) throw std::runtime_error(ktag_capture_get_last_error());
    std::unique_ptr<ktag_demo_feature_session> extractor(ktag_demo_feature_session::create(
        static_cast<int>(ktag_capture_get_link_type(capture.get()))));
    if (!extractor) throw std::runtime_error("cannot initialize feature plugin session");
    active_capture = capture.get();
    const bool immediate = live || mode == 0;
    std::vector<feature_vector> pending;
    while (!interrupted) {
        packet_guard next;
        const auto start = get_time_in_seconds();
        const auto status = ktag_capture_next(capture.get(), &next.packet);
        const auto captured = get_time_in_seconds();
        stats.pcap_read_time += captured - start;
        if (status == KTAG_STATUS_END_OF_STREAM) break;
        if (status == KTAG_STATUS_TIMEOUT) continue;
        if (status != KTAG_SUCCESS) {
            if (interrupted) break;
            throw std::runtime_error(ktag_capture_get_last_error());
        }
        ++stats.total_packets;
        stats.total_bytes += next.packet.captured_length;
        feature_vector features{};
        int ready = 0;
        const auto timestamp = packet_timeval(next.packet);
        const auto extracted = extractor->process(next.packet.data, next.packet.captured_length, &timestamp, &features, &ready);
        stats.feature_extraction_time += get_time_in_seconds() - captured;
        if (extracted != KTAG_SUCCESS) throw std::runtime_error("feature extraction failed");
        if (ready) {
            if (immediate) {
                const feature_vector_list one{&features, 1U, 1U};
                predict_and_append(inference, one, label, live, output, stats);
            } else pending.push_back(features);
        }
    }
    const auto start = get_time_in_seconds();
    feature_list_guard remaining;
    if (extractor->flush(&remaining.list) != KTAG_SUCCESS) throw std::runtime_error("cannot flush remaining feature records");
    stats.feature_extraction_time += get_time_in_seconds() - start;
    if (immediate) predict_and_append(inference, remaining.list, label, live, output, stats);
    else {
        if (remaining.list.count)
            pending.insert(pending.end(), remaining.list.vectors, remaining.list.vectors + remaining.list.count);
        if (mode == 2) roundtrip_features(pending, stats);
        const auto batch = view(pending);
        predict_and_append(inference, batch, label, false, output, stats);
    }
}

void write_results(const char *path, file_format format, results &output, PerformanceStats &stats) {
    if (!path || output.features.empty()) return;
    const auto start = get_time_in_seconds();
    file_writer_config config{};
    config.file_path = path;
    config.format = format;
    config.mode = WRITE_MODE_WITH_PREDICTIONS;
    std::unique_ptr<file_writer, decltype(&writer_destroy)> writer(writer_create(&config), writer_destroy);
    auto features = view(output.features);
    if (!writer || writer_write_features_with_predictions(writer.get(), &features, output.predictions.data(),
            static_cast<uint32_t>(output.predictions.size())) != WRITER_SUCCESS || writer_flush(writer.get()) != WRITER_SUCCESS)
        throw std::runtime_error("cannot write inference results");
    stats.file_io_time += get_time_in_seconds() - start;
}
}

int online_inference_mode(const char *config_file, const char **pcap_files, int num_pcap_files,
                          const char *interface, int mode, char *output_file, file_format format) {
    try {
        if (mode < 0 || mode > 2 || (!interface && (!pcap_files || num_pcap_files <= 0)))
            throw std::runtime_error("invalid inference mode or missing capture source");
        std::string error;
        auto inference = ktag_demo_inference_session::create(config_file, &error);
        if (!inference) throw std::runtime_error("cannot initialize inference: " + error);
        std::printf("========== 在线推理模式（公共 Inference API） ==========\n");
        signal_guard signals;
        const auto start = get_time_in_seconds();
        PerformanceStats stats{};
        results output;
        if (interface) run_source(*inference, interface, true, 0, -1, output, stats);
        else {
            for (int i = 0; i < num_pcap_files && !interrupted; ++i) {
                std::printf("处理文件 %d/%d: %s (Label=%d)\n", i + 1, num_pcap_files, pcap_files[i], i);
                run_source(*inference, pcap_files[i], false, mode, i, output, stats);
            }
            if (!output.labels.empty()) {
                std::unique_ptr<ClassificationReport, decltype(&free_classification_report)> report(
                    calculate_classification_report(output.labels.data(), output.predictions.data(), static_cast<int>(output.labels.size())),
                    free_classification_report);
                if (!report || print_and_save_classification_report(report.get(), output_file) != 0)
                    throw std::runtime_error("cannot produce classification report");
            }
        }
        write_results(output_file, format, output, stats);
        stats.total_time = stats.feature_extraction_time + stats.inference_time + stats.file_io_time;
        stats.end_to_end_time = get_time_in_seconds() - start;
        print_performance_stats(&stats);
        std::printf("过滤统计: 保留 %zu 个样本, 过滤掉 %llu 个样本\n", output.features.size(), static_cast<unsigned long long>(output.rejected));
        return 0;
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "在线推理失败: %s\n", exception.what());
        return -1;
    }
}
