/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_TRAFFIC_FEATURE_CATALOG_H
#define KTAG_TRAFFIC_FEATURE_CATALOG_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace ktag_traffic_features {

#if defined(__GNUC__) && (__GNUC__ >= 4)
#define KTAG_TRAFFIC_CATALOG_INTERNAL __attribute__((visibility("hidden")))
#else
#define KTAG_TRAFFIC_CATALOG_INTERNAL
#endif

inline constexpr uint32_t KTAG_LEGACY_HISTOGRAM_DIMENSION = 16U;
inline constexpr uint32_t KTAG_LEGACY_RAWBOW_DIMENSION = 32U;

struct feature_definition {
  const char *feature_id;
  const char *display_name;
  uint32_t dimension;
};

struct provider_catalog {
  const char *provider_id;
  uint32_t provider_version;
  const feature_definition *features;
  size_t feature_count;
};

/* Contract data is defined once in traffic_feature_catalog.cpp. Provider
 * implementations consume only the slot they own; the aggregate list exists
 * solely for frozen Legacy V1 ordering and transactional registration. */
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 1U>
    KTAG_METADATA_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 5U>
    KTAG_TUPLE_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 2U>
    KTAG_SEND_COUNTERS_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 7U>
    KTAG_SEND_HEADER_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 7U>
    KTAG_SEND_PAYLOAD_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 7U>
    KTAG_SEND_INTERVAL_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 2U>
    KTAG_RECEIVE_COUNTERS_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 7U>
    KTAG_RECEIVE_HEADER_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 7U>
    KTAG_RECEIVE_PAYLOAD_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 7U>
    KTAG_RECEIVE_INTERVAL_FEATURES;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<feature_definition, 1U>
    KTAG_RAWBOW_FEATURES;

extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_METADATA_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_TUPLE_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_SEND_COUNTERS_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_SEND_HEADER_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_SEND_PAYLOAD_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_SEND_INTERVAL_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_RECEIVE_COUNTERS_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_RECEIVE_HEADER_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_RECEIVE_PAYLOAD_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_RECEIVE_INTERVAL_CATALOG;
extern KTAG_TRAFFIC_CATALOG_INTERNAL const provider_catalog KTAG_RAWBOW_CATALOG;

extern KTAG_TRAFFIC_CATALOG_INTERNAL const std::array<const provider_catalog *, 11U>
    KTAG_TRAFFIC_PROVIDER_CATALOG;

#undef KTAG_TRAFFIC_CATALOG_INTERNAL

} // namespace ktag_traffic_features

#endif /* KTAG_TRAFFIC_FEATURE_CATALOG_H */
