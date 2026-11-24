/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Megascan is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 */
#ifndef KDADK_EXPORT_H
#define KDADK_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef KDADK_BUILDING_DLL
#    define KDADK_API __declspec(dllexport)
#  else
#    define KDADK_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#  define KDADK_API __attribute__((visibility("default")))
#else
#  define KDADK_API
#endif

#endif /* KDADK_EXPORT_H */
