/*
 * Copyright (c) 2025-2025 Huawei Technologies Co., Ltd.
 */
#ifndef DECODE_H
#define DECODE_H
#include <stdint.h>
#include <unordered_map>
#include "kdadk.h"
#include "domain_name_hash.h"
#define SLL_HEADER_LEN 16

typedef struct {
    uint16_t sll_pkttype;  /* packet type */
    uint16_t sll_hatype;   /* link-layer address type */
    uint16_t sll_halen;    /* link-layer address length */
    uint8_t sll_addr[8];   /* link-layer address */
    uint16_t sll_protocol; /* protocol */
} sll_header;

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
};

struct dns_resource_record {
    uint16_t type;
    uint16_t cls;
    uint32_t ttl;
    uint16_t len;
    uint8_t data[1];
};

extern const int ETHERNET_HEADER_LEN;
extern const int UDP_HEADER_LEN;

extern const std::string HTTP_PREFIX;
extern const int HTTP_PREFIX_LENGTH;

extern const std::string HTTP_PROPERTY_HOST;
extern const int HTTP_PROPERTY_HOST_LENGTH;

extern const int HTTP_MIN_LENGTH; /* Min Length "GET / " */
extern const int HTTP_METHOD_NUMS;
extern const char *http_methods[];

extern const int UNIT_BYTES_FOUR;

extern const int DNS_PROTOCOL_PORT;
extern const int DNS_RESPOND_IPV4_TYPE;
extern const int DNS_RESPOND_IPV6_TYPE;

constexpr int TLS_EXTENSION_TYPE = 0x0000;
constexpr int TLS_EXTENSION_TYPE_AND_LENGTH = 4;
constexpr int TLS_EXTENSION_SERVER_NAME_LIST_LENGTH = 2;
constexpr int TLS_EXTENSION_SERVER_NAME_TYPE_AND_LENGTH = 3;
constexpr int TLS_SNI_NAME_TYPE = 0x00;
constexpr int TLS_CIPHER_SUITS_LENGTH = 2;
constexpr int TLS_CLIENT_LENGTH = 3;
constexpr int TLS_VERSION = 0x03;
constexpr int TLS_CLIENT_VERSION_RANDOM_LENGTH = 34;
constexpr int TLS_EXTENSIONS_LENGTH = 2;
constexpr int TLS_CLIENTHELLO_TYPE = 0x01;
constexpr int TLS_RECODE_MIN_LENGTH = 5;
constexpr int TLS_PORT = 443;
constexpr int TLS_MIN_LENGTH = 5;
constexpr int TLS_HANDSHAKE_CONTENT_TYPE = 22;

extern const int BYTE_OFFSET_0;
extern const int BYTE_OFFSET_1;
extern const int BYTE_OFFSET_2;
extern const int SHIFT_16;
extern const int SHIFT_8;

KDADKResult decode_link_layer(const u_char *packet, packet_info *info, int link_type,
                              domain_name_hash *domain_name_table);
bool is_http_payload(const u_char *packet, int length);
KDADKResult parse_http_payload(const u_char *packet, packet_info *info, uint32_t length);
KDADKResult parse_tls_sni(const size_t msg_body_start, const size_t extensions_len, packet_info *info,
                          const std::string_view &sv);
size_t parse_tls_cipher_suits(size_t msg_start, packet_info *info, const std::string_view &sv);
KDADKResult parse_tls_client_hello(size_t handshake_start, size_t handshake_end, packet_info *info,
                                   const std::string_view &sv);
KDADKResult parse_tls(const u_char *data, packet_info *info, uint32_t len);
KDADKResult decode_tcp(const u_char *packet, packet_info *info);
std::string parse_domain(const u_char *data, size_t &pos, size_t data_len);
KDADKResult decode_udp(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table);
KDADKResult decode_ipv4(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table);
KDADKResult decode_ethernet(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table);
KDADKResult decode_sll(const u_char *packet, packet_info *info, domain_name_hash *domain_name_table);

#endif
