/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * This translation unit intentionally uses the C compiler. Its presence in
 * the functional-test source set verifies that the new public headers remain
 * valid C ABI headers.
 */
#include "ktag_feature.h"
#include "ktag_feature_plugin.h"
#include "ktag_inference.h"
#include "ktag_capture.h"
#include "ktag_types.h"
#include "ktag_legacy_types.h"
#include "ktag_legacy_inference.h"

_Static_assert(sizeof(feature_vector) == 355U * sizeof(float), "Retained inference vector layout changed");

void ktag_public_headers_c_compatibility_check(void)
{
    ktag_feature_descriptor descriptor = {0};
    ktag_feature_plugin_descriptor plugin = {0};
    ktag_feature_record_metadata metadata = {0};
    ktag_feature_record_details record_details = {0};
    ktag_capture_config capture = {0};
    ktag_packet packet = {0};
    ktag_domain_store_config domain_store = {0};
    ktag_domain_cache_config domain_cache = {0};
    ktag_feature_config feature_config = {0};
    ktag_feature_statistics feature_statistics = {0};
    ktag_packet_transport_view transport = {0};
    ktag_inference_config inference_config = {0};
    ktag_model_package_validation model_validation = {0};
    ktag_prediction_metadata prediction_metadata = {0};
    ktag_prediction_record prediction = {0};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.api_version = KTAG_API_VERSION;
    plugin.struct_size = sizeof(plugin);
    plugin.api_version = KTAG_API_VERSION;
    plugin.plugin_abi_version = KTAG_FEATURE_PLUGIN_ABI_VERSION;
    metadata.struct_size = sizeof(metadata);
    metadata.api_version = KTAG_API_VERSION;
    record_details.struct_size = sizeof(record_details);
    record_details.api_version = KTAG_API_VERSION;
    capture.struct_size = sizeof(capture);
    capture.api_version = KTAG_API_VERSION;
    ktag_packet_init(&packet);
    domain_store.struct_size = sizeof(domain_store);
    domain_store.api_version = KTAG_API_VERSION;
    domain_cache.struct_size = sizeof(domain_cache);
    domain_cache.api_version = KTAG_API_VERSION;
    feature_config.struct_size = sizeof(feature_config);
    feature_config.api_version = KTAG_API_VERSION;
    feature_statistics.struct_size = sizeof(feature_statistics);
    feature_statistics.api_version = KTAG_API_VERSION;
    transport.struct_size = sizeof(transport);
    transport.api_version = KTAG_API_VERSION;
    inference_config.struct_size = sizeof(inference_config);
    inference_config.api_version = KTAG_API_VERSION;
    model_validation.struct_size = sizeof(model_validation);
    model_validation.api_version = KTAG_API_VERSION;
    prediction_metadata.struct_size = sizeof(prediction_metadata);
    prediction_metadata.api_version = KTAG_API_VERSION;
    prediction.struct_size = sizeof(prediction);
    prediction.api_version = KTAG_API_VERSION;
    (void)descriptor;
    (void)plugin;
    (void)metadata;
    (void)record_details;
    (void)capture;
    (void)packet;
    (void)domain_store;
    (void)domain_cache;
    (void)feature_config;
    (void)feature_statistics;
    (void)transport;
    (void)inference_config;
    (void)model_validation;
    (void)prediction_metadata;
    (void)prediction;
}
