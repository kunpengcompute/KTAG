/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_TRAFFIC_FEATURES_H
#define KTAG_TRAFFIC_FEATURES_H

#include "ktag_feature.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef KTAG_TRAFFIC_FEATURES_BUILDING_DLL
#    define KTAG_TRAFFIC_FEATURES_API __declspec(dllexport)
#  else
#    define KTAG_TRAFFIC_FEATURES_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#  define KTAG_TRAFFIC_FEATURES_API __attribute__((visibility("default")))
#else
#  define KTAG_TRAFFIC_FEATURES_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Direct registration for statically linked hosts and functional tests.
 * Dynamic hosts should load the plugin through ktag_feature_plugin_query. */
KTAG_TRAFFIC_FEATURES_API ktag_status ktag_traffic_features_register(
    ktag_feature_registry *registry);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_TRAFFIC_FEATURES_H */
