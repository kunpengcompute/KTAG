/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
#include "http.h"
#include <cstring>
namespace ktag_traffic_features::legacy {
namespace {
const std::string HTTP_PREFIX = "http://";
constexpr int HTTP_PREFIX_LENGTH = 7;
const std::string HTTP_PROPERTY_HOST = "Host:";
constexpr int HTTP_PROPERTY_HOST_LENGTH = 5;
struct application_info { std::string http_uri, http_host, http_url; };
bool parse_http_payload(const uint8_t *packet, application_info *info,
                               uint32_t length) {
    if (!packet) {
        return false;
    }
    std::string uri;
    std::string host;
    size_t url_start;
    size_t url_end;
    size_t host_start;
    size_t host_end;

    std::string_view sv((char *)packet, length);
    // Parse the request line
    size_t line_end = sv.find("\r\n");
    if (line_end == std::string_view::npos) {
        return false;
    }
    // Extract URL in the request line
    size_t first_space = sv.find(' ');
    if (first_space == std::string_view::npos) {
        return false;
    }
    url_start = first_space + 1;
    url_end = sv.find(' ', url_start);
    if (url_end == std::string_view::npos) {
        return false;
    }

    uri = sv.substr(url_start, url_end - url_start);
    info->http_uri = uri;
    if (uri.compare(0, HTTP_PREFIX_LENGTH, HTTP_PREFIX) == 0) {
        info->http_url = uri;
        return true;
    }

    // Concatenate host and uri
    host_start = sv.find(HTTP_PROPERTY_HOST, url_end);
    if (host_start == std::string_view::npos) {
        info->http_url = uri;
        return true;
    }
    host_start += HTTP_PROPERTY_HOST_LENGTH; // "Host:"
    while (host_start < sv.size() && sv[host_start] == ' ') {
        host_start++;
    }
    host_end = sv.find("\r\n", host_start);
    if (host_end == std::string_view::npos) {
        info->http_url = info->http_uri;
        return false;
    }
    host = sv.substr(host_start, host_end - host_start);
    info->http_host = host;
    info->http_url = HTTP_PREFIX + host + uri;
    return true;
}

}
std::string http_url(const uint8_t *payload, uint32_t length) {
    if (!payload || length < 7U) return {};
    bool http = false;
    for (const char *method : {"GET ", "POST ", "PUT ", "DELETE ", "HEAD "}) {
        if (length >= std::strlen(method) && std::memcmp(payload, method, std::strlen(method)) == 0) { http = true; break; }
    }
    if (!http) return {};
    application_info info{};
    parse_http_payload(payload, &info, length);
    return info.http_url;
}
}
