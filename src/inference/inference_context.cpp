/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "ktag_feature.h"
#include "ktag_inference.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "model_contract_internal.h"
#include "prediction_batch_internal.h"

namespace {

thread_local std::string inference_last_error;

void set_last_error(const std::string &message) {
  inference_last_error = message;
}

size_t current_thread_token() {
  static std::atomic<size_t> next_token{1U};
  thread_local const size_t token =
      next_token.fetch_add(1U, std::memory_order_relaxed);
  return token == 0U ? std::numeric_limits<size_t>::max() : token;
}

OrtLoggingLevel parse_log_level(const std::string &level) {
  if (level == "VERBOSE") {
    return ORT_LOGGING_LEVEL_VERBOSE;
  }
  if (level == "INFO") {
    return ORT_LOGGING_LEVEL_INFO;
  }
  if (level == "ERROR") {
    return ORT_LOGGING_LEVEL_ERROR;
  }
  if (level == "FATAL") {
    return ORT_LOGGING_LEVEL_FATAL;
  }
  return ORT_LOGGING_LEVEL_WARNING;
}

bool valid_config(const ktag_inference_config *config) {
  return config != nullptr && config->struct_size >= sizeof(*config) &&
         config->api_version == KTAG_API_VERSION &&
         config->model_package_path != nullptr &&
         config->model_package_path[0] != '\0' &&
         config->feature_schema != nullptr &&
         ktag_feature_schema_is_frozen(config->feature_schema) != 0U &&
         config->intra_op_threads <=
             static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
         config->inter_op_threads <=
             static_cast<uint32_t>(std::numeric_limits<int>::max());
}

ktag_status validate_model_input(Ort::Session *session,
                                 uint32_t input_value_count,
                                 std::string *error) {
  if (session->GetInputCount() != 1U) {
    *error = "the first inference API supports exactly one ONNX input";
    return KTAG_ERROR_NOT_SUPPORTED;
  }
  /*
   * TensorTypeAndShapeInfo borrows its OrtTensorTypeAndShapeInfo from the
   * enclosing TypeInfo.  Keep TypeInfo alive until all input metadata has
   * been consumed instead of obtaining the tensor view from a temporary.
   */
  const Ort::TypeInfo input_type = session->GetInputTypeInfo(0U);
  const auto tensor = input_type.GetTensorTypeAndShapeInfo();
  const ONNXTensorElementDataType element_type = tensor.GetElementType();
  if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    *error = "ONNX model input must use float32 values (actual element "
             "type enum=" +
             std::to_string(static_cast<int>(element_type)) + ")";
    return KTAG_ERROR_SCHEMA_MISMATCH;
  }
  const std::vector<int64_t> shape = tensor.GetShape();
  if (shape.size() != 2U) {
    *error = "ONNX model input must be a rank-2 [batch, values] tensor";
    return KTAG_ERROR_NOT_SUPPORTED;
  }
  const int64_t declared_width = shape.back();
  if (declared_width <= 0) {
    *error = "ONNX model input must declare a static, non-zero value dimension";
    return KTAG_ERROR_SCHEMA_MISMATCH;
  }
  if (static_cast<uint64_t>(declared_width) != input_value_count) {
    *error = "ONNX input width does not match manifest/scaler";
    return KTAG_ERROR_SCHEMA_MISMATCH;
  }
  return KTAG_SUCCESS;
}

ktag_status extract_labels(const std::vector<Ort::Value> &outputs,
                           size_t expected_count, std::vector<int64_t> *labels,
                           std::string *error) {
  if (outputs.empty() || !outputs[0].IsTensor()) {
    *error = "ONNX first output must be a label tensor";
    return KTAG_ERROR_NOT_SUPPORTED;
  }
  const auto tensor = outputs[0].GetTensorTypeAndShapeInfo();
  const size_t count = tensor.GetElementCount();
  if (count != expected_count) {
    *error = "ONNX label count does not match inference row count";
    return KTAG_ERROR_INTERNAL;
  }
  labels->resize(count);
  if (tensor.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
    const int64_t *values = outputs[0].GetTensorData<int64_t>();
    std::copy(values, values + count, labels->begin());
    return KTAG_SUCCESS;
  }
  if (tensor.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
    const int32_t *values = outputs[0].GetTensorData<int32_t>();
    for (size_t index = 0U; index < count; ++index) {
      (*labels)[index] = values[index];
    }
    return KTAG_SUCCESS;
  }
  *error = "ONNX label tensor must contain int32 or int64 values";
  return KTAG_ERROR_NOT_SUPPORTED;
}

void extract_tensor_confidences(const std::vector<Ort::Value> &outputs,
                                size_t row_count,
                                std::vector<float> *confidences) {
  constexpr double probability_tolerance = 1.0e-6;
  confidences->assign(row_count, KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE);
  if (row_count == 0U || outputs.size() < 2U || !outputs[1].IsTensor()) {
    return;
  }
  try {
    const auto tensor = outputs[1].GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> shape = tensor.GetShape();
    if (shape.size() != 2U) {
      return;
    }
    const size_t element_count = tensor.GetElementCount();
    if (element_count == 0U || element_count % row_count != 0U) {
      return;
    }
    const size_t class_count = element_count / row_count;
    if (class_count == 0U) {
      return;
    }
    if ((shape[0] > 0 && static_cast<uint64_t>(shape[0]) != row_count) ||
        (shape[1] > 0 && static_cast<uint64_t>(shape[1]) != class_count)) {
      return;
    }
    if (tensor.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      const float *probabilities = outputs[1].GetTensorData<float>();
      for (size_t row = 0U; row < row_count; ++row) {
        const float *begin = probabilities + row * class_count;
        bool valid = true;
        float confidence = 0.0F;
        for (size_t index = 0U; index < class_count; ++index) {
          const float probability = begin[index];
          if (!std::isfinite(probability) ||
              probability < -probability_tolerance ||
              probability > 1.0 + probability_tolerance) {
            valid = false;
            break;
          }
          confidence =
              std::max(confidence, std::min(1.0F, std::max(0.0F, probability)));
        }
        if (valid) {
          (*confidences)[row] = confidence;
        }
      }
    } else if (tensor.GetElementType() ==
               ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE) {
      const double *probabilities = outputs[1].GetTensorData<double>();
      for (size_t row = 0U; row < row_count; ++row) {
        const double *begin = probabilities + row * class_count;
        bool valid = true;
        double confidence = 0.0;
        for (size_t index = 0U; index < class_count; ++index) {
          const double probability = begin[index];
          if (!std::isfinite(probability) ||
              probability < -probability_tolerance ||
              probability > 1.0 + probability_tolerance) {
            valid = false;
            break;
          }
          confidence =
              std::max(confidence, std::min(1.0, std::max(0.0, probability)));
        }
        if (valid) {
          const float converted = static_cast<float>(confidence);
          if (std::isfinite(converted) && converted >= 0.0F &&
              converted <= 1.0F) {
            (*confidences)[row] = converted;
          }
        }
      }
    }
  } catch (...) {
    confidences->assign(row_count, KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE);
  }
}

} // namespace

struct ktag_inference_context {
  ktag_model_contract contract;
  std::unique_ptr<Ort::Env> environment;
  std::unique_ptr<Ort::Session> session;
  std::string input_name;
  std::vector<std::string> output_names;
  std::atomic<size_t> owner_thread{0U};
};

namespace {

bool check_and_bind_owner(ktag_inference_context *context) {
  const size_t token = current_thread_token();
  const size_t owner = context->owner_thread.load(std::memory_order_acquire);
  if (owner == token) {
    return true;
  }
  if (owner != 0U) {
    return false;
  }

  size_t expected = 0U;
  if (context->owner_thread.compare_exchange_strong(
          expected, token, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return true;
  }
  return expected == token;
}

ktag_status run_prediction(ktag_inference_context *context,
                           const ktag_feature_batch *features,
                           ktag_prediction_batch *predictions) {
  if (context == nullptr || features == nullptr || predictions == nullptr) {
    set_last_error("invalid inference context, features or predictions");
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  if (!check_and_bind_owner(context)) {
    set_last_error("Inference Context was called from a non-owner thread");
    return KTAG_ERROR_INVALID_STATE;
  }
  predictions->records.clear();

  std::vector<float> scaled_values;
  std::vector<uint32_t> source_indexes;
  std::string error;
  ktag_status status = ktag_model_contract_prepare_input_internal(
      context->contract, features, &scaled_values, &source_indexes, &error);
  if (status != KTAG_SUCCESS) {
    set_last_error(error);
    return status;
  }
  if (source_indexes.empty()) {
    set_last_error(std::string());
    return KTAG_SUCCESS;
  }

  try {
    const std::vector<int64_t> input_shape = {
        static_cast<int64_t>(source_indexes.size()),
        static_cast<int64_t>(context->contract.input_value_count)};
    auto memory = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(
        memory, scaled_values.data(), scaled_values.size(), input_shape.data(),
        input_shape.size());

    const char *input_name = context->input_name.c_str();
    std::vector<const char *> output_name_pointers;
    output_name_pointers.reserve(context->output_names.size());
    for (const auto &name : context->output_names) {
      output_name_pointers.push_back(name.c_str());
    }
    std::vector<Ort::Value> outputs = context->session->Run(
        Ort::RunOptions{nullptr}, &input_name, &input, 1U,
        output_name_pointers.data(), output_name_pointers.size());

    std::vector<int64_t> labels;
    status = extract_labels(outputs, source_indexes.size(), &labels, &error);
    if (status != KTAG_SUCCESS) {
      set_last_error(error);
      return status;
    }
    std::vector<float> confidences;
    extract_tensor_confidences(outputs, source_indexes.size(), &confidences);

    predictions->records.reserve(source_indexes.size());
    for (size_t index = 0U; index < source_indexes.size(); ++index) {
      ktag_feature_record feature_record{};
      status = ktag_feature_batch_get_record(features, source_indexes[index],
                                             &feature_record);
      if (status != KTAG_SUCCESS) {
        predictions->records.clear();
        set_last_error("failed to recover Feature Batch metadata");
        return status;
      }
      ktag_prediction_record prediction{};
      prediction.struct_size = sizeof(prediction);
      prediction.api_version = KTAG_API_VERSION;
      prediction.metadata.struct_size = sizeof(prediction.metadata);
      prediction.metadata.api_version = KTAG_API_VERSION;
      prediction.metadata.flow_id = feature_record.metadata.flow_id;
      prediction.metadata.flow_key = feature_record.metadata.flow_key;
      prediction.metadata.first_packet_sequence =
          feature_record.metadata.first_packet_sequence;
      prediction.metadata.last_packet_sequence =
          feature_record.metadata.last_packet_sequence;
      prediction.source_record_index = source_indexes[index];
      prediction.label = labels[index];
      prediction.confidence = confidences[index];
      predictions->records.push_back(prediction);
    }
    set_last_error(std::string());
    return KTAG_SUCCESS;
  } catch (const std::bad_alloc &) {
    predictions->records.clear();
    set_last_error("out of memory");
    return KTAG_ERROR_OUT_OF_MEMORY;
  } catch (const Ort::Exception &exception) {
    predictions->records.clear();
    set_last_error(exception.what());
    return KTAG_ERROR_INTERNAL;
  } catch (const std::exception &exception) {
    predictions->records.clear();
    set_last_error(exception.what());
    return KTAG_ERROR_INTERNAL;
  } catch (...) {
    predictions->records.clear();
    set_last_error("unknown inference error");
    return KTAG_ERROR_INTERNAL;
  }
}

} // namespace

extern "C" {

KTAG_API ktag_status ktag_inference_validate_model_package(
    const char *model_package_path, const ktag_feature_schema *feature_schema,
    ktag_model_package_validation *validation) {
  if (validation == nullptr || validation->struct_size < sizeof(*validation) ||
      validation->api_version != KTAG_API_VERSION) {
    set_last_error("invalid model package validation output");
    return KTAG_ERROR_INVALID_ARGUMENT;
  }
  validation->schema_hash = 0U;
  validation->input_feature_count = 0U;
  validation->input_value_count = 0U;
  validation->minimum_total_packets = 0U;
  ktag_model_contract contract;
  std::string error;
  const ktag_status status = ktag_model_contract_load_internal(
      model_package_path, feature_schema, &contract, &error);
  if (status != KTAG_SUCCESS) {
    set_last_error(error);
    return status;
  }
  validation->schema_hash = contract.schema_hash;
  validation->input_feature_count =
      static_cast<uint32_t>(contract.input_segments.size());
  validation->input_value_count = contract.input_value_count;
  validation->minimum_total_packets = contract.minimum_total_packets;
  set_last_error(std::string());
  return KTAG_SUCCESS;
}

KTAG_API ktag_inference_context *
ktag_inference_create(const ktag_inference_config *config) {
  if (!valid_config(config)) {
    set_last_error("invalid Inference configuration");
    return nullptr;
  }
  try {
    std::unique_ptr<ktag_inference_context> context(
        new (std::nothrow) ktag_inference_context());
    if (!context) {
      set_last_error("out of memory");
      return nullptr;
    }
    std::string error;
    ktag_status status = ktag_model_contract_load_internal(
        config->model_package_path, config->feature_schema, &context->contract,
        &error);
    if (status != KTAG_SUCCESS) {
      set_last_error(error);
      return nullptr;
    }

    context->environment.reset(new Ort::Env(
        parse_log_level(context->contract.onnx_log_level), "KTAG_Inference"));
    Ort::SessionOptions options;
    if (config->intra_op_threads != 0U) {
      options.SetIntraOpNumThreads(static_cast<int>(config->intra_op_threads));
    }
    if (config->inter_op_threads != 0U) {
      options.SetInterOpNumThreads(static_cast<int>(config->inter_op_threads));
    }
    options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    context->session.reset(new Ort::Session(
        *context->environment, context->contract.model_path.c_str(), options));

    status = validate_model_input(context->session.get(),
                                  context->contract.input_value_count, &error);
    if (status != KTAG_SUCCESS) {
      set_last_error(error);
      return nullptr;
    }

    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name = context->session->GetInputNameAllocated(0U, allocator);
    if (!input_name || input_name.get()[0] == '\0') {
      set_last_error("ONNX model input name is invalid");
      return nullptr;
    }
    context->input_name = input_name.get();
    const size_t output_count = context->session->GetOutputCount();
    if (output_count == 0U) {
      set_last_error("ONNX model has no outputs");
      return nullptr;
    }
    context->output_names.reserve(output_count);
    for (size_t index = 0U; index < output_count; ++index) {
      auto output_name =
          context->session->GetOutputNameAllocated(index, allocator);
      if (!output_name || output_name.get()[0] == '\0') {
        set_last_error("ONNX model output name is invalid");
        return nullptr;
      }
      context->output_names.emplace_back(output_name.get());
    }
    set_last_error(std::string());
    return context.release();
  } catch (const std::bad_alloc &) {
    set_last_error("out of memory");
    return nullptr;
  } catch (const Ort::Exception &exception) {
    set_last_error(exception.what());
    return nullptr;
  } catch (const std::exception &exception) {
    set_last_error(exception.what());
    return nullptr;
  } catch (...) {
    set_last_error("unknown inference initialization error");
    return nullptr;
  }
}

KTAG_API ktag_status ktag_inference_predict(
    ktag_inference_context *context, const ktag_feature_batch *features,
    ktag_prediction_batch *predictions) {
  return run_prediction(context, features, predictions);
}

KTAG_API uint64_t
ktag_inference_get_schema_hash(const ktag_inference_context *context) {
  return context == nullptr ? 0U : context->contract.schema_hash;
}

KTAG_API uint32_t
ktag_inference_get_input_value_count(const ktag_inference_context *context) {
  return context == nullptr ? 0U : context->contract.input_value_count;
}

KTAG_API const char *
ktag_inference_get_model_name(const ktag_inference_context *context) {
  return context == nullptr ? nullptr : context->contract.model_name.c_str();
}

KTAG_API void ktag_inference_destroy(ktag_inference_context *context) {
  delete context;
}

KTAG_API const char *ktag_inference_get_last_error(void) {
  return inference_last_error.c_str();
}

} // extern "C"
