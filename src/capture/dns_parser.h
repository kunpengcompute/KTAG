/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DNS_PARSER_H
#define KTAG_DNS_PARSER_H

#include <cstdint>
#include <string>

#include "ktag_capture.h"

using ktag_dns_update_callback = ktag_status (*)(void *user_data,
                                                 const ktag_ip_address &address,
                                                 const std::string &domain,
                                                 uint64_t expire_at_us);

/*
 * Parses bounded DNS response data and publishes A/AAAA observations. Invalid
 * or incomplete DNS messages are ignored because application parsing is
 * best-effort; callback failures are propagated to the caller.
 */
ktag_status ktag_parse_dns_updates_internal(const uint8_t *dns,
                                            uint32_t dns_length,
                                            uint64_t timestamp_us,
                                            ktag_dns_update_callback callback,
                                            void *user_data);

#endif /* KTAG_DNS_PARSER_H */
