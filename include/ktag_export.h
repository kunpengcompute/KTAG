/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_EXPORT_H
#define KTAG_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef KTAG_BUILDING_DLL
#    define KTAG_API __declspec(dllexport)
#  else
#    define KTAG_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#  define KTAG_API __attribute__((visibility("default")))
#else
#  define KTAG_API
#endif

/* Legacy fixed-vector compatibility APIs remain available until explicitly removed. */
#if defined(KTAG_SUPPRESS_DEPRECATED_WARNINGS)
#  define KTAG_DEPRECATED(message)
#elif defined(_MSC_VER)
#  define KTAG_DEPRECATED(message) __declspec(deprecated(message))
#elif defined(__GNUC__) || defined(__clang__)
#  define KTAG_DEPRECATED(message) __attribute__((deprecated(message)))
#else
#  define KTAG_DEPRECATED(message)
#endif

#endif /* KTAG_EXPORT_H */
