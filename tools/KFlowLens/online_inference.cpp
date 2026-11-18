/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#include "online_inference.h"
#include "capture_adapter.h"
#include "feature_session.h"
#include "inference_session.h"
#include "utils.h"
#include "pcap_labels.h"

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
    if (!session.predict(input, &selected, &predictions, &error,
                         &stats.inference_time))
        throw std::runtime_error("inference failed: " + error);
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
void roundtrip_features(std::vector<feature_vector> &features) {
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
        const auto extracted = extractor->process(next.packet.data, next.packet.captured_length,
            &timestamp, &features, &ready, &stats.feature_extraction_time);
        if (extracted != KTAG_SUCCESS) throw std::runtime_error("feature extraction failed");
        if (ready) {
            if (immediate) {
                const feature_vector_list one{&features, 1U, 1U};
                predict_and_append(inference, one, label, live, output, stats);
            } else pending.push_back(features);
        }
    }
    feature_list_guard remaining;
    if (extractor->flush(&remaining.list, &stats.feature_extraction_time) != KTAG_SUCCESS)
        throw std::runtime_error("cannot flush remaining feature records");
    if (immediate) predict_and_append(inference, remaining.list, label, live, output, stats);
    else {
        if (remaining.list.count)
            pending.insert(pending.end(), remaining.list.vectors, remaining.list.vectors + remaining.list.count);
        if (mode == 2) roundtrip_features(pending);
        const auto batch = view(pending);
        predict_and_append(inference, batch, label, false, output, stats);
    }
}

void write_results(const char *path, file_format format, results &output) {
    if (!path || output.features.empty()) return;
    file_writer_config config{};
    config.file_path = path;
    config.format = format;
    config.mode = WRITE_MODE_WITH_PREDICTIONS;
    std::unique_ptr<file_writer, decltype(&writer_destroy)> writer(writer_create(&config), writer_destroy);
    auto features = view(output.features);
    if (!writer || writer_write_features_with_predictions(writer.get(), &features, output.predictions.data(),
            static_cast<uint32_t>(output.predictions.size())) != WRITER_SUCCESS || writer_flush(writer.get()) != WRITER_SUCCESS)
        throw std::runtime_error("cannot write inference results");
}
}

int online_inference_mode(const char *config_file, const char **pcap_files, int num_pcap_files,
                          const char *interface, int mode, char *output_file, file_format format,
                          const char *pcap_label_list) {
    try {
        if (mode < 0 || mode > 2 || (!interface && (!pcap_files || num_pcap_files <= 0)))
            throw std::runtime_error("invalid inference mode or missing capture source");
        std::string error;
        ktag_demo_pcap_labels pcap_labels;
        if (!interface) {
            pcap_labels = ktag_demo_load_pcap_labels(
                ktag_demo_model_package_path(config_file), pcap_label_list,
                pcap_files, num_pcap_files);
        }
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
                const int label = pcap_labels.label_for(static_cast<uint32_t>(i));
                std::printf("处理文件 %d/%d: %s (source_index=%d, ground_truth_label=%d)\n",
                            i + 1, num_pcap_files, pcap_files[i], i, label);
                run_source(*inference, pcap_files[i], false, mode, label, output, stats);
            }
            if (!output.labels.empty()) {
                for (int label : output.predictions) pcap_labels.validate_prediction(label);
                std::unique_ptr<ClassificationReport, decltype(&free_classification_report)> report(
                    calculate_classification_report(output.labels.data(), output.predictions.data(),
                        static_cast<int>(output.labels.size()), pcap_labels.report_labels, pcap_labels.description),
                    free_classification_report);
                if (!report || print_and_save_classification_report(report.get(), output_file) != 0)
                    throw std::runtime_error("cannot produce classification report");
            } else {
                std::printf("无有效推理样本，未生成 Classification Report；accuracy=N/A。\n");
            }
        }
        write_results(output_file, format, output);
        stats.end_to_end_time = get_time_in_seconds() - start;
        print_performance_stats(&stats);
        std::printf("过滤统计: 保留 %zu 个样本, 过滤掉 %llu 个样本\n", output.features.size(), static_cast<unsigned long long>(output.rejected));
        return 0;
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "在线推理失败: %s\n", exception.what());
        return -1;
    }
}
