/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "online_inference.h"
#include "kdadk_file_writer.h"
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

int expand_features_capacity(feature_vector_list *list)
{
    size_t new_capacity = list->capacity * 2;
    feature_vector *new_vectors = (feature_vector *)realloc(list->vectors, new_capacity * sizeof(feature_vector));
    if (!new_vectors) {
        return -1;
    }
    list->vectors = new_vectors;
    list->capacity = new_capacity;
    return 0;
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

int inference_mode_single_flow(feature_extractor *extractor, inference_engine *engine, ktag_capture_context *capture,
                               int ground_truth_label, filtered_features *all_filtered_out, int **all_predictions,
                               int **all_labels, int *total_count, PerformanceStats *stats)
{
    ktag_packet packet{};
    ktag_packet_init(&packet);
    feature_vector_list remaining = {0};
    double extract_time = 0, infer_time = 0, pcap_time = 0;

    int capacity = 1000;
    *all_predictions = (int *)malloc(capacity * sizeof(int));
    if (!*all_predictions) {
        fprintf(stderr, "错误: 无法分配预测结果内存\n");
        return -1;
    }

    *all_labels = (int *)malloc(capacity * sizeof(int));
    if (!*all_labels) {
        fprintf(stderr, "错误: 无法分配标签内存\n");
        free(*all_predictions);
        *all_predictions = NULL;
        return -1;
    }
    *total_count = 0;

    all_filtered_out->capacity = 1000;
    all_filtered_out->vectors = (feature_vector *)malloc(all_filtered_out->capacity * sizeof(feature_vector));
    if (!all_filtered_out->vectors) {
        fprintf(stderr, "错误: 无法分配过滤特征内存\n");
        free(*all_predictions);
        free(*all_labels);
        *all_predictions = NULL;
        *all_labels = NULL;
        return -1;
    }
    all_filtered_out->count = 0;
    all_filtered_out->filtered_count = 0;

    while (1) {
        double t0 = get_time_in_seconds();
        const ktag_status capture_status = ktag_capture_next(capture, &packet);
        double t1 = get_time_in_seconds();

        if (capture_status == KTAG_STATUS_END_OF_STREAM)
            break;
        if (capture_status == KTAG_STATUS_TIMEOUT)
            continue;
        if (capture_status != KTAG_SUCCESS) {
            fprintf(stderr, "Capture read failed: %s\n", ktag_capture_get_last_error());
            break;
        }

        pcap_time += (t1 - t0);
        stats->total_packets++;
        stats->total_bytes += packet.captured_length;

        feature_vector features;
        int has_feature = 0;

        const timeval timestamp = packet_timeval(packet);
        const int extract_status = process_packet(
            extractor, packet.data, packet.captured_length, &timestamp,
            &features, &has_feature);
        ktag_packet_release(&packet);
        if (extract_status ==
            EXTRACTOR_SUCCESS) {
            double t2 = get_time_in_seconds();
            extract_time += (t2 - t1);

            if (has_feature) {
                feature_vector_list fv_list = {&features, 1, 1};

                filtered_features filtered = {0};
                if (filter_features(engine, &fv_list, &filtered) == 0 && filtered.count > 0) {
                    if (all_filtered_out->count >= all_filtered_out->capacity) {
                        size_t new_capacity = all_filtered_out->capacity * 2;
                        feature_vector *new_vectors =
                            (feature_vector *)realloc(all_filtered_out->vectors, new_capacity * sizeof(feature_vector));
                        if (!new_vectors) {
                            fprintf(stderr, "错误: 扩容过滤特征列表失败\n");
                            free_filtered_features(&filtered);
                            continue;
                        }
                        all_filtered_out->vectors = new_vectors;
                        all_filtered_out->capacity = new_capacity;
                    }
                    all_filtered_out->vectors[all_filtered_out->count++] = filtered.vectors[0];
                    all_filtered_out->filtered_count += filtered.filtered_count;

                    inference_result result;
                    result.predictions = (int *)malloc(sizeof(int));
                    if (!result.predictions) {
                        fprintf(stderr, "错误: 无法分配推理结果内存\n");
                        free_filtered_features(&filtered);
                        continue;
                    }
                    result.count = 1;

                    double t3 = get_time_in_seconds();
                    if (inference_predict(engine, &fv_list, &result) == INFERENCE_SUCCESS) {
                        double t4 = get_time_in_seconds();
                        infer_time += (t4 - t3);

                        if (*total_count >= capacity) {
                            size_t new_capacity = capacity * 2;
                            int *new_predictions = (int *)realloc(*all_predictions, new_capacity * sizeof(int));
                            int *new_labels = (int *)realloc(*all_labels, new_capacity * sizeof(int));
                            if (!new_predictions || !new_labels) {
                                fprintf(stderr, "错误: 扩容预测/标签列表失败\n");
                                free(result.predictions);
                                free_filtered_features(&filtered);
                                continue;
                            }
                            *all_predictions = new_predictions;
                            *all_labels = new_labels;
                            capacity = new_capacity;
                        }
                        (*all_predictions)[*total_count] = result.predictions[0];
                        (*all_labels)[*total_count] = ground_truth_label;
                        (*total_count)++;
                    }
                    free(result.predictions);
                    free_filtered_features(&filtered);
                } else {
                    all_filtered_out->filtered_count++;
                }
            }
        }
    }

    remaining.capacity = 100000;
    remaining.vectors = (feature_vector *)malloc(remaining.capacity * sizeof(feature_vector));
    if (!remaining.vectors) {
        fprintf(stderr, "错误: 无法分配剩余流内存\n");
        stats->pcap_read_time += pcap_time;
        stats->feature_extraction_time += extract_time;
        stats->inference_time += infer_time;
        return 0;
    }

    double t1_r = get_time_in_seconds();
    if (extractor_finalize(extractor, &remaining) == EXTRACTOR_SUCCESS) {
        double t2 = get_time_in_seconds();
        extract_time += (t2 - t1_r);

        if (remaining.count > 0) {
            filtered_features filtered = {0};
            if (filter_features(engine, &remaining, &filtered) == 0 && filtered.count > 0) {
                for (uint32_t i = 0; i < filtered.count; i++) {
                    if (all_filtered_out->count >= all_filtered_out->capacity) {
                        size_t new_capacity = all_filtered_out->capacity * 2;
                        feature_vector *new_vectors =
                            (feature_vector *)realloc(all_filtered_out->vectors, new_capacity * sizeof(feature_vector));
                        if (!new_vectors) {
                            fprintf(stderr, "错误: 扩容过滤特征列表失败\n");
                            break;
                        }
                        all_filtered_out->vectors = new_vectors;
                        all_filtered_out->capacity = new_capacity;
                    }
                    all_filtered_out->vectors[all_filtered_out->count++] = filtered.vectors[i];
                }
                all_filtered_out->filtered_count += filtered.filtered_count;

                inference_result result;
                result.predictions = (int *)malloc(filtered.count * sizeof(int));
                if (result.predictions) {
                    result.count = filtered.count;

                    double t3 = get_time_in_seconds();
                    if (inference_predict(engine, &remaining, &result) == INFERENCE_SUCCESS) {
                        double t4 = get_time_in_seconds();
                        infer_time += (t4 - t3);

                        for (uint32_t i = 0; i < result.count; i++) {
                            if (*total_count >= capacity) {
                                size_t new_capacity = capacity * 2;
                                int *new_predictions = (int *)realloc(*all_predictions, new_capacity * sizeof(int));
                                int *new_labels = (int *)realloc(*all_labels, new_capacity * sizeof(int));
                                if (!new_predictions || !new_labels) {
                                    fprintf(stderr, "错误: 扩容预测/标签列表失败\n");
                                    break;
                                }
                                *all_predictions = new_predictions;
                                *all_labels = new_labels;
                                capacity = new_capacity;
                            }
                            (*all_predictions)[*total_count] = result.predictions[i];
                            (*all_labels)[*total_count] = ground_truth_label;
                            (*total_count)++;
                        }
                    }
                    free(result.predictions);
                }
                free_filtered_features(&filtered);
            }
        }
        free(remaining.vectors);
    }

    stats->pcap_read_time += pcap_time;
    stats->feature_extraction_time += extract_time;
    stats->inference_time += infer_time;

    return 0;
}

int inference_mode_batch(feature_extractor *extractor, inference_engine *engine, ktag_capture_context *capture,
                         int ground_truth_label,
                         filtered_features *all_filtered_out, int **all_predictions, int **all_labels, int *total_count,
                         PerformanceStats *stats)
{
    ktag_packet packet{};
    ktag_packet_init(&packet);
    feature_vector_list remaining = {0};
    double extract_time = 0, pcap_time = 0;

    feature_vector_list all_features = {0};
    all_features.capacity = 1000;
    all_features.vectors = (feature_vector *)malloc(all_features.capacity * sizeof(feature_vector));
    if (!all_features.vectors) {
        fprintf(stderr, "错误: 无法分配特征向量内存\n");
        return -1;
    }

    while (1) {
        double t0 = get_time_in_seconds();
        const ktag_status capture_status = ktag_capture_next(capture, &packet);
        double t1 = get_time_in_seconds();

        if (capture_status == KTAG_STATUS_END_OF_STREAM)
            break;
        if (capture_status == KTAG_STATUS_TIMEOUT)
            continue;
        if (capture_status != KTAG_SUCCESS) {
            fprintf(stderr, "Capture read failed: %s\n", ktag_capture_get_last_error());
            break;
        }

        pcap_time += (t1 - t0);
        stats->total_packets++;
        stats->total_bytes += packet.captured_length;

        feature_vector features;
        int has_feature = 0;

        const timeval timestamp = packet_timeval(packet);
        const int extract_status = process_packet(
            extractor, packet.data, packet.captured_length, &timestamp,
            &features, &has_feature);
        ktag_packet_release(&packet);
        if (extract_status ==
            EXTRACTOR_SUCCESS) {
            double t2 = get_time_in_seconds();
            extract_time += (t2 - t1);

            if (has_feature) {
                if (all_features.count >= all_features.capacity) {
                    if (expand_features_capacity(&all_features) != 0) {
                        fprintf(stderr, "错误: 扩容特征列表失败\n");
                        continue;
                    }
                }
                all_features.vectors[all_features.count++] = features;
            }
        }
    }

    remaining.capacity = 100000;
    remaining.vectors = (feature_vector *)malloc(remaining.capacity * sizeof(feature_vector));
    if (!remaining.vectors) {
        fprintf(stderr, "错误: 无法分配剩余流内存\n");
        free(all_features.vectors);
        stats->pcap_read_time += pcap_time;
        stats->feature_extraction_time += extract_time;
        return -1;
    }

    double t1_r = get_time_in_seconds();
    if (extractor_finalize(extractor, &remaining) == EXTRACTOR_SUCCESS) {
        double t2 = get_time_in_seconds();
        extract_time += (t2 - t1_r);

        for (uint32_t i = 0; i < remaining.count; i++) {
            if (all_features.count >= all_features.capacity) {
                if (expand_features_capacity(&all_features) != 0) {
                    fprintf(stderr, "错误: 扩容特征列表失败\n");
                    break;
                }
            }
            all_features.vectors[all_features.count++] = remaining.vectors[i];
        }
        free(remaining.vectors);
    } else {
        free(remaining.vectors);
    }

    stats->pcap_read_time += pcap_time;
    stats->feature_extraction_time += extract_time;

    filtered_features filtered = {0};
    if (filter_features(engine, &all_features, &filtered) == 0 && filtered.count > 0) {
        *all_filtered_out = filtered;

        inference_result result;
        result.predictions = (int *)malloc(filtered.count * sizeof(int));
        if (!result.predictions) {
            fprintf(stderr, "错误: 无法分配推理结果内存\n");
            free(all_features.vectors);
            return -1;
        }
        result.count = filtered.count;

        double t3 = get_time_in_seconds();
        if (inference_predict(engine, &all_features, &result) == INFERENCE_SUCCESS) {
            double t4 = get_time_in_seconds();
            stats->inference_time += (t4 - t3);

            *all_predictions = result.predictions;
            *all_labels = (int *)malloc(result.count * sizeof(int));
            if (!*all_labels) {
                fprintf(stderr, "错误: 无法分配标签内存\n");
                free(result.predictions);
                free(all_features.vectors);
                return -1;
            }
            for (uint32_t i = 0; i < result.count; i++) {
                (*all_labels)[i] = ground_truth_label;
            }
            *total_count = result.count;
        } else {
            free(result.predictions);
        }
    } else {
        all_filtered_out->count = 0;
        all_filtered_out->vectors = NULL;
        all_filtered_out->filtered_count = all_features.count;
    }

    free(all_features.vectors);
    return 0;
}

int read_features_from_file(const char *file_path, feature_vector_list *features)
{
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        fprintf(stderr, "错误: 无法打开文件 %s\n", file_path);
        return -1;
    }

    features->capacity = 1000;
    features->vectors = (feature_vector *)malloc(features->capacity * sizeof(feature_vector));
    if (!features->vectors) {
        fprintf(stderr, "错误: 无法分配特征向量内存\n");
        fclose(fp);
        return -1;
    }
    features->count = 0;

    char line[65536];
    int line_num = 0;

    if (fgets(line, sizeof(line), fp) == NULL) {
        fprintf(stderr, "错误: 文件为空\n");
        free(features->vectors);
        features->vectors = NULL;
        fclose(fp);
        return -1;
    }
    line_num++;

    while (fgets(line, sizeof(line), fp) != NULL) {
        line_num++;

        if (features->count >= features->capacity) {
            size_t new_capacity = features->capacity * 2;
            feature_vector *new_vectors =
                (feature_vector *)realloc(features->vectors, new_capacity * sizeof(feature_vector));
            if (!new_vectors) {
                fprintf(stderr, "错误: 扩容特征列表失败\n");
                break;
            }
            features->vectors = new_vectors;
            features->capacity = new_capacity;
        }

        feature_vector *fv = &features->vectors[features->count];

        char *token = strtok(line, ",");
        int col = 0;

        while (token != NULL && col < FEATURES_NUM - 1) {
            fv->features[col] = atof(token);
            token = strtok(NULL, ",");
            col++;
        }

        if (col == FEATURES_NUM - 1) {
            features->count++;
        } else {
            fprintf(stderr, "警告: 第 %d 行特征数量不正确 (期望 %d, 实际 %d)\n", line_num, FEATURES_NUM - 1, col);
        }
    }

    fclose(fp);
    printf("从文件读取了 %u 个特征向量\n", features->count);
    return 0;
}

int inference_mode_file(feature_extractor *extractor, inference_engine *engine, ktag_capture_context *capture,
                        int ground_truth_label,
                        filtered_features *all_filtered_out, int **all_predictions, int **all_labels, int *total_count,
                        PerformanceStats *stats, const char *output_file)
{
    ktag_packet packet{};
    ktag_packet_init(&packet);
    feature_vector_list remaining = {0};
    feature_vector_list loaded_features = {0};
    double extract_time = 0, io_time = 0, pcap_time = 0;

    char temp_file[1024];
    snprintf(temp_file, sizeof(temp_file), "temp.csv");

    file_writer_config writer_config = {
        .file_path = temp_file, .format = FILE_FORMAT_CSV, .mode = WRITE_MODE_FEATURES_ONLY, .append = 0};
    file_writer *writer = writer_create(&writer_config);
    if (!writer) {
        fprintf(stderr, "错误: 无法创建临时文件写入器\n");
        return -1;
    }

    feature_vector_list all_features = {0};
    all_features.capacity = 1000;
    all_features.vectors = (feature_vector *)malloc(all_features.capacity * sizeof(feature_vector));
    if (!all_features.vectors) {
        fprintf(stderr, "错误: 无法分配特征向量内存\n");
        writer_destroy(writer);
        return -1;
    }

    while (1) {
        double t0 = get_time_in_seconds();
        const ktag_status capture_status = ktag_capture_next(capture, &packet);
        double t1 = get_time_in_seconds();

        if (capture_status == KTAG_STATUS_END_OF_STREAM)
            break;
        if (capture_status == KTAG_STATUS_TIMEOUT)
            continue;
        if (capture_status != KTAG_SUCCESS) {
            fprintf(stderr, "Capture read failed: %s\n", ktag_capture_get_last_error());
            break;
        }

        pcap_time += (t1 - t0);
        stats->total_packets++;
        stats->total_bytes += packet.captured_length;

        feature_vector features;
        int has_feature = 0;

        const timeval timestamp = packet_timeval(packet);
        const int extract_status = process_packet(
            extractor, packet.data, packet.captured_length, &timestamp,
            &features, &has_feature);
        ktag_packet_release(&packet);
        if (extract_status ==
            EXTRACTOR_SUCCESS) {
            double t2 = get_time_in_seconds();
            extract_time += (t2 - t1);

            if (has_feature) {
                if (all_features.count >= all_features.capacity) {
                    if (expand_features_capacity(&all_features) != 0) {
                        fprintf(stderr, "错误: 扩容特征列表失败\n");
                        continue;
                    }
                }
                all_features.vectors[all_features.count++] = features;
            }
        }
    }

    remaining.capacity = 100000;
    remaining.vectors = (feature_vector *)malloc(remaining.capacity * sizeof(feature_vector));
    if (!remaining.vectors) {
        fprintf(stderr, "错误: 无法分配剩余流内存\n");
        free(all_features.vectors);
        writer_destroy(writer);
        stats->pcap_read_time += pcap_time;
        stats->feature_extraction_time += extract_time;
        return -1;
    }

    double t1_r = get_time_in_seconds();
    if (extractor_finalize(extractor, &remaining) == EXTRACTOR_SUCCESS) {
        double t2 = get_time_in_seconds();
        extract_time += (t2 - t1_r);

        for (uint32_t i = 0; i < remaining.count; i++) {
            if (all_features.count >= all_features.capacity) {
                if (expand_features_capacity(&all_features) != 0) {
                    fprintf(stderr, "错误: 扩容特征列表失败\n");
                    break;
                }
            }
            all_features.vectors[all_features.count++] = remaining.vectors[i];
        }
    }
    free(remaining.vectors);

    stats->pcap_read_time += pcap_time;
    stats->feature_extraction_time += extract_time;

    double t3 = get_time_in_seconds();
    if (writer_write_features(writer, &all_features) != WRITER_SUCCESS) {
        fprintf(stderr, "错误: 写入特征失败\n");
    }
    writer_flush(writer);
    writer_destroy(writer);
    double t4 = get_time_in_seconds();
    io_time += (t4 - t3);

    printf("特征已写入临时文件: %s (耗时: %.4f 秒)\n", temp_file, t4 - t3);

    free(all_features.vectors);

    double t5 = get_time_in_seconds();
    if (read_features_from_file(temp_file, &loaded_features) != 0) {
        fprintf(stderr, "错误: 从文件读取特征失败\n");
        stats->file_io_time += io_time;
        remove(temp_file);
        return -1;
    }
    double t6 = get_time_in_seconds();
    io_time += (t6 - t5);

    printf("从文件读取特征完成 (耗时: %.4f 秒)\n", t6 - t5);

    filtered_features filtered = {0};
    if (filter_features(engine, &loaded_features, &filtered) == 0 && filtered.count > 0) {
        *all_filtered_out = filtered;

        inference_result result;
        result.predictions = (int *)malloc(filtered.count * sizeof(int));
        if (!result.predictions) {
            fprintf(stderr, "错误: 无法分配推理结果内存\n");
            free(loaded_features.vectors);
            stats->file_io_time += io_time;
            remove(temp_file);
            return -1;
        }
        result.count = filtered.count;

        double t7 = get_time_in_seconds();
        if (inference_predict(engine, &loaded_features, &result) == INFERENCE_SUCCESS) {
            double t8 = get_time_in_seconds();
            stats->inference_time += (t8 - t7);

            *all_predictions = result.predictions;
            *all_labels = (int *)malloc(result.count * sizeof(int));
            if (!*all_labels) {
                fprintf(stderr, "错误: 无法分配标签内存\n");
                free(result.predictions);
                free(loaded_features.vectors);
                stats->file_io_time += io_time;
                remove(temp_file);
                return -1;
            }
            for (uint32_t i = 0; i < result.count; i++) {
                (*all_labels)[i] = ground_truth_label;
            }
            *total_count = result.count;
        } else {
            free(result.predictions);
        }
    } else {
        all_filtered_out->count = 0;
        all_filtered_out->vectors = NULL;
        all_filtered_out->filtered_count = loaded_features.count;
    }

    stats->file_io_time += io_time;
    free(loaded_features.vectors);
    remove(temp_file);

    return 0;
}
