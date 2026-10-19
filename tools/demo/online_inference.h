/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#include "ktag_file_writer.h"
#include "ktag_legacy_inference.h"
#include "ktag_type.h"
#include "utils.h"

int expand_features_capacity(feature_vector_list *list);
// 在线推理主函数
int online_inference_mode(const char *config_file, const char **pcap_files,
                          int num_pcap_files, const char *interface, int mode,
                          char *output_file, file_format format);
