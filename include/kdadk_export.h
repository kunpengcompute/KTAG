/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
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
