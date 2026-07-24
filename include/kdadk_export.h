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

/* Legacy KDADK compatibility APIs remain available until explicitly removed. */
#if defined(KTAG_SUPPRESS_DEPRECATED_WARNINGS)
#  define KDADK_DEPRECATED(message)
#elif defined(_MSC_VER)
#  define KDADK_DEPRECATED(message) __declspec(deprecated(message))
#elif defined(__GNUC__) || defined(__clang__)
#  define KDADK_DEPRECATED(message) __attribute__((deprecated(message)))
#else
#  define KDADK_DEPRECATED(message)
#endif

#endif /* KDADK_EXPORT_H */
