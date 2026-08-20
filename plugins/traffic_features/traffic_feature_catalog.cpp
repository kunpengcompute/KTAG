/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "traffic_feature_catalog.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ktag_traffic_features {

/*
 * A Provider is a replaceable feature-family slot, not a wrapper around the
 * complete historical vector. The catalog order is also the frozen Legacy V1
 * CSV order; changing it requires a new schema version.
 */
const char KTAG_METADATA_PROVIDER_ID[] =
    "builtin.legacy.metadata";
const uint32_t KTAG_METADATA_PROVIDER_VERSION = 1U;
const char KTAG_TUPLE_PROVIDER_ID[] = "builtin.legacy.tuple";
const uint32_t KTAG_TUPLE_PROVIDER_VERSION = 1U;
const char KTAG_SEND_COUNTERS_PROVIDER_ID[] =
    "builtin.legacy.send.counters";
const uint32_t KTAG_SEND_COUNTERS_PROVIDER_VERSION = 1U;
const char KTAG_SEND_HEADER_PROVIDER_ID[] =
    "builtin.legacy.send.header";
const uint32_t KTAG_SEND_HEADER_PROVIDER_VERSION = 1U;
const char KTAG_SEND_PAYLOAD_PROVIDER_ID[] =
    "builtin.legacy.send.payload";
const uint32_t KTAG_SEND_PAYLOAD_PROVIDER_VERSION = 1U;
const char KTAG_SEND_INTERVAL_PROVIDER_ID[] =
    "builtin.legacy.send.interval";
const uint32_t KTAG_SEND_INTERVAL_PROVIDER_VERSION = 1U;
const char KTAG_RECEIVE_COUNTERS_PROVIDER_ID[] =
    "builtin.legacy.receive.counters";
const uint32_t KTAG_RECEIVE_COUNTERS_PROVIDER_VERSION = 1U;
const char KTAG_RECEIVE_HEADER_PROVIDER_ID[] =
    "builtin.legacy.receive.header";
const uint32_t KTAG_RECEIVE_HEADER_PROVIDER_VERSION = 1U;
const char KTAG_RECEIVE_PAYLOAD_PROVIDER_ID[] =
    "builtin.legacy.receive.payload";
const uint32_t KTAG_RECEIVE_PAYLOAD_PROVIDER_VERSION = 1U;
const char KTAG_RECEIVE_INTERVAL_PROVIDER_ID[] =
    "builtin.legacy.receive.interval";
const uint32_t KTAG_RECEIVE_INTERVAL_PROVIDER_VERSION = 1U;
const char KTAG_RAWBOW_PROVIDER_ID[] = "builtin.legacy.rawbow";
const uint32_t KTAG_RAWBOW_PROVIDER_VERSION = 1U;

const std::array<feature_definition, 1U> KTAG_METADATA_FEATURES{{
    {"legacy.flow_id", "flow_id", 1U},
}};

const std::array<feature_definition, 5U> KTAG_TUPLE_FEATURES{{
    {"network.src_ip", "src_ip", 1U},
    {"network.dst_ip", "dst_ip", 1U},
    {"network.src_port", "src_port", 1U},
    {"network.dst_port", "dst_port", 1U},
    {"network.protocol", "protocol", 1U},
}};

/*
 * Keep every slot's contract explicit.  Apart from being easier to review,
 * this avoids compiler-dependent aggregate initialization through macros.
 * The entry order below is the historical 354-value vector order.
 */
const std::array<feature_definition, 2U>
    KTAG_SEND_COUNTERS_FEATURES{{
        {"traffic.send.byte_count", "send_byte_nums", 1U},
        {"traffic.send.packet_count", "send_packet_nums", 1U},
    }};

const std::array<feature_definition, 7U>
    KTAG_SEND_HEADER_FEATURES{{
        {"traffic.send.header.minimum", "send_hdr_min", 1U},
        {"traffic.send.header.maximum", "send_hdr_max", 1U},
        {"traffic.send.header.mean", "send_hdr_mean", 1U},
        {"traffic.send.header.variance", "send_hdr_var", 1U},
        {"traffic.send.header.histogram.relative", "send_hdr_hist_rel",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.send.header.histogram.absolute", "send_hdr_hist_abs",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.send.header.histogram.normalized", "send_hdr_hist_nor",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
    }};

const std::array<feature_definition, 7U>
    KTAG_SEND_PAYLOAD_FEATURES{{
        {"traffic.send.payload.minimum", "send_pld_min", 1U},
        {"traffic.send.payload.maximum", "send_pld_max", 1U},
        {"traffic.send.payload.mean", "send_pld_mean", 1U},
        {"traffic.send.payload.variance", "send_pld_var", 1U},
        {"traffic.send.payload.histogram.relative", "send_pld_hist_rel",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.send.payload.histogram.absolute", "send_pld_hist_abs",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.send.payload.histogram.normalized", "send_pld_hist_nor",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
    }};

const std::array<feature_definition, 7U>
    KTAG_SEND_INTERVAL_FEATURES{{
        {"traffic.send.interval.minimum", "send_interval_min", 1U},
        {"traffic.send.interval.maximum", "send_interval_max", 1U},
        {"traffic.send.interval.mean", "send_interval_mean", 1U},
        {"traffic.send.interval.variance", "send_interval_var", 1U},
        {"traffic.send.interval.histogram.relative", "send_interval_hist_rel",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.send.interval.histogram.absolute", "send_interval_hist_abs",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.send.interval.histogram.normalized", "send_interval_hist_nor",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
    }};

const std::array<feature_definition, 2U>
    KTAG_RECEIVE_COUNTERS_FEATURES{{
        {"traffic.receive.byte_count", "receive_byte_nums", 1U},
        {"traffic.receive.packet_count", "receive_packet_nums", 1U},
    }};

const std::array<feature_definition, 7U>
    KTAG_RECEIVE_HEADER_FEATURES{{
        {"traffic.receive.header.minimum", "receive_hdr_min", 1U},
        {"traffic.receive.header.maximum", "receive_hdr_max", 1U},
        {"traffic.receive.header.mean", "receive_hdr_mean", 1U},
        {"traffic.receive.header.variance", "receive_hdr_var", 1U},
        {"traffic.receive.header.histogram.relative", "receive_hdr_hist_rel",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.receive.header.histogram.absolute", "receive_hdr_hist_abs",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.receive.header.histogram.normalized", "receive_hdr_hist_nor",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
    }};

const std::array<feature_definition, 7U>
    KTAG_RECEIVE_PAYLOAD_FEATURES{{
        {"traffic.receive.payload.minimum", "receive_pld_min", 1U},
        {"traffic.receive.payload.maximum", "receive_pld_max", 1U},
        {"traffic.receive.payload.mean", "receive_pld_mean", 1U},
        {"traffic.receive.payload.variance", "receive_pld_var", 1U},
        {"traffic.receive.payload.histogram.relative", "receive_pld_hist_rel",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.receive.payload.histogram.absolute", "receive_pld_hist_abs",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.receive.payload.histogram.normalized", "receive_pld_hist_nor",
         KTAG_LEGACY_HISTOGRAM_DIMENSION},
    }};

const std::array<feature_definition, 7U>
    KTAG_RECEIVE_INTERVAL_FEATURES{{
        {"traffic.receive.interval.minimum", "receive_interval_min", 1U},
        {"traffic.receive.interval.maximum", "receive_interval_max", 1U},
        {"traffic.receive.interval.mean", "receive_interval_mean", 1U},
        {"traffic.receive.interval.variance", "receive_interval_var", 1U},
        {"traffic.receive.interval.histogram.relative",
         "receive_interval_hist_rel", KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.receive.interval.histogram.absolute",
         "receive_interval_hist_abs", KTAG_LEGACY_HISTOGRAM_DIMENSION},
        {"traffic.receive.interval.histogram.normalized",
         "receive_interval_hist_nor", KTAG_LEGACY_HISTOGRAM_DIMENSION},
    }};

const std::array<feature_definition, 1U> KTAG_RAWBOW_FEATURES{{
    {"rawbow.crc_histogram", "raw_bow_crc_hist",
     KTAG_LEGACY_RAWBOW_DIMENSION},
}};

template <size_t FeatureCount>
constexpr provider_catalog make_catalog(
    const char *provider_id,
    uint32_t provider_version,
    const std::array<feature_definition, FeatureCount> &features) {
  return {provider_id, provider_version, features.data(), features.size()};
}

const provider_catalog KTAG_METADATA_CATALOG =
    make_catalog(KTAG_METADATA_PROVIDER_ID, KTAG_METADATA_PROVIDER_VERSION,
                 KTAG_METADATA_FEATURES);
const provider_catalog KTAG_TUPLE_CATALOG =
    make_catalog(KTAG_TUPLE_PROVIDER_ID, KTAG_TUPLE_PROVIDER_VERSION,
                 KTAG_TUPLE_FEATURES);
const provider_catalog KTAG_SEND_COUNTERS_CATALOG =
    make_catalog(KTAG_SEND_COUNTERS_PROVIDER_ID,
                 KTAG_SEND_COUNTERS_PROVIDER_VERSION,
                 KTAG_SEND_COUNTERS_FEATURES);
const provider_catalog KTAG_SEND_HEADER_CATALOG =
    make_catalog(KTAG_SEND_HEADER_PROVIDER_ID,
                 KTAG_SEND_HEADER_PROVIDER_VERSION,
                 KTAG_SEND_HEADER_FEATURES);
const provider_catalog KTAG_SEND_PAYLOAD_CATALOG =
    make_catalog(KTAG_SEND_PAYLOAD_PROVIDER_ID,
                 KTAG_SEND_PAYLOAD_PROVIDER_VERSION,
                 KTAG_SEND_PAYLOAD_FEATURES);
const provider_catalog KTAG_SEND_INTERVAL_CATALOG =
    make_catalog(KTAG_SEND_INTERVAL_PROVIDER_ID,
                 KTAG_SEND_INTERVAL_PROVIDER_VERSION,
                 KTAG_SEND_INTERVAL_FEATURES);
const provider_catalog KTAG_RECEIVE_COUNTERS_CATALOG = make_catalog(
    KTAG_RECEIVE_COUNTERS_PROVIDER_ID,
    KTAG_RECEIVE_COUNTERS_PROVIDER_VERSION,
    KTAG_RECEIVE_COUNTERS_FEATURES);
const provider_catalog KTAG_RECEIVE_HEADER_CATALOG = make_catalog(
    KTAG_RECEIVE_HEADER_PROVIDER_ID, KTAG_RECEIVE_HEADER_PROVIDER_VERSION,
    KTAG_RECEIVE_HEADER_FEATURES);
const provider_catalog KTAG_RECEIVE_PAYLOAD_CATALOG = make_catalog(
    KTAG_RECEIVE_PAYLOAD_PROVIDER_ID, KTAG_RECEIVE_PAYLOAD_PROVIDER_VERSION,
    KTAG_RECEIVE_PAYLOAD_FEATURES);
const provider_catalog KTAG_RECEIVE_INTERVAL_CATALOG = make_catalog(
    KTAG_RECEIVE_INTERVAL_PROVIDER_ID, KTAG_RECEIVE_INTERVAL_PROVIDER_VERSION,
    KTAG_RECEIVE_INTERVAL_FEATURES);
const provider_catalog KTAG_RAWBOW_CATALOG =
    make_catalog(KTAG_RAWBOW_PROVIDER_ID, KTAG_RAWBOW_PROVIDER_VERSION,
                 KTAG_RAWBOW_FEATURES);

const std::array<const provider_catalog *, 11U>
    KTAG_TRAFFIC_PROVIDER_CATALOG{{
        &KTAG_METADATA_CATALOG,
        &KTAG_TUPLE_CATALOG,
        &KTAG_SEND_COUNTERS_CATALOG,
        &KTAG_SEND_HEADER_CATALOG,
        &KTAG_SEND_PAYLOAD_CATALOG,
        &KTAG_SEND_INTERVAL_CATALOG,
        &KTAG_RECEIVE_COUNTERS_CATALOG,
        &KTAG_RECEIVE_HEADER_CATALOG,
        &KTAG_RECEIVE_PAYLOAD_CATALOG,
        &KTAG_RECEIVE_INTERVAL_CATALOG,
        &KTAG_RAWBOW_CATALOG,
    }};

constexpr uint32_t TRAFFIC_FEATURE_DESCRIPTOR_COUNT =
    1U + 5U + 2U + 7U + 7U + 7U + 2U + 7U + 7U + 7U + 1U;
constexpr uint32_t TRAFFIC_FEATURE_VALUE_COUNT =
    1U + 5U + 2U + (6U * (4U + 3U * KTAG_LEGACY_HISTOGRAM_DIMENSION)) +
    2U + KTAG_LEGACY_RAWBOW_DIMENSION;

static_assert(TRAFFIC_FEATURE_DESCRIPTOR_COUNT == 53U,
              "Legacy V1 must expose exactly 53 feature descriptors");
static_assert(TRAFFIC_FEATURE_VALUE_COUNT == 354U,
              "Legacy V1 must expose exactly 354 values");

} // namespace ktag_traffic_features
