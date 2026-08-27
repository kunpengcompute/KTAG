/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#include "feature_schema_internal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace {

constexpr uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV1A_PRIME = 1099511628211ULL;

bool valid_string(const char *value, size_t maximum_length) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    for (size_t index = 0; index <= maximum_length; ++index) {
        if (value[index] == '\0') {
            return true;
        }
    }
    return false;
}

void hash_byte(uint64_t &hash, uint8_t value) {
    hash ^= value;
    hash *= FNV1A_PRIME;
}

void hash_u32(uint64_t &hash, uint32_t value) {
    for (uint32_t index = 0; index < sizeof(value); ++index) {
        hash_byte(hash, static_cast<uint8_t>((value >> (index * 8U)) & 0xffU));
    }
}

void hash_string(uint64_t &hash, const std::string &value) {
    hash_u32(hash, static_cast<uint32_t>(value.size()));
    for (unsigned char character : value) {
        hash_byte(hash, character);
    }
}

uint64_t calculate_schema_hash(const ktag_feature_schema &schema) {
    uint64_t hash = FNV1A_OFFSET_BASIS;
    hash_string(hash, schema.name);
    hash_u32(hash, schema.version);
    hash_u32(hash, static_cast<uint32_t>(schema.descriptors.size()));

    for (uint32_t index = 0; index < schema.descriptors.size(); ++index) {
        const auto &descriptor = schema.descriptors[index];
        hash_u32(hash, index);
        hash_string(hash, descriptor.feature_id);
        hash_string(hash, descriptor.contract_provider_id);
        hash_u32(hash, descriptor.provider_version);
        hash_u32(hash, static_cast<uint32_t>(descriptor.value_type));
        hash_u32(hash, descriptor.dimension);
    }
    return hash == 0 ? 1 : hash;
}

auto find_descriptor(ktag_feature_schema *schema, const char *feature_id) {
    return std::find_if(
        schema->descriptors.begin(), schema->descriptors.end(),
        [feature_id](const ktag_feature_descriptor_storage &item) {
            return item.feature_id == feature_id;
        });
}

ktag_status add_descriptor(ktag_feature_schema *schema,
                           const ktag_feature_descriptor *descriptor,
                           const char *contract_provider_id) {
    if (schema == nullptr || !ktag_feature_descriptor_is_valid(descriptor) ||
        !valid_string(contract_provider_id, KTAG_PROVIDER_ID_MAX_LENGTH)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (schema->frozen) {
        return KTAG_ERROR_INVALID_STATE;
    }
    if (find_descriptor(schema, descriptor->feature_id) !=
        schema->descriptors.end()) {
        return KTAG_ERROR_ALREADY_EXISTS;
    }
    if (schema->value_count >
        std::numeric_limits<uint32_t>::max() - descriptor->dimension) {
        return KTAG_ERROR_CAPACITY;
    }

    try {
        ktag_feature_descriptor_storage storage;
        storage.feature_id = descriptor->feature_id;
        storage.display_name = descriptor->display_name;
        storage.provider_id = descriptor->provider_id;
        storage.contract_provider_id = contract_provider_id;
        storage.provider_version = descriptor->provider_version;
        storage.value_type = descriptor->value_type;
        storage.dimension = descriptor->dimension;
        schema->descriptors.push_back(std::move(storage));
        schema->value_count += descriptor->dimension;
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

} // namespace

ktag_status
ktag_feature_schema_add_contract(ktag_feature_schema *schema,
                                 const ktag_feature_descriptor *descriptor,
                                 const char *contract_provider_id) {
    return add_descriptor(schema, descriptor, contract_provider_id);
}

bool ktag_feature_descriptor_is_valid(
    const ktag_feature_descriptor *descriptor) {
    if (descriptor == nullptr ||
        descriptor->struct_size < sizeof(ktag_feature_descriptor) ||
        descriptor->api_version != KTAG_API_VERSION) {
        return false;
    }
    if (!valid_string(descriptor->feature_id, KTAG_FEATURE_ID_MAX_LENGTH) ||
        !valid_string(descriptor->display_name, KTAG_FEATURE_NAME_MAX_LENGTH) ||
        !valid_string(descriptor->provider_id, KTAG_PROVIDER_ID_MAX_LENGTH)) {
        return false;
    }
    return descriptor->provider_version > 0 &&
           descriptor->value_type == KTAG_FEATURE_FLOAT32 &&
           descriptor->dimension > 0;
}

void ktag_feature_copy_descriptor(const ktag_feature_descriptor_storage &source,
                                  ktag_feature_descriptor *destination) {
    destination->struct_size = sizeof(ktag_feature_descriptor);
    destination->api_version = KTAG_API_VERSION;
    destination->feature_id = source.feature_id.c_str();
    destination->display_name = source.display_name.c_str();
    destination->provider_id = source.provider_id.c_str();
    destination->provider_version = source.provider_version;
    destination->value_type = source.value_type;
    destination->dimension = source.dimension;
}

extern "C" {

KTAG_API ktag_feature_schema *
ktag_feature_schema_create(const char *schema_name, uint32_t schema_version) {
    if (!valid_string(schema_name, KTAG_FEATURE_NAME_MAX_LENGTH) ||
        schema_version == 0) {
        return nullptr;
    }
    try {
        std::unique_ptr<ktag_feature_schema> schema(new (std::nothrow)
                                                        ktag_feature_schema());
        if (schema == nullptr) {
            return nullptr;
        }
        schema->name = schema_name;
        schema->version = schema_version;
        return schema.release();
    } catch (...) {
        return nullptr;
    }
}

KTAG_API void ktag_feature_schema_destroy(ktag_feature_schema *schema) {
    delete schema;
}

KTAG_API ktag_status ktag_feature_schema_add(
    ktag_feature_schema *schema, const ktag_feature_descriptor *descriptor) {
    if (descriptor == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    return add_descriptor(schema, descriptor, descriptor->provider_id);
}

KTAG_API ktag_status ktag_feature_schema_remove(ktag_feature_schema *schema,
                                                const char *feature_id) {
    if (schema == nullptr ||
        !valid_string(feature_id, KTAG_FEATURE_ID_MAX_LENGTH)) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (schema->frozen) {
        return KTAG_ERROR_INVALID_STATE;
    }
    auto descriptor = find_descriptor(schema, feature_id);
    if (descriptor == schema->descriptors.end()) {
        return KTAG_ERROR_NOT_FOUND;
    }
    schema->value_count -= descriptor->dimension;
    schema->descriptors.erase(descriptor);
    return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_feature_schema_freeze(ktag_feature_schema *schema) {
    if (schema == nullptr) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    if (schema->frozen) {
        return KTAG_SUCCESS;
    }
    if (schema->descriptors.empty() || schema->value_count == 0) {
        return KTAG_ERROR_INVALID_STATE;
    }

    try {
        std::unordered_map<std::string_view, uint32_t> feature_indices;
        feature_indices.reserve(schema->descriptors.size());
        for (uint32_t index = 0; index < schema->descriptors.size(); ++index) {
            feature_indices.emplace(
                std::string_view(schema->descriptors[index].feature_id), index);
        }

        uint32_t offset = 0;
        for (auto &descriptor : schema->descriptors) {
            descriptor.value_offset = offset;
            offset += descriptor.dimension;
        }
        schema->feature_indices = std::move(feature_indices);
        schema->schema_hash = calculate_schema_hash(*schema);
        schema->frozen = true;
        return KTAG_SUCCESS;
    } catch (const std::bad_alloc &) {
        return KTAG_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return KTAG_ERROR_INTERNAL;
    }
}

KTAG_API uint32_t
ktag_feature_schema_is_frozen(const ktag_feature_schema *schema) {
    return schema != nullptr && schema->frozen ? 1U : 0U;
}

KTAG_API uint32_t
ktag_feature_schema_get_feature_count(const ktag_feature_schema *schema) {
    return schema == nullptr
               ? 0U
               : static_cast<uint32_t>(schema->descriptors.size());
}

KTAG_API uint32_t
ktag_feature_schema_get_value_count(const ktag_feature_schema *schema) {
    return schema == nullptr ? 0U : schema->value_count;
}

KTAG_API uint64_t
ktag_feature_schema_get_hash(const ktag_feature_schema *schema) {
    return schema == nullptr || !schema->frozen ? 0U : schema->schema_hash;
}

KTAG_API const char *
ktag_feature_schema_get_name(const ktag_feature_schema *schema) {
    return schema == nullptr ? nullptr : schema->name.c_str();
}

KTAG_API uint32_t
ktag_feature_schema_get_version(const ktag_feature_schema *schema) {
    return schema == nullptr ? 0U : schema->version;
}

KTAG_API ktag_status ktag_feature_schema_get_descriptor(
    const ktag_feature_schema *schema, uint32_t index,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset) {
    if (schema == nullptr || descriptor == nullptr || value_offset == nullptr ||
        !schema->frozen || index >= schema->descriptors.size()) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    ktag_feature_copy_descriptor(schema->descriptors[index], descriptor);
    *value_offset = schema->descriptors[index].value_offset;
    return KTAG_SUCCESS;
}

KTAG_API ktag_status ktag_feature_schema_find(
    const ktag_feature_schema *schema, const char *feature_id,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset) {
    if (schema == nullptr ||
        !valid_string(feature_id, KTAG_FEATURE_ID_MAX_LENGTH) ||
        descriptor == nullptr || value_offset == nullptr || !schema->frozen) {
        return KTAG_ERROR_INVALID_ARGUMENT;
    }
    const auto position =
        schema->feature_indices.find(std::string_view(feature_id));
    if (position == schema->feature_indices.cend()) {
        return KTAG_ERROR_NOT_FOUND;
    }
    const auto &item = schema->descriptors[position->second];
    ktag_feature_copy_descriptor(item, descriptor);
    *value_offset = item.value_offset;
    return KTAG_SUCCESS;
}

} // extern "C"
