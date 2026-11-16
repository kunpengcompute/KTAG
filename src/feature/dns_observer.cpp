/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#include "dns_observer_internal.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
namespace {
constexpr uint32_t DNS_RESPOND_IPV4_TYPE = 1U;
constexpr uint32_t DNS_RESPOND_IPV6_TYPE = 28U;
}
static uint64_t dns_expire_at_us(const ktag_timestamp &timestamp,
                                 uint32_t ttl_seconds) {
    uint64_t now_us = 0U;
    if (timestamp.seconds >= 0 && timestamp.microseconds >= 0) {
        const uint64_t seconds = static_cast<uint64_t>(timestamp.seconds);
        const uint64_t microseconds = static_cast<uint64_t>(timestamp.microseconds);
        if (seconds <=
            (std::numeric_limits<uint64_t>::max() - microseconds) / 1000000U) {
            now_us = seconds * 1000000U + microseconds;
        }
    }
    if (ttl_seconds == 0U) {
        return now_us == std::numeric_limits<uint64_t>::max() ? now_us
                                                              : now_us + 1U;
    }
    const uint64_t ttl_us = static_cast<uint64_t>(ttl_seconds) * 1000000U;
    return ttl_us > std::numeric_limits<uint64_t>::max() - now_us
               ? std::numeric_limits<uint64_t>::max()
               : now_us + ttl_us;
}

static uint16_t dns_read_u16(const uint8_t *data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) |
                                 static_cast<uint16_t>(data[1]));
}

static uint32_t dns_read_u32(const uint8_t *data) {
    return (static_cast<uint32_t>(data[0]) << 24U) |
           (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) |
           static_cast<uint32_t>(data[3]);
}

static bool dns_read_name_bounded(const uint8_t *data, size_t length,
                                  size_t *position, std::string *domain) {
    if (data == nullptr || position == nullptr || domain == nullptr ||
        *position >= length) {
        return false;
    }
    size_t cursor = *position;
    bool jumped = false;
    uint32_t steps = 0U;
    domain->clear();
    while (cursor < length && steps++ < 128U) {
        const uint8_t label_length = data[cursor];
        if (label_length == 0U) {
            if (!jumped) {
                *position = cursor + 1U;
            }
            return true;
        }
        if ((label_length & 0xc0U) == 0xc0U) {
            if (cursor + 1U >= length) {
                return false;
            }
            const uint16_t pointer = static_cast<uint16_t>(
                (static_cast<uint16_t>(label_length & 0x3fU) << 8U) |
                data[cursor + 1U]);
            if (pointer >= length) {
                return false;
            }
            if (!jumped) {
                *position = cursor + 2U;
                jumped = true;
            }
            cursor = pointer;
            continue;
        }
        if ((label_length & 0xc0U) != 0U || label_length > 63U ||
            cursor + 1U + label_length > length) {
            return false;
        }
        if (!domain->empty()) {
            domain->push_back('.');
        }
        domain->append(reinterpret_cast<const char *>(data + cursor + 1U),
                       label_length);
        cursor += 1U + label_length;
        if (!jumped) {
            *position = cursor;
        }
    }
    return false;
}

ktag_status ktag_dns_observe_internal(const uint8_t *dns,
                                             uint32_t dns_length,
                                             const ktag_timestamp &timestamp,
                                             ktag_dns_update_callback callback,
                                             void *user_data) {
    constexpr uint32_t DNS_HEADER_LENGTH = 12U;
    if (dns == nullptr || dns_length < DNS_HEADER_LENGTH ||
        callback == nullptr) {
        return KTAG_SUCCESS;
    }
    const uint16_t question_count = dns_read_u16(dns + 4U);
    const uint16_t answer_count =
        std::min<uint16_t>(dns_read_u16(dns + 6U), 1024U);
    size_t position = DNS_HEADER_LENGTH;
    std::string question_domain;
    for (uint16_t index = 0U; index < question_count; ++index) {
        std::string current_domain;
        if (!dns_read_name_bounded(dns, dns_length, &position,
                                   &current_domain) ||
            position + 4U > dns_length) {
            return KTAG_SUCCESS;
        }
        if (question_domain.empty()) {
            question_domain = std::move(current_domain);
        }
        position += 4U;
    }
    if (question_domain.empty()) {
        return KTAG_SUCCESS;
    }
    question_domain.push_back('.');

    for (uint16_t index = 0U; index < answer_count; ++index) {
        std::string answer_name;
        if (!dns_read_name_bounded(dns, dns_length, &position, &answer_name) ||
            position + 10U > dns_length) {
            break;
        }
        const uint16_t type = dns_read_u16(dns + position);
        const uint16_t record_class = dns_read_u16(dns + position + 2U);
        const uint32_t ttl = dns_read_u32(dns + position + 4U);
        const uint16_t record_length = dns_read_u16(dns + position + 8U);
        position += 10U;
        if (position + record_length > dns_length) {
            break;
        }

        ktag_ip_address address{};
        if (record_class == 1U && type == DNS_RESPOND_IPV4_TYPE &&
            record_length == 4U) {
            address.version = 4U;
            std::memcpy(address.bytes, dns + position, 4U);
        } else if (record_class == 1U && type == DNS_RESPOND_IPV6_TYPE &&
                   record_length == KTAG_IPV6_ADDRESS_SIZE) {
            address.version = 6U;
            std::memcpy(address.bytes, dns + position, KTAG_IPV6_ADDRESS_SIZE);
        }
        if (address.version != 0U) {
            const ktag_status status =
                callback(user_data, &address, question_domain,
                         dns_expire_at_us(timestamp, ttl));
            if (status != KTAG_SUCCESS) {
                return status;
            }
        }
        position += record_length;
    }
    return KTAG_SUCCESS;
}
