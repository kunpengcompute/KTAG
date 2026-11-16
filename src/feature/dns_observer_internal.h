/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#ifndef KTAG_DNS_OBSERVER_INTERNAL_H
#define KTAG_DNS_OBSERVER_INTERNAL_H
#include "ktag_feature.h"
#include <string>
using ktag_dns_update_callback = ktag_status (*)(void *, const ktag_ip_address *, const std::string &, uint64_t);
ktag_status ktag_dns_observe_internal(const uint8_t *, uint32_t, const ktag_timestamp &, ktag_dns_update_callback, void *);
#endif
