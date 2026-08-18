/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PROXY_CARRIER_PARSER_H
#define KTAG_PROXY_CARRIER_PARSER_H

#include <cstddef>
#include <cstdint>

namespace ktag_proxy_features {

struct carrier_parse_result {
  bool http1_request = false;
  bool websocket_upgrade = false;
  bool http2_preface = false;
  bool http2_frame = false;
  bool grpc_content_type = false;
};

void parse_cleartext_carrier(const uint8_t *data, size_t length,
                             carrier_parse_result *result);

} // namespace ktag_proxy_features

#endif /* KTAG_PROXY_CARRIER_PARSER_H */
