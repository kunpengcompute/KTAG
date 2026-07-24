/*
 * Minimal customer integration using only the three public KTAG layers.
 *
 * Usage:
 *   ./ktag_three_layer_example MODEL_PACKAGE INPUT.pcap
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ktag_capture.h"
#include "ktag_feature.h"
#include "ktag_inference.h"

static int print_predictions(
    ktag_inference_context *inference,
    const ktag_feature_batch *features,
    ktag_prediction_batch *predictions,
    uint64_t *prediction_count)
{
    uint32_t index;
    ktag_status status = ktag_inference_predict(
        inference, features, predictions);
    if (status != KTAG_SUCCESS) {
        fprintf(stderr, "Inference failed: %s\n",
                ktag_inference_get_last_error());
        return 0;
    }

    for (index = 0U;
         index < ktag_prediction_batch_get_count(predictions);
         ++index) {
        ktag_prediction_record record = {0};
        status = ktag_prediction_batch_get_record(
            predictions, index, &record);
        if (status != KTAG_SUCCESS) {
            fprintf(stderr, "Could not read prediction %u\n", index);
            return 0;
        }
        printf("flow=%" PRIu64 " label=%" PRId64 " confidence=%.6f\n",
               record.metadata.flow_id,
               record.label,
               (double)record.confidence);
        ++(*prediction_count);
    }
    return 1;
}

int main(int argc, char **argv)
{
    int result = EXIT_FAILURE;
    uint64_t packet_count = 0U;
    uint64_t prediction_count = 0U;
    ktag_feature_schema *schema = NULL;
    ktag_feature_registry *registry = NULL;
    ktag_domain_store *domain_store = NULL;
    ktag_feature_context *feature = NULL;
    ktag_inference_context *inference = NULL;
    ktag_capture_context *capture = NULL;
    ktag_feature_batch *features = NULL;
    ktag_prediction_batch *predictions = NULL;
    ktag_packet packet = {0};

    if (argc != 3) {
        fprintf(stderr, "Usage: %s MODEL_PACKAGE INPUT.pcap\n", argv[0]);
        return EXIT_FAILURE;
    }

    schema = ktag_feature_schema_create_legacy_v1();
    registry = ktag_feature_registry_create();
    if (schema == NULL || registry == NULL ||
        ktag_feature_registry_register_legacy_builtins(registry) !=
            KTAG_SUCCESS) {
        fprintf(stderr, "Could not create the Legacy V1 feature contract\n");
        goto cleanup;
    }

    {
        ktag_domain_store_config config = {0};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.max_entries = KTAG_DOMAIN_DEFAULT_MAX_ENTRIES;
        config.journal_capacity = KTAG_DOMAIN_DEFAULT_JOURNAL_CAPACITY;
        domain_store = ktag_domain_store_create(&config);
    }
    if (domain_store == NULL) {
        fprintf(stderr, "Could not create DomainStore\n");
        goto cleanup;
    }

    {
        ktag_feature_config config = {0};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.schema = schema;
        config.registry = registry;
        config.shared_domain_store = domain_store;
        config.udp_packet_limit = KTAG_FEATURE_DEFAULT_UDP_PACKET_LIMIT;
        config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
        feature = ktag_feature_create(&config);
    }
    if (feature == NULL) {
        fprintf(stderr, "Could not create Feature Context\n");
        goto cleanup;
    }

    {
        ktag_inference_config config = {0};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.model_package_path = argv[1];
        config.feature_schema = schema;
        config.intra_op_threads = 1U;
        config.inter_op_threads = 1U;
        inference = ktag_inference_create(&config);
    }
    if (inference == NULL) {
        fprintf(stderr, "Could not create Inference Context: %s\n",
                ktag_inference_get_last_error());
        goto cleanup;
    }

    {
        ktag_capture_config config = {0};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.source_type = KTAG_CAPTURE_SOURCE_PCAP_FILE;
        config.source = argv[2];
        config.packet_mode = KTAG_CAPTURE_PACKET_BORROWED;
        capture = ktag_capture_open(&config);
    }
    if (capture == NULL) {
        fprintf(stderr, "Could not open capture: %s\n",
                ktag_capture_get_last_error());
        goto cleanup;
    }

    features = ktag_feature_batch_create(schema, 1U);
    predictions = ktag_prediction_batch_create(1U);
    if (features == NULL || predictions == NULL) {
        fprintf(stderr, "Could not allocate output batches\n");
        goto cleanup;
    }

    ktag_packet_init(&packet);
    for (;;) {
        ktag_status status = ktag_capture_next(capture, &packet);
        if (status == KTAG_STATUS_END_OF_STREAM) {
            break;
        }
        if (status == KTAG_STATUS_TIMEOUT) {
            continue;
        }
        if (status != KTAG_SUCCESS) {
            fprintf(stderr, "Capture failed: %s\n",
                    ktag_capture_get_last_error());
            goto cleanup;
        }
        ++packet_count;

        status = ktag_feature_process_packet(feature, &packet, features);
        if (status != KTAG_SUCCESS) {
            fprintf(stderr, "Feature extraction failed (%d)\n", status);
            goto cleanup;
        }
        if (ktag_feature_batch_get_count(features) != 0U &&
            !print_predictions(
                inference, features, predictions, &prediction_count)) {
            goto cleanup;
        }
    }

    {
        ktag_feature_statistics statistics = {0};
        statistics.struct_size = sizeof(statistics);
        statistics.api_version = KTAG_API_VERSION;
        if (ktag_feature_get_statistics(feature, &statistics) !=
                KTAG_SUCCESS ||
            ktag_feature_batch_reserve(features, statistics.active_flows) !=
                KTAG_SUCCESS ||
            ktag_prediction_batch_reserve(
                predictions, statistics.active_flows) != KTAG_SUCCESS) {
            fprintf(stderr, "Could not reserve final flush capacity\n");
            goto cleanup;
        }
    }

    if (ktag_feature_flush(feature, features) != KTAG_SUCCESS ||
        !print_predictions(
            inference, features, predictions, &prediction_count)) {
        fprintf(stderr, "Could not flush remaining flows\n");
        goto cleanup;
    }

    printf("complete: packets=%" PRIu64 " predictions=%" PRIu64 "\n",
           packet_count, prediction_count);
    result = EXIT_SUCCESS;

cleanup:
    ktag_packet_release(&packet);
    ktag_prediction_batch_destroy(predictions);
    ktag_feature_batch_destroy(features);
    ktag_capture_close(capture);
    ktag_inference_destroy(inference);
    ktag_feature_destroy(feature);
    ktag_domain_store_destroy(domain_store);
    ktag_feature_registry_destroy(registry);
    ktag_feature_schema_destroy(schema);
    return result;
}
