/*
        Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights
   reserved.
*/
#include "decode.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <unordered_map>
#include <utility>

const int ETHERNET_HEADER_LEN = 14;
const int UDP_HEADER_LEN = 8;

const int DLT_EN10MB = 1;      /* Ethernet (10Mb) */
const int DLT_LINUX_SLL = 113; /* Linux cooked sockets. */

const std::string HTTP_PREFIX = "http://";
const int HTTP_PREFIX_LENGTH = 7;

const std::string HTTP_PROPERTY_HOST = "Host:";
const int HTTP_PROPERTY_HOST_LENGTH = 5;

const int HTTP_MIN_LENGTH = 7; /* Min Length "GET / " */
const int HTTP_METHOD_NUMS = 5;
const char *http_methods[] = {"GET ", "POST ", "PUT ", "DELETE ", "HEAD "};

const int UNIT_BYTES_FOUR = 4;

const int DNS_PROTOCOL_PORT = 53;
const int DNS_RESPOND_IPV4_TYPE = 1;
const int DNS_RESPOND_IPV6_TYPE = 28;

const int BYTE_OFFSET_0 = 0;
const int BYTE_OFFSET_1 = 1;
const int BYTE_OFFSET_2 = 2;
const int SHIFT_16 = 16;
const int SHIFT_8 = 8;

bool is_http_payload(const u_char *packet, int length) {
    if (length < HTTP_MIN_LENGTH || !packet) {
        return false;
    }

    for (int i = 0; i < HTTP_METHOD_NUMS; i++) {
        if (strncmp(reinterpret_cast<const char *>(packet), http_methods[i],
                    strlen(http_methods[i])) == 0) {
            return true;
        }
    }
    return false;
}

KTAGResult parse_http_payload(const u_char *packet, packet_info *info,
                               uint32_t length) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
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
        return KTAG_STS_DECODE_HTTP_ERROR;
    }
    // Extract URL in the request line
    size_t first_space = sv.find(' ');
    if (first_space == std::string_view::npos) {
        return KTAG_STS_DECODE_HTTP_ERROR;
    }
    url_start = first_space + 1;
    url_end = sv.find(' ', url_start);
    if (url_end == std::string_view::npos) {
        return KTAG_STS_DECODE_HTTP_ERROR;
    }

    uri = sv.substr(url_start, url_end - url_start);
    info->http_uri = uri;
    if (uri.compare(0, HTTP_PREFIX_LENGTH, HTTP_PREFIX) == 0) {
        info->http_url = uri;
        return KTAG_STS_NO_ERR;
    }

    // Concatenate host and uri
    host_start = sv.find(HTTP_PROPERTY_HOST, url_end);
    if (host_start == std::string_view::npos) {
        info->http_url = uri;
        return KTAG_STS_NO_ERR;
    }
    host_start += HTTP_PROPERTY_HOST_LENGTH; // "Host:"
    while (host_start < sv.size() && sv[host_start] == ' ') {
        host_start++;
    }
    host_end = sv.find("\r\n", host_start);
    if (host_end == std::string_view::npos) {
        info->http_url = info->http_uri;
        return KTAG_STS_DECODE_HTTP_ERROR;
    }
    host = sv.substr(host_start, host_end - host_start);
    info->http_host = host;
    info->http_url = HTTP_PREFIX + host + uri;
    return KTAG_STS_NO_ERR;
}

KTAGResult parse_tls_sni(const size_t msg_body_start,
                          const size_t extensions_len, packet_info *info,
                          const std::string_view &sv) {
    if (sv.empty() || info == nullptr) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    if (msg_body_start > sv.size() ||
        extensions_len > sv.size() - msg_body_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    size_t extensions_cur = msg_body_start;
    size_t extensions_end = extensions_cur + extensions_len;

    if (extensions_end - extensions_cur <=
        static_cast<size_t>(TLS_EXTENSION_TYPE_AND_LENGTH)) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    uint16_t ext_type = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(sv[extensions_cur]))
         << 8U) |
        static_cast<uint8_t>(sv[extensions_cur + 1U]));
    uint16_t ext_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(sv[extensions_cur + 2U]))
         << 8U) |
        static_cast<uint8_t>(sv[extensions_cur + 3U]));
    extensions_cur += TLS_EXTENSION_TYPE_AND_LENGTH;
    if (ext_len >= extensions_end - extensions_cur) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    if (ext_type == TLS_EXTENSION_TYPE) {
        if (extensions_cur + TLS_EXTENSION_SERVER_NAME_LIST_LENGTH +
                TLS_EXTENSION_SERVER_NAME_TYPE_AND_LENGTH <
            extensions_end) {
            extensions_cur += TLS_EXTENSION_SERVER_NAME_LIST_LENGTH;
            size_t sni_start = extensions_cur;
            if (sni_start >= sv.size()) {
                return KTAG_STS_DECODE_TLS_ERROR;
            }
            uint8_t name_type = sv[sni_start];
            if (sni_start + 1 + sizeof(uint16_t) >= sv.size()) {
                return KTAG_STS_DECODE_TLS_ERROR;
            }
            uint16_t name_len = static_cast<uint16_t>(
                (static_cast<uint16_t>(static_cast<uint8_t>(sv[sni_start + 1U]))
                 << 8U) |
                static_cast<uint8_t>(sv[sni_start + 2U]));
            sni_start += TLS_EXTENSION_SERVER_NAME_TYPE_AND_LENGTH;

            if (name_type == TLS_SNI_NAME_TYPE && name_len > 0) {
                if (name_len > extensions_end - sni_start) {
                    return KTAG_STS_DECODE_TLS_ERROR;
                }
                std::string sni_str(sv.data() + sni_start, name_len);
                info->tls_info.sni = sni_str;
            }
        }
    }

    return KTAG_STS_NO_ERR;
}

size_t parse_tls_cipher_suits(size_t msg_start, packet_info *info,
                              const std::string_view &sv) {
    if (sv.empty() || info == nullptr || msg_start > sv.size()) {
        return msg_start;
    }
    size_t msg_body_start = msg_start;
    if (sv.size() - msg_body_start <=
        static_cast<size_t>(TLS_CIPHER_SUITS_LENGTH)) {
        return msg_start;
    }
    uint16_t cipher_suites_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(sv[msg_body_start]))
         << 8U) |
        static_cast<uint8_t>(sv[msg_body_start + 1U]));
    if (cipher_suites_len >
        sv.size() - msg_body_start - TLS_CIPHER_SUITS_LENGTH) {
        cipher_suites_len =
            sv.size() - msg_body_start - TLS_CIPHER_SUITS_LENGTH;
    }
    msg_body_start += TLS_CIPHER_SUITS_LENGTH;
    info->tls_info.cipher_suites_length = cipher_suites_len;

    size_t cipher_suites_count = cipher_suites_len / 2;
    for (size_t i = 0; i < cipher_suites_count; ++i) {
        if (msg_body_start + (i << 1) + 1 >= sv.size()) {
            break;
        }
        uint8_t high_byte = sv[msg_body_start + (i << 1)];
        uint8_t low_byte = sv[msg_body_start + (i << 1) + 1];
        uint16_t cipher_suite = (high_byte << 8) | low_byte;
        if (i < 256) {
            info->tls_info.cipher_suites[i] = cipher_suite;
        }
    }
    msg_body_start += cipher_suites_len;

    return msg_body_start;
}

KTAGResult parse_tls_client_hello(size_t handshake_start, size_t handshake_end,
                                   packet_info *info,
                                   const std::string_view &sv) {
    if (sv.empty() || info == nullptr) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    if (handshake_start >= sv.size() || handshake_end > sv.size() ||
        handshake_end <= handshake_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }

    size_t msg_body_start = handshake_start + 1;

    // client length 占3byte
    if (static_cast<size_t>(TLS_CLIENT_LENGTH) >=
        handshake_end - msg_body_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    size_t handshake_len_byte0 =
        static_cast<uint8_t>(sv[msg_body_start + BYTE_OFFSET_0]) << SHIFT_16;
    size_t handshake_len_byte1 =
        static_cast<uint8_t>(sv[msg_body_start + BYTE_OFFSET_1]) << SHIFT_8;
    size_t handshake_len_byte2 =
        static_cast<uint8_t>(sv[msg_body_start + BYTE_OFFSET_2]);
    size_t handshake_len =
        handshake_len_byte0 | handshake_len_byte1 | handshake_len_byte2;
    // 至少需要 38 字节(client_length(3) + client_version(2) + random(32) +
    // session_id_len(1))
    if (handshake_len <
        (TLS_CLIENT_VERSION_RANDOM_LENGTH + TLS_CLIENT_LENGTH + 1)) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    msg_body_start += TLS_CLIENT_LENGTH;
    if (handshake_len > handshake_end - msg_body_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    const size_t client_hello_end = msg_body_start + handshake_len;
    if (static_cast<size_t>(TLS_CLIENT_VERSION_RANDOM_LENGTH + 1) >
        client_hello_end - msg_body_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    uint16_t client_version = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(sv[msg_body_start]))
         << 8U) |
        static_cast<uint8_t>(sv[msg_body_start + 1U]));
    info->tls_info.client_version = client_version;

    msg_body_start +=
        TLS_CLIENT_VERSION_RANDOM_LENGTH; // client_version(2) + random(32)
    uint8_t session_id_len = sv[msg_body_start];
    ++msg_body_start;
    if (session_id_len > client_hello_end - msg_body_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    msg_body_start += session_id_len;

    if (client_hello_end - msg_body_start <=
        static_cast<size_t>(TLS_CIPHER_SUITS_LENGTH)) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    const uint16_t cipher_suites_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(sv[msg_body_start]))
         << 8U) |
        static_cast<uint8_t>(sv[msg_body_start + 1U]));
    if (cipher_suites_len >
        client_hello_end - msg_body_start - TLS_CIPHER_SUITS_LENGTH) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }

    msg_body_start = parse_tls_cipher_suits(msg_body_start, info, sv);
    // 跳过 compression_methods
    if (msg_body_start >= client_hello_end) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    uint8_t compression_methods_len = sv[msg_body_start];
    ++msg_body_start;
    if (compression_methods_len > client_hello_end - msg_body_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    msg_body_start += compression_methods_len;

    // 解析 extensions
    if (client_hello_end - msg_body_start <
        static_cast<size_t>(TLS_EXTENSIONS_LENGTH)) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    uint16_t extensions_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(sv[msg_body_start]))
         << 8U) |
        static_cast<uint8_t>(sv[msg_body_start + 1U]));
    info->tls_info.extensions_length = extensions_len;
    msg_body_start += TLS_EXTENSIONS_LENGTH;
    if (extensions_len > client_hello_end - msg_body_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }

    return parse_tls_sni(msg_body_start, extensions_len, info, sv);
}

KTAGResult parse_tls(const u_char *data, packet_info *info, uint32_t len) {
    if (!data || info == nullptr) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }

    std::string_view sv((char *)data, len);
    if (sv.size() < TLS_RECODE_MIN_LENGTH + sizeof(uint16_t)) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    size_t handshake_start = TLS_RECODE_MIN_LENGTH;
    uint16_t record_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(sv[3U])) << 8U) |
        static_cast<uint8_t>(sv[4U]));
    if (record_len > sv.size() - handshake_start) {
        return KTAG_STS_DECODE_TLS_ERROR;
    }
    size_t handshake_end = handshake_start + record_len;
    if (handshake_end <= sv.size() && handshake_end > handshake_start) {
        uint8_t msg_type = sv[handshake_start];
        if (msg_type == TLS_CLIENTHELLO_TYPE) { // ClientHello 消息
            return parse_tls_client_hello(handshake_start, handshake_end, info,
                                          sv);
        }
    }

    return KTAG_STS_NO_ERR;
}

bool is_tls_payload(const u_char *packet, int length) {
    if (length < TLS_MIN_LENGTH || !packet) {
        return false;
    }
    uint8_t content_type = packet[0];
    if (content_type == TLS_HANDSHAKE_CONTENT_TYPE) {
        uint8_t tls_version = packet[1];
        if (tls_version == TLS_VERSION) {
            return true;
        }
    }
    return false;
}

KTAGResult decode_tcp(const u_char *packet, packet_info *info) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    const struct tcphdr *tcp_header =
        reinterpret_cast<const struct tcphdr *>(packet);
    info->tuple.src_port = tcp_header->th_sport;
    info->tuple.dst_port = tcp_header->th_dport;
    info->tcp_flag = tcp_header->th_flags;
    info->header_len += (tcp_header->th_off * UNIT_BYTES_FOUR);
    info->payload_len = info->packet_size - info->header_len;
    if (info->packet_size > info->header_len) {
        const u_char *payload = packet + (tcp_header->th_off * UNIT_BYTES_FOUR);
        uint32_t payload_len = info->payload_len;
        if (is_http_payload(payload, payload_len)) {
            return parse_http_payload(payload, info, payload_len);
        } else {
            if (is_tls_payload(payload, payload_len)) {
                return parse_tls(payload, info, payload_len);
            }
        }
    }
    return KTAG_STS_NO_ERR;
}

std::string parse_domain(const u_char *data, size_t &pos, size_t data_len) {
    std::string domain("");
    if (data == nullptr) {
        return domain;
    }

    if (pos >= data_len) {
        return domain;
    }

    uint8_t length = data[pos];
    int loop_count = 0;
    const int MAX_DNS_LABEL_LEN = 63;

    while (length != 0) {
        if (++loop_count > MAX_DNS_LABEL_LEN * 128) {
            return "";
        }

        if (pos + length + 1 > data_len) {
            return "";
        }

        if (length > MAX_DNS_LABEL_LEN) {
            return "";
        }

        domain +=
            std::string(reinterpret_cast<const char *>(data + pos + 1), length);
        domain += ".";
        pos += length + 1;

        if (pos >= data_len) {
            return "";
        }
        length = data[pos];
    }
    pos += 1;
    return domain;
}

static uint64_t dns_expire_at_us(const timeval &timestamp,
                                 uint32_t ttl_seconds) {
    uint64_t now_us = 0U;
    if (timestamp.tv_sec >= 0 && timestamp.tv_usec >= 0) {
        const uint64_t seconds = static_cast<uint64_t>(timestamp.tv_sec);
        const uint64_t microseconds = static_cast<uint64_t>(timestamp.tv_usec);
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

static KTAGResult parse_dns_updates_bounded(const uint8_t *dns,
                                             uint32_t dns_length,
                                             const packet_info &info,
                                             domain_ip_update_callback callback,
                                             void *user_data) {
    constexpr uint32_t DNS_HEADER_LENGTH = 12U;
    if (dns == nullptr || dns_length < DNS_HEADER_LENGTH ||
        callback == nullptr) {
        return KTAG_STS_NO_ERR;
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
            return KTAG_STS_NO_ERR;
        }
        if (question_domain.empty()) {
            question_domain = std::move(current_domain);
        }
        position += 4U;
    }
    if (question_domain.empty()) {
        return KTAG_STS_NO_ERR;
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
            const KTAGResult status =
                callback(user_data, &address, question_domain,
                         dns_expire_at_us(info.ts, ttl));
            if (status != KTAG_STS_NO_ERR) {
                return status;
            }
        }
        position += record_length;
    }
    return KTAG_STS_NO_ERR;
}

static KTAGResult best_effort_application_status(KTAGResult status) {
    if (status == KTAG_STS_DECODE_HTTP_ERROR ||
        status == KTAG_STS_DECODE_TLS_ERROR ||
        status == KTAG_STS_INVALID_PCAP_PACKET) {
        return KTAG_STS_NO_ERR;
    }
    return status;
}

static KTAGResult decode_tcp_transport_bounded(const u_char *transport,
                                                uint32_t transport_length,
                                                packet_info *info) {
    constexpr uint32_t TCP_MINIMUM_HEADER_LENGTH = 20U;
    if (transport == nullptr || info == nullptr ||
        transport_length < TCP_MINIMUM_HEADER_LENGTH) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    const uint32_t tcp_header_length =
        static_cast<uint32_t>(transport[12U] >> 4U) * 4U;
    if (tcp_header_length < TCP_MINIMUM_HEADER_LENGTH ||
        tcp_header_length > transport_length ||
        info->header_len > info->packet_size ||
        tcp_header_length > info->packet_size - info->header_len) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }

    std::memcpy(&info->tuple.src_port, transport, sizeof(info->tuple.src_port));
    std::memcpy(&info->tuple.dst_port, transport + 2U,
                sizeof(info->tuple.dst_port));
    info->tcp_flag = transport[13U];
    info->header_len += tcp_header_length;
    info->payload_len = info->packet_size - info->header_len;

    const uint32_t bounded_payload_length =
        transport_length - tcp_header_length;
    if (bounded_payload_length == 0U) {
        return KTAG_STS_NO_ERR;
    }
    const u_char *payload = transport + tcp_header_length;
    if (is_http_payload(payload, bounded_payload_length)) {
        return best_effort_application_status(
            parse_http_payload(payload, info, bounded_payload_length));
    }
    if (!is_tls_payload(payload, bounded_payload_length)) {
        return KTAG_STS_NO_ERR;
    }

    const uint16_t record_length = static_cast<uint16_t>(
        (static_cast<uint16_t>(payload[3U]) << 8U) | payload[4U]);
    if (record_length > bounded_payload_length - TLS_RECODE_MIN_LENGTH) {
        /*
         * A TLS record may span TCP segments. The modern Provider owns
         * prefix assembly, so an incomplete record is not a packet error.
         */
        return KTAG_STS_NO_ERR;
    }
    return best_effort_application_status(
        parse_tls(payload, info, bounded_payload_length));
}

static KTAGResult decode_udp_transport_bounded(
    const u_char *transport, uint32_t transport_length, packet_info *info,
    domain_ip_update_callback callback, void *user_data) {
    if (transport == nullptr || info == nullptr ||
        transport_length < static_cast<uint32_t>(UDP_HEADER_LEN)) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    const auto *udp_header = reinterpret_cast<const struct udphdr *>(transport);
    info->tuple.src_port = udp_header->uh_sport;
    info->tuple.dst_port = udp_header->uh_dport;
    info->header_len += UDP_HEADER_LEN;
    if (info->packet_size < info->header_len) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    /*
     * Keep the Legacy Built-in byte/payload statistics based on captured
     * packet size. The modern Packet View still uses the bounded IP/UDP
     * payload length supplied by the shared decoder.
     */
    info->payload_len = info->packet_size - info->header_len;
    if (udp_header->uh_sport == htons(DNS_PROTOCOL_PORT) ||
        udp_header->uh_dport == htons(DNS_PROTOCOL_PORT)) {
        return parse_dns_updates_bounded(transport + UDP_HEADER_LEN,
                                         info->payload_len, *info, callback,
                                         user_data);
    }
    return KTAG_STS_NO_ERR;
}

KTAGResult decode_transport_payload_with_domain_callback(
    const u_char *transport, uint32_t transport_length, uint8_t protocol,
    packet_info *info, domain_ip_update_callback callback, void *user_data) {
    if (transport == nullptr || info == nullptr) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    if (protocol == IPPROTO_TCP) {
        return decode_tcp_transport_bounded(transport, transport_length, info);
    }
    if (protocol == IPPROTO_UDP) {
        return decode_udp_transport_bounded(transport, transport_length, info,
                                            callback, user_data);
    }
    return KTAG_STS_NOT_SUPPORT;
}

static KTAGResult
decode_udp_with_domain_callback(const u_char *packet, packet_info *info,
                                domain_ipv4_update_callback callback,
                                void *user_data) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }

    const struct udphdr *udp_header =
        reinterpret_cast<const struct udphdr *>(packet);
    info->tuple.src_port = udp_header->uh_sport;
    info->tuple.dst_port = udp_header->uh_dport;
    info->header_len += UDP_HEADER_LEN;
    info->payload_len = info->packet_size - info->header_len;

    if (udp_header->uh_sport == htons(DNS_PROTOCOL_PORT) ||
        udp_header->uh_dport == htons(DNS_PROTOCOL_PORT)) {
        size_t dns_pos = sizeof(struct udphdr);
        if (dns_pos + sizeof(dns_header) > info->packet_size) {
            return KTAG_STS_INVALID_PCAP_PACKET;
        }
        dns_header *dns = (dns_header *)(packet + dns_pos);

        size_t question_pos = dns_pos + sizeof(dns_header);
        std::string domain =
            parse_domain(packet, question_pos, info->packet_size);

        uint16_t ancount = ntohs(dns->ancount);
        if (ancount > 1024) {
            ancount = 1024;
        }

        size_t answer_pos = question_pos + 6;
        for (uint32_t i = 0; i < ancount; i++) {
            if (answer_pos + sizeof(dns_resource_record) > info->packet_size) {
                break;
            }
            dns_resource_record *rr =
                (dns_resource_record *)(packet + answer_pos);
            uint16_t rr_len = ntohs(rr->len);
            if (answer_pos + sizeof(dns_resource_record) + rr_len >
                info->packet_size) {
                break;
            }

            answer_pos += (sizeof(dns_resource_record) + rr_len);

            if (ntohs(rr->type) == DNS_RESPOND_IPV4_TYPE) {
                if (rr_len >= sizeof(struct in_addr)) {
                    struct in_addr ipv4_addr;
                    memcpy(&ipv4_addr, rr->data, sizeof(struct in_addr));

                    if (callback != nullptr && !domain.empty()) {
                        const KTAGResult update_result = callback(
                            user_data, ipv4_addr.s_addr, domain,
                            dns_expire_at_us(info->ts, ntohl(rr->ttl)));
                        if (update_result != KTAG_STS_NO_ERR) {
                            return update_result;
                        }
                    }
                }
            } else if (ntohs(rr->type) == DNS_RESPOND_IPV6_TYPE) {
                if (rr_len >= sizeof(struct in6_addr)) {
                    struct in6_addr ipv6_addr;
                    memcpy(&ipv6_addr, rr->data, sizeof(struct in6_addr));
                    char ipv6_str[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, &ipv6_addr, ipv6_str, sizeof(ipv6_str));
                }
            }
        }
    }

    return KTAG_STS_NO_ERR;
}

static KTAGResult legacy_domain_update(void *user_data,
                                        uint32_t network_order_address,
                                        const std::string &domain,
                                        uint64_t expire_at_us) {
    auto *table = static_cast<domain_name_hash *>(user_data);
    if (table == nullptr) {
        return KTAG_STS_NULL_PTR_ERR;
    }
    /* Preserve the legacy decoder's best-effort DomainTable update behavior. */
    (void)domain_name_table_add_with_expiry(table, network_order_address,
                                            domain, expire_at_us);
    return KTAG_STS_NO_ERR;
}

KTAGResult decode_udp(const u_char *packet, packet_info *info,
                       domain_name_hash *domain_name_table) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    if (domain_name_table == nullptr) {
        return KTAG_STS_NULL_PTR_ERR;
    }
    return decode_udp_with_domain_callback(packet, info, legacy_domain_update,
                                           domain_name_table);
}

static KTAGResult
decode_ipv4_with_domain_callback(const u_char *packet, packet_info *info,
                                 domain_ipv4_update_callback callback,
                                 void *user_data) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    const struct ip *ip_header = reinterpret_cast<const struct ip *>(packet);
    info->tuple.src_ip = ip_header->ip_src.s_addr;
    info->tuple.dst_ip = ip_header->ip_dst.s_addr;
    info->tuple.protocol = ip_header->ip_p;
    info->header_len += (ip_header->ip_hl * UNIT_BYTES_FOUR);
    if (ip_header->ip_p == IPPROTO_TCP) {
        return decode_tcp(packet + (ip_header->ip_hl * UNIT_BYTES_FOUR), info);
    } else if (ip_header->ip_p == IPPROTO_UDP) {
        return decode_udp_with_domain_callback(
            packet + (ip_header->ip_hl * UNIT_BYTES_FOUR), info, callback,
            user_data);
    } else {
        return KTAG_STS_NOT_SUPPORT;
    }
    return KTAG_STS_NO_ERR;
}

KTAGResult decode_ipv4(const u_char *packet, packet_info *info,
                        domain_name_hash *domain_name_table) {
    return decode_ipv4_with_domain_callback(packet, info, legacy_domain_update,
                                            domain_name_table);
}

static KTAGResult
decode_ethernet_with_domain_callback(const u_char *packet, packet_info *info,
                                     domain_ipv4_update_callback callback,
                                     void *user_data) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    const struct ether_header *eth =
        reinterpret_cast<const struct ether_header *>(packet);
    if (ntohs(eth->ether_type) != ETHERTYPE_IP) {
        return KTAG_STS_NOT_SUPPORT;
    }
    info->header_len += ETHERNET_HEADER_LEN;
    return decode_ipv4_with_domain_callback(packet + ETHERNET_HEADER_LEN, info,
                                            callback, user_data);
}

KTAGResult decode_ethernet(const u_char *packet, packet_info *info,
                            domain_name_hash *domain_name_table) {
    return decode_ethernet_with_domain_callback(
        packet, info, legacy_domain_update, domain_name_table);
}

static KTAGResult
decode_sll_with_domain_callback(const u_char *packet, packet_info *info,
                                domain_ipv4_update_callback callback,
                                void *user_data) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    sll_header *sll_hdr = (sll_header *)packet;
    if (ntohs(sll_hdr->sll_protocol) != ETHERTYPE_IP) {
        return KTAG_STS_NOT_SUPPORT;
    }
    info->header_len += SLL_HEADER_LEN;
    return decode_ipv4_with_domain_callback(packet + SLL_HEADER_LEN, info,
                                            callback, user_data);
}

KTAGResult decode_sll(const u_char *packet, packet_info *info,
                       domain_name_hash *domain_name_table) {
    return decode_sll_with_domain_callback(packet, info, legacy_domain_update,
                                           domain_name_table);
}

KTAGResult decode_link_layer_with_domain_callback(
    const u_char *packet, packet_info *info, int link_type,
    domain_ipv4_update_callback callback, void *user_data) {
    if (!packet) {
        return KTAG_STS_INVALID_PCAP_PACKET;
    }
    KTAGResult ret;
    switch (link_type) {
    case DLT_EN10MB:
        ret = decode_ethernet_with_domain_callback(packet, info, callback,
                                                   user_data);
        break;
    case DLT_LINUX_SLL:
        ret =
            decode_sll_with_domain_callback(packet, info, callback, user_data);
        break;
    default:
        printf("The link type not in DLT_EN10MB or DLT_LINUX_SLL\n");
        ret = KTAG_STS_NOT_SUPPORT;
        break;
    }
    return ret;
}

KTAGResult decode_link_layer(const u_char *packet, packet_info *info,
                              int link_type,
                              domain_name_hash *domain_name_table) {
    return decode_link_layer_with_domain_callback(
        packet, info, link_type, legacy_domain_update, domain_name_table);
}
