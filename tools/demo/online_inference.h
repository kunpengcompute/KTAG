/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */
#ifndef KTAG_DEMO_ONLINE_INFERENCE_H
#define KTAG_DEMO_ONLINE_INFERENCE_H
#include "ktag_file_writer.h"

// 在线推理主函数
int online_inference_mode(const char *config_file, const char **pcap_files,
                          int num_pcap_files, const char *interface, int mode,
                          char *output_file, file_format format);
#endif
