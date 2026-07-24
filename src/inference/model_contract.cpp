/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "model_contract_internal.h"

#include "ktag_feature.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

namespace {

constexpr double MINIMUM_SCALER_MAGNITUDE = 1.0e-8;

struct input_feature_spec {
    std::string feature_id;
    uint32_t declared_dimension = 0U;
};

ktag_status fail(
    ktag_status status,
    const std::string &message,
    std::string *error)
{
    if (error != nullptr) {
        *error = message;
    }
    return status;
}

bool parse_hash(const YAML::Node &node, uint64_t *value)
{
    if (!node || !node.IsScalar() || value == nullptr) {
        return false;
    }
    try {
        const std::string text = node.Scalar();
        if (text.empty() || text[0] == '-') {
            return false;
        }
        size_t parsed = 0U;
        const int base = text.size() > 2U && text[0] == '0' &&
                (text[1] == 'x' || text[1] == 'X') ?
            16 : 10;
        const std::string digits = base == 16 ? text.substr(2U) : text;
        if (digits.empty()) {
            return false;
        }
        const unsigned long long result = std::stoull(digits, &parsed, base);
        if (parsed != digits.size()) {
            return false;
        }
        *value = static_cast<uint64_t>(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool path_is_within(const fs::path &root, const fs::path &candidate)
{
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *root_part != *candidate_part) {
            return false;
        }
    }
    return true;
}

ktag_status resolve_package_file(
    const fs::path &package_root,
    const YAML::Node &node,
    const char *field_name,
    fs::path *resolved,
    std::string *error)
{
    if (!node || !node.IsScalar()) {
        return fail(
            KTAG_ERROR_INVALID_ARGUMENT,
            std::string("manifest field '") + field_name +
                "' must be a relative file path",
            error);
    }
    try {
        const fs::path relative(node.as<std::string>());
        if (relative.empty() || relative.is_absolute()) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                std::string("manifest field '") + field_name +
                    "' must remain inside the model package",
                error);
        }
        const fs::path candidate = fs::weakly_canonical(package_root / relative);
        if (!path_is_within(package_root, candidate)) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                std::string("manifest field '") + field_name +
                    "' escapes the model package",
                error);
        }
        if (!fs::is_regular_file(candidate)) {
            return fail(
                KTAG_ERROR_IO,
                std::string("model package file does not exist: ") +
                    candidate.string(),
                error);
        }
        *resolved = candidate;
        return KTAG_SUCCESS;
    } catch (const std::exception &exception) {
        return fail(KTAG_ERROR_IO, exception.what(), error);
    }
}

ktag_status parse_input_features(
    const YAML::Node &node,
    std::vector<input_feature_spec> *features,
    std::string *error)
{
    if (!node || !node.IsSequence() || node.size() == 0U) {
        return fail(
            KTAG_ERROR_INVALID_ARGUMENT,
            "manifest input_features must be a non-empty array",
            error);
    }
    std::unordered_set<std::string> identifiers;
    for (const auto &item : node) {
        input_feature_spec feature;
        try {
            if (item.IsScalar()) {
                feature.feature_id = item.as<std::string>();
            } else if (item.IsMap() && item["feature_id"]) {
                feature.feature_id = item["feature_id"].as<std::string>();
                if (item["dimension"]) {
                    feature.declared_dimension = item["dimension"].as<uint32_t>();
                }
            } else {
                return fail(
                    KTAG_ERROR_INVALID_ARGUMENT,
                    "each input_features item must be a feature ID or an object",
                    error);
            }
        } catch (const YAML::Exception &exception) {
            return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
        }
        if (feature.feature_id.empty() ||
            !identifiers.insert(feature.feature_id).second) {
            return fail(
                KTAG_ERROR_ALREADY_EXISTS,
                "manifest input feature IDs must be non-empty and unique",
                error);
        }
        features->push_back(std::move(feature));
    }
    return KTAG_SUCCESS;
}

ktag_status map_input_features(
    const std::vector<input_feature_spec> &specifications,
    const ktag_feature_schema *schema,
    ktag_model_contract *contract,
    std::string *error)
{
    uint64_t value_count = 0U;
    for (const auto &specification : specifications) {
        ktag_feature_descriptor descriptor{};
        uint32_t source_offset = 0U;
        const ktag_status find_status = ktag_feature_schema_find(
            schema,
            specification.feature_id.c_str(),
            &descriptor,
            &source_offset);

        ktag_model_input_segment segment;
        segment.feature_id = specification.feature_id;
        if (find_status == KTAG_SUCCESS) {
            if (specification.declared_dimension != 0U &&
                specification.declared_dimension != descriptor.dimension) {
                return fail(
                    KTAG_ERROR_SCHEMA_MISMATCH,
                    "manifest dimension does not match Schema for feature '" +
                        specification.feature_id + "'",
                    error);
            }
            segment.source_offset = source_offset;
            segment.dimension = descriptor.dimension;
        } else if (find_status == KTAG_ERROR_NOT_FOUND &&
                   contract->missing_policy ==
                       ktag_missing_feature_policy::fill_zero &&
                   specification.declared_dimension != 0U) {
            segment.dimension = specification.declared_dimension;
            segment.missing = true;
        } else if (find_status == KTAG_ERROR_NOT_FOUND) {
            return fail(
                KTAG_ERROR_NOT_FOUND,
                "Schema does not contain model input feature '" +
                    specification.feature_id + "'",
                error);
        } else {
            return fail(find_status, "failed to query Feature Schema", error);
        }

        value_count += segment.dimension;
        if (segment.dimension == 0U ||
            value_count > std::numeric_limits<uint32_t>::max()) {
            return fail(
                KTAG_ERROR_CAPACITY,
                "model input dimension exceeds the supported range",
                error);
        }
        contract->input_feature_ids.push_back(specification.feature_id);
        contract->input_segments.push_back(std::move(segment));
    }
    contract->input_value_count = static_cast<uint32_t>(value_count);
    return KTAG_SUCCESS;
}

ktag_status parse_packet_filter(
    const YAML::Node &preprocessing,
    const ktag_feature_schema *schema,
    ktag_model_contract *contract,
    std::string *error)
{
    try {
        if (preprocessing["minimum_total_packets"]) {
            contract->minimum_total_packets =
                preprocessing["minimum_total_packets"].as<uint64_t>();
        }
    } catch (const YAML::Exception &exception) {
        return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
    }
    if (contract->minimum_total_packets == 0U) {
        return KTAG_SUCCESS;
    }

    std::vector<std::string> packet_count_features;
    const YAML::Node configured = preprocessing["packet_count_features"];
    if (configured) {
        if (!configured.IsSequence() || configured.size() == 0U) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "packet_count_features must be a non-empty array",
                error);
        }
        try {
            for (const auto &item : configured) {
                packet_count_features.push_back(item.as<std::string>());
            }
        } catch (const YAML::Exception &exception) {
            return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
        }
    } else {
        packet_count_features = {
            "traffic.send.packet_count",
            "traffic.receive.packet_count"};
    }

    std::unordered_set<std::string> unique;
    for (const auto &feature_id : packet_count_features) {
        if (feature_id.empty() || !unique.insert(feature_id).second) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "packet_count_features must contain unique feature IDs",
                error);
        }
        ktag_feature_descriptor descriptor{};
        uint32_t offset = 0U;
        const ktag_status status = ktag_feature_schema_find(
            schema, feature_id.c_str(), &descriptor, &offset);
        if (status != KTAG_SUCCESS || descriptor.dimension != 1U) {
            return fail(
                KTAG_ERROR_NOT_FOUND,
                "packet filter feature is missing or is not scalar: '" +
                    feature_id + "'",
                error);
        }
        contract->packet_count_offsets.push_back(offset);
    }
    return KTAG_SUCCESS;
}

ktag_status parse_number_array(
    const YAML::Node &node,
    const char *field_name,
    std::vector<double> *values,
    std::string *error)
{
    if (!node || !node.IsSequence()) {
        return fail(
            KTAG_ERROR_INVALID_ARGUMENT,
            std::string("scaler field '") + field_name + "' must be an array",
            error);
    }
    try {
        values->reserve(node.size());
        for (const auto &item : node) {
            const double value = item.as<double>();
            if (!std::isfinite(value)) {
                return fail(
                    KTAG_ERROR_INVALID_ARGUMENT,
                    std::string("scaler field '") + field_name +
                        "' contains a non-finite value",
                    error);
            }
            values->push_back(value);
        }
        return KTAG_SUCCESS;
    } catch (const YAML::Exception &exception) {
        return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
    }
}

ktag_status load_scaler(
    const fs::path &path,
    ktag_model_contract *contract,
    std::string *error)
{
    YAML::Node scaler;
    try {
        scaler = YAML::LoadFile(path.string());
    } catch (const YAML::Exception &exception) {
        return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
    }

    uint64_t scaler_hash = 0U;
    if (!parse_hash(scaler["schema_hash"], &scaler_hash)) {
        return fail(
            KTAG_ERROR_INVALID_ARGUMENT,
            "scaler schema_hash is missing or invalid",
            error);
    }
    if (scaler_hash != contract->schema_hash) {
        return fail(
            KTAG_ERROR_SCHEMA_MISMATCH,
            "scaler schema_hash does not match manifest/schema",
            error);
    }

    const YAML::Node scaler_features = scaler["input_features"];
    if (!scaler_features || !scaler_features.IsSequence() ||
        scaler_features.size() != contract->input_feature_ids.size()) {
        return fail(
            KTAG_ERROR_SCHEMA_MISMATCH,
            "scaler input_features do not match manifest input_features",
            error);
    }
    try {
        for (size_t index = 0U; index < contract->input_feature_ids.size(); ++index) {
            if (scaler_features[index].as<std::string>() !=
                contract->input_feature_ids[index]) {
                return fail(
                    KTAG_ERROR_SCHEMA_MISMATCH,
                    "scaler input feature order does not match manifest",
                    error);
            }
        }
    } catch (const YAML::Exception &exception) {
        return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
    }

    ktag_status status = parse_number_array(
        scaler["mean"], "mean", &contract->scaler_mean, error);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    status = parse_number_array(
        scaler["scale"], "scale", &contract->scaler_scale, error);
    if (status != KTAG_SUCCESS) {
        return status;
    }
    if (contract->scaler_mean.size() != contract->input_value_count ||
        contract->scaler_scale.size() != contract->input_value_count) {
        return fail(
            KTAG_ERROR_SCHEMA_MISMATCH,
            "scaler dimensions do not match resolved model input dimension",
            error);
    }
    for (double scale : contract->scaler_scale) {
        if (std::abs(scale) < MINIMUM_SCALER_MAGNITUDE) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "scaler scale contains a zero or near-zero value",
                error);
        }
    }
    return KTAG_SUCCESS;
}

} // namespace

ktag_status ktag_model_contract_load_internal(
    const char *model_package_path,
    const ktag_feature_schema *schema,
    ktag_model_contract *contract,
    std::string *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (model_package_path == nullptr || model_package_path[0] == '\0' ||
        schema == nullptr || contract == nullptr ||
        ktag_feature_schema_is_frozen(schema) == 0U) {
        return fail(
            KTAG_ERROR_INVALID_ARGUMENT,
            "model package path and a frozen Feature Schema are required",
            error);
    }

    try {
        *contract = {};
        const fs::path package_root = fs::weakly_canonical(
            fs::absolute(model_package_path));
        if (!fs::is_directory(package_root)) {
            return fail(
                KTAG_ERROR_IO,
                "model package path is not a directory",
                error);
        }
        const fs::path manifest_path = package_root / "manifest.json";
        if (!fs::is_regular_file(manifest_path)) {
            return fail(
                KTAG_ERROR_IO,
                "model package does not contain manifest.json",
                error);
        }

        YAML::Node manifest;
        try {
            manifest = YAML::LoadFile(manifest_path.string());
        } catch (const YAML::Exception &exception) {
            return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
        }
        if (!manifest.IsMap() || !manifest["model_name"] ||
            !manifest["input_features"] || !manifest["preprocessing"] ||
            !manifest["preprocessing"].IsMap()) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "manifest is missing model_name, input_features or preprocessing",
                error);
        }

        contract->model_name = manifest["model_name"].as<std::string>();
        contract->model_version = manifest["model_version"] ?
            manifest["model_version"].as<std::string>() : std::string();
        contract->onnx_log_level = manifest["onnx_log_level"] ?
            manifest["onnx_log_level"].as<std::string>() : "WARNING";
        if (contract->model_name.empty()) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "manifest model_name must not be empty",
                error);
        }
        const std::unordered_set<std::string> log_levels = {
            "VERBOSE", "INFO", "WARNING", "ERROR", "FATAL"};
        if (log_levels.find(contract->onnx_log_level) == log_levels.end()) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "onnx_log_level must be VERBOSE, INFO, WARNING, ERROR or FATAL",
                error);
        }
        if (!parse_hash(manifest["schema_hash"], &contract->schema_hash)) {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "manifest schema_hash is missing or invalid",
                error);
        }
        if (contract->schema_hash != ktag_feature_schema_get_hash(schema)) {
            return fail(
                KTAG_ERROR_SCHEMA_MISMATCH,
                "manifest schema_hash does not match the configured Feature Schema",
                error);
        }

        const YAML::Node preprocessing = manifest["preprocessing"];
        const std::string policy = preprocessing["missing_value_policy"] ?
            preprocessing["missing_value_policy"].as<std::string>() : "reject";
        if (policy == "reject") {
            contract->missing_policy = ktag_missing_feature_policy::reject;
        } else if (policy == "zero") {
            contract->missing_policy = ktag_missing_feature_policy::fill_zero;
        } else {
            return fail(
                KTAG_ERROR_INVALID_ARGUMENT,
                "missing_value_policy must be 'reject' or 'zero'",
                error);
        }

        std::vector<input_feature_spec> specifications;
        ktag_status status = parse_input_features(
            manifest["input_features"], &specifications, error);
        if (status != KTAG_SUCCESS) {
            return status;
        }
        status = map_input_features(
            specifications, schema, contract, error);
        if (status != KTAG_SUCCESS) {
            return status;
        }
        status = parse_packet_filter(
            preprocessing, schema, contract, error);
        if (status != KTAG_SUCCESS) {
            return status;
        }

        fs::path model_path;
        status = resolve_package_file(
            package_root,
            manifest["model"],
            "model",
            &model_path,
            error);
        if (status != KTAG_SUCCESS) {
            return status;
        }
        fs::path scaler_path;
        status = resolve_package_file(
            package_root,
            preprocessing["scaler"],
            "preprocessing.scaler",
            &scaler_path,
            error);
        if (status != KTAG_SUCCESS) {
            return status;
        }
        contract->model_path = model_path.string();
        contract->scaler_path = scaler_path.string();
        return load_scaler(scaler_path, contract, error);
    } catch (const std::bad_alloc &) {
        return fail(KTAG_ERROR_OUT_OF_MEMORY, "out of memory", error);
    } catch (const YAML::Exception &exception) {
        return fail(KTAG_ERROR_INVALID_ARGUMENT, exception.what(), error);
    } catch (const std::exception &exception) {
        return fail(KTAG_ERROR_INTERNAL, exception.what(), error);
    } catch (...) {
        return fail(KTAG_ERROR_INTERNAL, "unknown model package error", error);
    }
}

ktag_status ktag_model_contract_prepare_input_internal(
    const ktag_model_contract &contract,
    const ktag_feature_batch *features,
    std::vector<float> *scaled_values,
    std::vector<uint32_t> *source_record_indexes,
    std::string *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (features == nullptr || scaled_values == nullptr ||
        source_record_indexes == nullptr) {
        return fail(KTAG_ERROR_INVALID_ARGUMENT, "invalid inference input", error);
    }
    scaled_values->clear();
    source_record_indexes->clear();
    if (ktag_feature_batch_get_schema_hash(features) != contract.schema_hash) {
        return fail(
            KTAG_ERROR_SCHEMA_MISMATCH,
            "Feature Batch schema_hash does not match the model contract",
            error);
    }

    try {
        const uint32_t record_count = ktag_feature_batch_get_count(features);
        const size_t maximum_values =
            static_cast<size_t>(record_count) * contract.input_value_count;
        if (contract.input_value_count != 0U &&
            maximum_values / contract.input_value_count != record_count) {
            return fail(KTAG_ERROR_CAPACITY, "inference batch is too large", error);
        }
        scaled_values->reserve(maximum_values);
        source_record_indexes->reserve(record_count);

        for (uint32_t record_index = 0U;
             record_index < record_count;
             ++record_index) {
            ktag_feature_record record{};
            ktag_status status = ktag_feature_batch_get_record(
                features, record_index, &record);
            if (status != KTAG_SUCCESS) {
                return fail(status, "failed to read Feature Batch record", error);
            }

            double total_packets = 0.0;
            for (uint32_t offset : contract.packet_count_offsets) {
                if (offset >= record.value_count ||
                    !std::isfinite(record.values[offset])) {
                    return fail(
                        KTAG_ERROR_SCHEMA_MISMATCH,
                        "packet filter feature is outside the input record",
                        error);
                }
                total_packets += record.values[offset];
            }
            if (contract.minimum_total_packets != 0U &&
                total_packets <
                    static_cast<double>(contract.minimum_total_packets)) {
                continue;
            }

            size_t scaler_offset = 0U;
            for (const auto &segment : contract.input_segments) {
                for (uint32_t dimension = 0U;
                     dimension < segment.dimension;
                     ++dimension, ++scaler_offset) {
                    double value = 0.0;
                    if (!segment.missing) {
                        const uint64_t source =
                            static_cast<uint64_t>(segment.source_offset) +
                            dimension;
                        if (source >= record.value_count) {
                            return fail(
                                KTAG_ERROR_SCHEMA_MISMATCH,
                                "resolved feature is outside the input record",
                                error);
                        }
                        value = record.values[source];
                        if (!std::isfinite(value)) {
                            return fail(
                                KTAG_ERROR_INVALID_ARGUMENT,
                                "model input contains a non-finite value",
                                error);
                        }
                    }
                    const double scaled =
                        (value - contract.scaler_mean[scaler_offset]) /
                        contract.scaler_scale[scaler_offset];
                    if (!std::isfinite(scaled)) {
                        return fail(
                            KTAG_ERROR_INVALID_ARGUMENT,
                            "scaled model input is non-finite",
                            error);
                    }
                    const float converted = static_cast<float>(scaled);
                    if (!std::isfinite(converted)) {
                        return fail(
                            KTAG_ERROR_INVALID_ARGUMENT,
                            "scaled model input exceeds float32 range",
                            error);
                    }
                    scaled_values->push_back(converted);
                }
            }
            if (scaler_offset != contract.input_value_count) {
                return fail(
                    KTAG_ERROR_INTERNAL,
                    "resolved model input dimension is inconsistent",
                    error);
            }
            source_record_indexes->push_back(record_index);
        }
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        scaled_values->clear();
        source_record_indexes->clear();
        return fail(KTAG_ERROR_OUT_OF_MEMORY, "out of memory", error);
    } catch (const std::exception &exception) {
        scaled_values->clear();
        source_record_indexes->clear();
        return fail(KTAG_ERROR_INTERNAL, exception.what(), error);
    } catch (...) {
        scaled_values->clear();
        source_record_indexes->clear();
        return fail(KTAG_ERROR_INTERNAL, "unknown preprocessing error", error);
    }
}
