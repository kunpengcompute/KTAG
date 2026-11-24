/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Megascan is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 */
#include "decode.h"
#include <cstdio>
#include <cstring>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <unordered_map>

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

bool is_http_payload(const u_char *packet, int length)
{
if (length < HTTP_MIN_LENGTH || !packet) {
        return false;
    }

    for (int i = 0; i < HTTP_METHOD_NUMS; i++) {
        if (strncmp(reinterpret_cast<const char *>(packet), http_methods[i], strlen(http_methods[i])) == 0) {
            return true;
        }
    }
    return false;
}

KDADKResult parse_http_payload(const u_char *packet, packet_info *info, uint32_t length)
{
    if (!packet) {
        return KDADK_STS_INVALID_PCAP_PACKET;
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
        return KDADK_STS_DECODE_HTTP_ERROR;
    }
    // Extract URL in the request line
    size_t first_space = sv.find(' ');
    if (first_space == std::string_view::npos) {
        return KDADK_STS_DECODE_HTTP_ERROR;
    }
    url_start = first_space + 1;
    url_end = sv.find(' ', url_start);
    if (url_end == std::string_view::npos) {
        return KDADK_STS_DECODE_HTTP_ERROR;
    }

    uri = sv.substr(url_start, url_end - url_start);
    info->http_uri = uri;
    if (uri.compare(0, HTTP_PREFIX_LENGTH, HTTP_PREFIX) == 0) {
        info->http_url = uri;
        return KDADK_STS_NO_ERR;
    }

    // Concatenate host and uri
    host_start = sv.find(HTTP_PROPERTY_HOST, url_end);
    if (host_start == std::string_view::npos) {
        info->http_url = uri;
        return KDADK_STS_NO_ERR;
    }
    host_start += HTTP_PROPERTY_HOST_LENGTH;  // "Host:"
    while (host_start < sv.size() && sv[host_start] == ' ') {
        host_start++;
    }
    host_end = sv.find("\r\n", host_start);
    if (host_end == std::string_view::npos) {
        info->http_url = info->http_uri;
        return KDADK_STS_DECODE_HTTP_ERROR;
    }
    host = sv.substr(host_start, host_end - host_start);
    info->http_host = host;
    info->http_url = HTTP_PREFIX + host + uri;
    return KDADK_STS_NO_ERR;
}

KDADKResult parse_tls_sni(
    const size_t msg_body_start, const size_t extensions_len, packet_info *info, const std::string_view &sv)
{
    if (sv.empty()) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }
    size_t extensions_cur = msg_body_start;
    size_t extensions_end = extensions_cur + extensions_len;

    if (extensions_cur + TLS_EXTENSION_TYPE_AND_LENGTH >= sv.size()) {
        return KDADK_STS_DECODE_TLS_ERROR;
    }
    uint16_t ext_type = ntohs(*((uint16_t *)(sv.data() + extensions_cur)));
    uint16_t ext_len = ntohs(*((uint16_t *)(sv.data() + extensions_cur + 2)));
    extensions_cur += TLS_EXTENSION_TYPE_AND_LENGTH;
    if (extensions_cur + ext_len >= extensions_end) {
        return KDADK_STS_DECODE_TLS_ERROR;
    }
    if (ext_type == TLS_EXTENSION_TYPE) {
        if (extensions_cur + TLS_EXTENSION_SERVER_NAME_LIST_LENGTH + TLS_EXTENSION_SERVER_NAME_TYPE_AND_LENGTH < extensions_end) {
            extensions_cur += TLS_EXTENSION_SERVER_NAME_LIST_LENGTH;
            size_t sni_start = extensions_cur;
            if (sni_start >= sv.size()) {
                return KDADK_STS_DECODE_TLS_ERROR;
            }
            uint8_t name_type = sv[sni_start];
            if (sni_start + 1 + sizeof(uint16_t) >= sv.size()) {
                return KDADK_STS_DECODE_TLS_ERROR;
            }
            uint16_t name_len = ntohs(*((uint16_t *)(sv.data() + sni_start + 1)));
            sni_start += TLS_EXTENSION_SERVER_NAME_TYPE_AND_LENGTH;

            if (name_type == TLS_SNI_NAME_TYPE && name_len > 0) {
                if (sni_start + name_len > sv.size()) {
                    return KDADK_STS_DECODE_TLS_ERROR;
                }
                std::string sni_str(sv.data() + sni_start, name_len);
                info->tls_info.sni = sni_str;
            }
        }
    }

    return KDADK_STS_NO_ERR;
}

size_t parse_tls_cipher_suits(size_t msg_start, packet_info *info, const std::string_view &sv)
{
    if (sv.empty()) {
        return msg_start;
    }
    size_t msg_body_start = msg_start;
    if (msg_body_start + TLS_CIPHER_SUITS_LENGTH >= sv.size()) {
        return msg_start;
    }
    uint16_t cipher_suites_len = ntohs(*(uint16_t *)(sv.data() + msg_body_start));
    if (cipher_suites_len > sv.size() - msg_body_start - TLS_CIPHER_SUITS_LENGTH) {
        cipher_suites_len = sv.size() - msg_body_start - TLS_CIPHER_SUITS_LENGTH;
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

KDADKResult parse_tls_client_hello(
    size_t handshake_start, size_t handshake_end, packet_info *info, const std::string_view &sv)
{
    if (sv.empty()) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }

    size_t msg_body_start = handshake_start + 1;

    // client length 占3byte
    if (msg_body_start + TLS_CLIENT_LENGTH >= sv.size()) {
        return KDADK_STS_DECODE_TLS_ERROR;
    }
    size_t handshake_len_byte0 = sv[msg_body_start + BYTE_OFFSET_0] << SHIFT_16;
    size_t handshake_len_byte1 = sv[msg_body_start + BYTE_OFFSET_1] << SHIFT_8;
    size_t handshake_len_byte2 = sv[msg_body_start + BYTE_OFFSET_2];
    size_t handshake_len = handshake_len_byte0 | handshake_len_byte1 | handshake_len_byte2;
    // 至少需要 38 字节(client_length(3) + client_version(2) + random(32) + session_id_len(1))
    if (handshake_len < (TLS_CLIENT_VERSION_RANDOM_LENGTH + TLS_CLIENT_LENGTH + 1)) {
        return KDADK_STS_DECODE_TLS_ERROR;
    }
    msg_body_start += TLS_CLIENT_LENGTH;
    uint16_t client_version = ntohs(*(uint16_t *)(sv.data() + msg_body_start));
    info->tls_info.client_version = client_version;

    msg_body_start += TLS_CLIENT_VERSION_RANDOM_LENGTH;  // client_version(2) + random(32)
    uint8_t session_id_len = sv[msg_body_start];
    msg_body_start += 1 + session_id_len;  // session_id_len(1)  + session_id(session_id_len)

    msg_body_start = parse_tls_cipher_suits(msg_body_start, info, sv);
    // 跳过 compression_methods
    if (msg_body_start >= handshake_end) {
        return KDADK_STS_DECODE_TLS_ERROR;
    }
    uint8_t compression_methods_len = sv[msg_body_start];
    msg_body_start += 1 + compression_methods_len;

    // 解析 extensions
    uint16_t extensions_len = ntohs(*(uint16_t *)(sv.data() + msg_body_start));
    info->tls_info.extensions_length = extensions_len;
    msg_body_start += TLS_EXTENSIONS_LENGTH;
    if (msg_body_start + extensions_len > handshake_end) {
        return KDADK_STS_DECODE_TLS_ERROR;
    }

    return parse_tls_sni(msg_body_start, extensions_len, info, sv);
}

KDADKResult parse_tls(const u_char *data, packet_info *info, uint32_t len)
{
    if (!data) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }

    std::string_view sv((char *)data, len);
    if (sv.size() < TLS_RECODE_MIN_LENGTH + sizeof(uint16_t)) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }
    size_t handshake_start = TLS_RECODE_MIN_LENGTH;
    uint16_t record_len = ntohs(*(uint16_t *)(sv.data() + 3));
    if (handshake_start + record_len > sv.size()) {
        record_len = sv.size() - handshake_start;
    }
    size_t handshake_end = handshake_start + record_len;
    if (handshake_end <= sv.size() && handshake_end > handshake_start) {
        uint8_t msg_type = sv[handshake_start];
        if (msg_type == TLS_CLIENTHELLO_TYPE) {  // ClientHello 消息
            return parse_tls_client_hello(handshake_start, handshake_end, info, sv);
        }
    }

    return KDADK_STS_NO_ERR;
}

bool is_tls_payload(const u_char *packet, int length)
{
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

KDADKResult decode_tcp(const u_char *packet, packet_info *info)
{
    if (!packet) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }
    const struct tcphdr *tcp_header = reinterpret_cast<const struct tcphdr *>(packet);
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
    return KDADK_STS_NO_ERR;
}

std::string parse_domain(const u_char *data, size_t &pos, size_t data_len)
{
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

        domain += std::string(reinterpret_cast<const char *>(data + pos + 1), length);
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

KDADKResult decode_udp(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table)
{
    if (!packet) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }

    if (domain_name_table == nullptr) {
        return KDADK_STS_NULL_PTR_ERR;
    }

    const struct udphdr *udp_header = reinterpret_cast<const struct udphdr *>(packet);
    info->tuple.src_port = udp_header->uh_sport;
    info->tuple.dst_port = udp_header->uh_dport;
    info->header_len += UDP_HEADER_LEN;
    info->payload_len = info->packet_size - info->header_len;

    if (udp_header->uh_sport == htons(DNS_PROTOCOL_PORT) || udp_header->uh_dport == htons(DNS_PROTOCOL_PORT)) {
        size_t dns_pos = sizeof(struct udphdr);
        if (dns_pos + sizeof(dns_header) > info->packet_size) {
            return KDADK_STS_INVALID_PCAP_PACKET;
        }
        dns_header *dns = (dns_header *)(packet + dns_pos);

        size_t question_pos = dns_pos + sizeof(dns_header);
        std::string domain = parse_domain(packet, question_pos, info->packet_size);

        uint16_t ancount = ntohs(dns->ancount);
        if (ancount > 1024) {
            ancount = 1024;
        }

        size_t answer_pos = question_pos + 6;
        for (uint32_t i = 0; i < ancount; i++) {
            if (answer_pos + sizeof(dns_resource_record) > info->packet_size) {
                break;
            }
            dns_resource_record *rr = (dns_resource_record *)(packet + answer_pos);
            uint16_t rr_len = ntohs(rr->len);
            if (answer_pos + sizeof(dns_resource_record) + rr_len > info->packet_size) {
                break;
            }

            answer_pos += (sizeof(dns_resource_record) + rr_len);

            if (ntohs(rr->type) == DNS_RESPOND_IPV4_TYPE) {
                if (rr_len >= sizeof(struct in_addr)) {
                    struct in_addr ipv4_addr;
                    memcpy(&ipv4_addr, rr->data, sizeof(struct in_addr));

                    domain_name_table_add(domain_name_table, ipv4_addr.s_addr, domain);
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

    return KDADK_STS_NO_ERR;
}

KDADKResult decode_ipv4(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table)
{
    if (!packet) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }
    const struct ip *ip_header = reinterpret_cast<const struct ip *>(packet);
    info->tuple.src_ip = ip_header->ip_src.s_addr;
    info->tuple.dst_ip = ip_header->ip_dst.s_addr;
    info->tuple.protocol = ip_header->ip_p;
    info->header_len += (ip_header->ip_hl * UNIT_BYTES_FOUR);
    if (ip_header->ip_p == IPPROTO_TCP) {
        return decode_tcp(packet + (ip_header->ip_hl * UNIT_BYTES_FOUR), info);
    } else if (ip_header->ip_p == IPPROTO_UDP) {
        return decode_udp(packet + (ip_header->ip_hl * UNIT_BYTES_FOUR), info, domain_name_table);
    } else {
        return KDADK_STS_NOT_SUPPORT;
    }
    return KDADK_STS_NO_ERR;
}

KDADKResult decode_ethernet(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table)
{
    if (!packet) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }
    const struct ether_header *eth = reinterpret_cast<const struct ether_header *>(packet);
    if (ntohs(eth->ether_type) != ETHERTYPE_IP) {
        return KDADK_STS_NOT_SUPPORT;
    }
    info->header_len += ETHERNET_HEADER_LEN;
    return decode_ipv4(packet + ETHERNET_HEADER_LEN, info, domain_name_table);
}

KDADKResult decode_sll(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table)
{
    if (!packet) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }
    sll_header *sll_hdr = (sll_header *)packet;
    if (ntohs(sll_hdr->sll_protocol) != ETHERTYPE_IP) {
        return KDADK_STS_NOT_SUPPORT;
    }
    info->header_len += SLL_HEADER_LEN;
    return decode_ipv4(packet + SLL_HEADER_LEN, info, domain_name_table);
}

KDADKResult decode_link_layer(
    const u_char *packet, packet_info *info, int link_type, domain_name_hash *domain_name_table)
{
    if (!packet) {
        return KDADK_STS_INVALID_PCAP_PACKET;
    }
    KDADKResult ret;
    switch (link_type) {
        case DLT_EN10MB:
            ret = decode_ethernet(packet, info, domain_name_table);
            break;
        case DLT_LINUX_SLL:
            ret = decode_sll(packet, info, domain_name_table);
            break;
        default:
            printf("The link type not in DLT_EN10MB or DLT_LINUX_SLL\n");
            ret = KDADK_STS_NOT_SUPPORT;
            break;
    }
    return ret;
}

