/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_INFERENCE_H
#define KTAG_INFERENCE_H

#include <stdint.h>

#include "ktag_export.h"
#include "ktag_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * confidence in [0, 1] is a model-provided value; zero is valid and must not
 * suppress or rewrite a label.  -1 means that the model has no usable
 * probability output while the numeric label remains valid.
 */
#define KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE (-1.0F)

typedef struct ktag_inference_context ktag_inference_context;
typedef struct ktag_prediction_batch ktag_prediction_batch;
typedef struct ktag_feature_schema ktag_feature_schema;
typedef struct ktag_feature_batch ktag_feature_batch;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  const char *model_package_path;
  const ktag_feature_schema *feature_schema;
  uint32_t intra_op_threads;
  uint32_t inter_op_threads;
} ktag_inference_config;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t schema_hash;
  uint32_t input_feature_count;
  uint32_t input_value_count;
  uint64_t minimum_total_packets;
} ktag_model_package_validation;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t flow_id;
  ktag_flow_key flow_key;
  uint64_t first_packet_sequence;
  uint64_t last_packet_sequence;
} ktag_prediction_metadata;

typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  ktag_prediction_metadata metadata;
  uint32_t source_record_index;
  int64_t label;
  /* See KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE; no unknown threshold applies. */
  float confidence;
} ktag_prediction_record;

/*
 * Loads manifest.json and scaler metadata, resolves every model input by
 * feature ID, and verifies both schema hashes without creating an ONNX
 * session. This is useful as a deployment/package preflight check.
 */
KTAG_API ktag_status ktag_inference_validate_model_package(
    const char *model_package_path, const ktag_feature_schema *feature_schema,
    ktag_model_package_validation *validation);

/*
 * The Schema is borrowed only during create. Its descriptors are resolved
 * into an immutable input plan owned by the returned Context.
 */
KTAG_API ktag_inference_context *
ktag_inference_create(const ktag_inference_config *config);

/*
 * A Context becomes single-thread owned on its first predict call. Input
 * records rejected by manifest preprocessing do not produce predictions.
 */
KTAG_API ktag_status ktag_inference_predict(ktag_inference_context *context,
                                            const ktag_feature_batch *features,
                                            ktag_prediction_batch *predictions);

/* Optional diagnostic timing. Only synchronous ONNX Session::Run is timed;
 * preprocessing, tensor construction and result conversion are excluded.
 * Zero model_record_count means preprocessing skipped all records. */
typedef struct {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t input_record_count;
  uint32_t model_record_count;
  uint64_t model_nanoseconds;
} ktag_inference_timing;

KTAG_API ktag_status ktag_inference_predict_with_timing(
    ktag_inference_context *context, const ktag_feature_batch *features,
    ktag_prediction_batch *predictions, ktag_inference_timing *timing);


KTAG_API uint64_t
ktag_inference_get_schema_hash(const ktag_inference_context *context);

KTAG_API uint32_t
ktag_inference_get_input_value_count(const ktag_inference_context *context);

/* Borrowed until Context destruction. */
KTAG_API const char *
ktag_inference_get_model_name(const ktag_inference_context *context);

KTAG_API void ktag_inference_destroy(ktag_inference_context *context);

/* Thread-local detail for the most recent new Inference API failure. */
KTAG_API const char *ktag_inference_get_last_error(void);

KTAG_API ktag_prediction_batch *
ktag_prediction_batch_create(uint32_t initial_capacity);

KTAG_API void ktag_prediction_batch_destroy(ktag_prediction_batch *batch);

KTAG_API ktag_status ktag_prediction_batch_reserve(ktag_prediction_batch *batch,
                                                   uint32_t capacity);

KTAG_API void ktag_prediction_batch_clear(ktag_prediction_batch *batch);

KTAG_API uint32_t
ktag_prediction_batch_get_count(const ktag_prediction_batch *batch);

KTAG_API ktag_status ktag_prediction_batch_get_record(
    const ktag_prediction_batch *batch, uint32_t index,
    ktag_prediction_record *record);

#ifdef __cplusplus
}
#endif

#endif /* KTAG_INFERENCE_H */
