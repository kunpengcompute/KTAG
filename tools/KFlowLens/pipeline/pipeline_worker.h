/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#ifndef KTAG_DEMO_PIPELINE_WORKER_H
#define KTAG_DEMO_PIPELINE_WORKER_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if KTAG_ENABLE_DPDK_UB
#include "ub_ring_transport.h"
#endif

inline void ktag_demo_name_worker(const char *stage, uint32_t index) {
#ifdef __linux__
  const auto name = std::string("ktag_") + stage + "_" + std::to_string(index);
  (void)pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

inline bool ktag_demo_start_worker(const char *stage, uint32_t index,
    const std::vector<uint32_t> &cpus, const std::vector<uint32_t> &allowed,
    std::string *error) {
  ktag_demo_name_worker(stage, index);
  if (!cpus.empty()) {
    if (index >= cpus.size() || (!allowed.empty() &&
        std::find(allowed.begin(), allowed.end(), cpus[index]) == allowed.end())) {
      *error = std::string(stage) + " worker=" + std::to_string(index) +
          " CPU is outside the pre-EAL allowed CPU set";
      return false;
    }
#if KTAG_ENABLE_DPDK_UB
    if (!ktag_demo_ub_bind_cpu(cpus[index], error)) { return false; }
#else
    *error = "worker CPU binding requires the UB build";
    return false;
#endif
  }
#ifdef __linux__
  const auto tid = static_cast<unsigned long long>(syscall(SYS_gettid));
#else
  const auto tid = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
  if (cpus.empty()) {
    std::printf("KTAG worker: stage=%s, worker=%u, tid=%llu, cpu=inherited\n", stage, index, tid);
  } else {
    std::printf("KTAG worker: stage=%s, worker=%u, tid=%llu, cpu=%u\n", stage, index, tid, cpus[index]);
  }
  return true;
}
#endif
