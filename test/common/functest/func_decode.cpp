/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "ktag_test.h"
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/udp.h>
#include <netinet/ip.h>
#include <net/ethernet.h>
#include <pcap.h>

TEST(KTAG_DECODE, IS_HTTP_PAYLOAD_LengthSmallerThanMinLength)
{
    const u_char packet[] = "test";
    int length = HTTP_MIN_LENGTH - 1;
    bool res = is_http_payload(packet, length);
    EXPECT_EQ(res, false);
}

TEST(KTAG_DECODE, IS_HTTP_PAYLOAD_NullPacket)
{
    const u_char *packet = nullptr;
    int length = HTTP_MIN_LENGTH;
    bool res = is_http_payload(packet, length);
    EXPECT_EQ(res, false);
}

TEST(KTAG_DECODE, IS_HTTP_PAYLOAD_Basic)
{
    const u_char packet[] = "GET /index.html HTTP/1.1\r\n";
    int length = sizeof(packet) - 1;
    bool res = is_http_payload(packet, length);
    EXPECT_EQ(res, true);
}

TEST(KTAG_DECODE, IS_HTTP_PAYLOAD_Invalid)
{
    const u_char packet[] = "GE /index.html";
    int length = sizeof(packet) - 1;
    bool res = is_http_payload(packet, length);
    EXPECT_EQ(res, false);
}

TEST(KTAG_DECODE, PARSE_PAYLOAD_NullPacket)
{
    packet_info info = {};
    EXPECT_EQ(parse_http_payload(nullptr, &info, 100), KTAG_STS_INVALID_PCAP_PACKET);
}

TEST(KTAG_DECODE, PARSE_PAYLOAD_NoCRLF)
{
    const unsigned char packet[] = "GET /index.html";
    packet_info info = {};
    EXPECT_EQ(parse_http_payload(packet, &info, sizeof(packet) - 1), KTAG_STS_DECODE_HTTP_ERROR);
}

TEST(KTAG_DECODE, PARSE_PAYLOAD_NoFirstSpace)
{
    const unsigned char packet[] = "GET/index.html\r\n";
    packet_info info = {};
    EXPECT_EQ(parse_http_payload(packet, &info, sizeof(packet) - 1), KTAG_STS_DECODE_HTTP_ERROR);
}

TEST(KTAG_DECODE, PARSE_PAYLOAD_NoSecondSpace)
{
    const unsigned char packet[] = "GET /index.html\r\n";
    packet_info info = {};
    EXPECT_EQ(parse_http_payload(packet, &info, sizeof(packet) - 1), KTAG_STS_DECODE_HTTP_ERROR);
}

TEST(KTAG_DECODE, PARSE_PAYLOAD_FullURLInURI)
{
    const std::string request = "GET http://example.com/path HTTP/1.1\r\nHost: example.com\r\n\r\n";
    packet_info info = {};
    EXPECT_EQ(parse_http_payload(reinterpret_cast<const unsigned char*>(request.c_str()), &info, request.size()), KTAG_STS_NO_ERR);
    EXPECT_EQ(info.http_uri, "http://example.com/path");
    EXPECT_EQ(info.http_url, "http://example.com/path");
    EXPECT_TRUE(info.http_host.empty());
}

TEST(KTAG_DECODE, PARSE_PAYLOAD_NoHostHeader){
    const std::string request = "GET /path HTTP/1.1\r\nUser-Agent: Test\r\n\r\n";
    packet_info info = {};
    EXPECT_EQ(parse_http_payload(reinterpret_cast<const unsigned char*>(request.c_str()), &info, request.size()), KTAG_STS_NO_ERR);
    EXPECT_EQ(info.http_uri, "/path");
    EXPECT_EQ(info.http_url, "/path");
    EXPECT_TRUE(info.http_host.empty());
}

TEST(KTAG_DECODE, PARSE_PAYLOAD_HostHeaderWithSpaces)
{
    const std::string request = "GET /data HTTP/1.1\r\nHost: example.com:8080\r\n\r\n";
    packet_info info = {};
    EXPECT_EQ(parse_http_payload(reinterpret_cast<const unsigned char*>(request.c_str()), &info, request.size()), KTAG_STS_NO_ERR);
    EXPECT_EQ(info.http_host, "example.com:8080");
    EXPECT_EQ(info.http_url, HTTP_PREFIX + "example.com:8080/data");
}

TEST(KTAG_DECODE, PARSE_TLS_SNI_EmptyInput)
{
    packet_info info = {};
    std::string_view sv;
    EXPECT_EQ(parse_tls_sni(0, 0, &info, sv), KTAG_STS_INVALID_PCAP_PACKET);
}

// 辅助函数：构建TLS扩展数据
std::vector<uint8_t> build_tls_extensions(uint16_t ext_type, uint16_t ext_len, const std::string& sni = "") {
    std::vector<uint8_t> data;
    
    // 添加扩展类型和长度
    data.push_back(ext_type >> 8);
    data.push_back(ext_type & 0xFF);
    data.push_back(ext_len >> 8);
    data.push_back(ext_len & 0xFF);
    
    if (ext_type == TLS_EXTENSION_TYPE) {
        // 添加服务器名称列表长度（假设只有一个SNI）
        uint16_t server_name_list_length = 3 + sni.length(); // 类型(1) + 长度(2) + 名称长度
        data.push_back(server_name_list_length >> 8);
        data.push_back(server_name_list_length & 0xFF);
        
        // 添加服务器名称类型和长度
        data.push_back(TLS_SNI_NAME_TYPE); // 类型：主机名
        data.push_back(sni.length() >> 8);
        data.push_back(sni.length() & 0xFF);
        
        // 添加服务器名称
        data.insert(data.end(), sni.begin(), sni.end());
    }
    
    return data;
}

TEST(KTAG_DECODE, PARSE_TLS_SNI_ExtensionsExceedBuffer) {
    packet_info info = {};
    std::vector<uint8_t> data = build_tls_extensions(TLS_EXTENSION_TYPE, 10, "example.com");
    std::string_view sv(reinterpret_cast<char*>(data.data()), data.size() - 18);
    EXPECT_EQ(parse_tls_sni(0, data.size(), &info, sv), KTAG_STS_DECODE_TLS_ERROR);
}

TEST(KTAG_DECODE, PARSE_TLS_SNI_InvalidExtensionLength)
{
    packet_info info = {};
    std::vector<uint8_t> data = build_tls_extensions(TLS_EXTENSION_TYPE, 1000, "example.com"); // 超长扩展
    std::string_view sv(reinterpret_cast<char*>(data.data()), data.size());
    EXPECT_EQ(parse_tls_sni(0, 10, &info, sv), KTAG_STS_DECODE_TLS_ERROR);
}

TEST(KTAG_DECODE, PARSE_TLS_SNI_Basic) {
    packet_info info = {};
    std::vector<uint8_t> data = build_tls_extensions(TLS_EXTENSION_TYPE, 10, "example.com");
    std::string_view sv(reinterpret_cast<char*>(data.data()), data.size());
    EXPECT_EQ(parse_tls_sni(0, data.size(), &info, sv), KTAG_STS_NO_ERR);
}

// 辅助函数：构建测试用的 TLS 加密套件数据（网络字节序）
std::vector<uint8_t> build_cipher_suits_data (const std::vector<uint16_t>& suites) {
    std::vector<uint8_t> data;
    // 加密套件总长度（2 字节，网络字节序）
    uint16_t total_len = static_cast<uint16_t>(suites.size () * 2);
    data.push_back ((total_len >> 8) & 0xFF); // 高字节
    data.push_back (total_len & 0xFF); // 低字节
    // 逐个添加加密套件（每个套件 2 字节，网络字节序）
    for (uint16_t suite : suites) {
    data.push_back ((suite >> 8) & 0xFF); // 高字节
    data.push_back (suite & 0xFF); // 低字节
    }
    return data;
}

TEST (KTAG_DECODE, PARSE_TLS_CIPHER_SUITS_EmptyInput) {
    packet_info info = {};
    std::string_view sv; // 空输入
    size_t msg_start = 15;
    EXPECT_EQ (parse_tls_cipher_suits (msg_start, &info, sv), msg_start);
}

TEST (KTAG_DECODE, Parse_TLS_CIPHER_SUITS_InsufficientLengthForCipherLen) {
    packet_info info = {};
    std::vector<uint8_t> data = {0x00}; // 仅 1 字节，不足读取加密套件长度（需要 2 字节）
    std::string_view sv (reinterpret_cast<char*>(data.data ()), data.size ());
    size_t msg_start = 0;
    EXPECT_EQ(parse_tls_cipher_suits(msg_start, &info, sv), msg_start);
}

TEST (KTAG_DECODE, Parse_TLS_CIPHER_SUITS_ValidCipherSuitsParsing) {
    packet_info info = {};
    std::vector<uint16_t> suites = {0x000A, 0x000B, 0x000C}; // 测试用加密套件
    std::vector<uint8_t> data = build_cipher_suits_data (suites);
    std::string_view sv (reinterpret_cast<char*>(data.data ()), data.size ());
    size_t msg_start = 0;
    size_t expected_end = msg_start + TLS_CIPHER_SUITS_LENGTH + suites.size () * 2; // 0 + 2 + 6 = 8
    EXPECT_EQ (parse_tls_cipher_suits (msg_start, &info, sv), expected_end);
    EXPECT_EQ (info.tls_info.cipher_suites_length, suites.size () * 2); // 总长度 6 字节
    // 验证每个加密套件解析正确
    for (size_t i = 0; i < suites.size (); ++i) {
    EXPECT_EQ (info.tls_info.cipher_suites [i], suites [i]);
    }
}

TEST(KTAG_DECODE, PARSE_TLS_CLIENT_HELLO_EmptySV)
{
    packet_info info = {};
    std::string_view sv; // 空输入
    EXPECT_EQ(parse_tls_client_hello(0, 1, &info, sv), KTAG_STS_INVALID_PCAP_PACKET);
}

// 辅助函数：构建TLS Client Hello消息
std::vector<uint8_t> build_tls_client_hello(
    uint16_t client_version,
    const std::vector<uint8_t>& random_data,
    const std::vector<uint8_t>& session_id,
    const std::vector<uint16_t>& cipher_suites,
    const std::vector<uint8_t>& compression_methods,
    const std::vector<uint8_t>& extensions) {
    
    std::vector<uint8_t> data;
    
    // 消息类型 (1 byte)
    data.push_back(TLS_CLIENTHELLO_TYPE);
    
    // 消息长度 (3 bytes)
    size_t msg_length_pos = data.size();
    data.resize(data.size() + 3);  // 预留3字节用于长度字段
    
    // Client Version (2 bytes)
    data.push_back(client_version >> 8);
    data.push_back(client_version & 0xFF);
    
    // Random (32 bytes)
    data.insert(data.end(), random_data.begin(), random_data.end());
    
    // Session ID Length (1 byte)
    data.push_back(session_id.size());
    
    // Session ID
    data.insert(data.end(), session_id.begin(), session_id.end());
    
    // Cipher Suites Length (2 bytes)
    uint16_t cipher_suites_length = cipher_suites.size() * 2;
    data.push_back(cipher_suites_length >> 8);
    data.push_back(cipher_suites_length & 0xFF);
    
    // Cipher Suites
    for (uint16_t suite : cipher_suites) {
        data.push_back(suite >> 8);
        data.push_back(suite & 0xFF);
    }
    
    // Compression Methods Length (1 byte)
    data.push_back(compression_methods.size());
    
    // Compression Methods
    data.insert(data.end(), compression_methods.begin(), compression_methods.end());
    
    // Extensions Length (2 bytes)
    uint16_t extensions_length = extensions.size();
    data.push_back(extensions_length >> 8);
    data.push_back(extensions_length & 0xFF);
    
    // Extensions
    data.insert(data.end(), extensions.begin(), extensions.end());
    
    // 计算并填充消息长度
    size_t msg_length = data.size() - msg_length_pos - 3;
    data[msg_length_pos] = (msg_length >> 16) & 0xFF;
    data[msg_length_pos + 1] = (msg_length >> 8) & 0xFF;
    data[msg_length_pos + 2] = msg_length & 0xFF;
    
    return data;
}

// 辅助函数：构建包含SNI扩展的数据
std::vector<uint8_t> build_sni_extension(const std::string& hostname) {
    std::vector<uint8_t> data;
    
    // Extension Type: Server Name Indication (0x0000)
    data.push_back(0x00);
    data.push_back(0x00);
    
    // Extension Length (2 bytes)
    size_t ext_length_pos = data.size();
    data.resize(data.size() + 2);  // 预留2字节用于长度字段
    
    // Server Name List Length (2 bytes)
    uint16_t server_name_list_length = 3 + hostname.length();  // 1字节类型 + 2字节长度 + 域名长度
    data.push_back(server_name_list_length >> 8);
    data.push_back(server_name_list_length & 0xFF);
    
    // Server Name Type (1 byte): host_name (0x00)
    data.push_back(0x00);
    
    // Server Name Length (2 bytes)
    data.push_back(hostname.length() >> 8);
    data.push_back(hostname.length() & 0xFF);
    
    // Server Name (域名)
    data.insert(data.end(), hostname.begin(), hostname.end());
    
    // 填充扩展长度
    uint16_t ext_length = data.size() - ext_length_pos - 2;
    data[ext_length_pos] = ext_length >> 8;
    data[ext_length_pos + 1] = ext_length & 0xFF;
    
    return data;
}

TEST(KTAG_DECODE, InsufficientLengthForClientLength) {
    packet_info info = {};
    std::vector<uint8_t> data = {TLS_CLIENTHELLO_TYPE, 0x00, 0x00};  // 仅3字节（类型+不完整的长度）
    std::string_view sv(reinterpret_cast<char*>(data.data()), data.size());
    EXPECT_EQ(parse_tls_client_hello(0, data.size(), &info, sv), KTAG_STS_DECODE_TLS_ERROR);
}

TEST(KTAG_DECODE, InvalidClientVersion) {
    packet_info info = {};
    std::vector<uint8_t> random(32, 0xAA);
    std::vector<uint8_t> session_id;
    std::vector<uint16_t> cipher_suites = {0x002F};
    std::vector<uint8_t> compression_methods = {0x00};
    std::vector<uint8_t> extensions;
    
    // 构建带有无效版本(0x0000)的Client Hello
    std::vector<uint8_t> data = build_tls_client_hello(
        0x0000, random, session_id, cipher_suites, compression_methods, extensions);
    
    std::string_view sv(reinterpret_cast<char*>(data.data()), data.size());
    EXPECT_EQ(parse_tls_client_hello(0, data.size(), &info, sv), KTAG_STS_DECODE_TLS_ERROR);
    EXPECT_EQ(parse_tls_client_hello(1,0, &info, sv), KTAG_STS_DECODE_TLS_ERROR);
}

TEST(KTAG_DECODE, ValidClientHelloWithSNI) {
    packet_info info = {};
    std::vector<uint8_t> random(32, 0xAA);
    std::vector<uint8_t> session_id = {0x01, 0x02, 0x03};
    std::vector<uint16_t> cipher_suites = {0x002F, 0x0035};
    std::vector<uint8_t> compression_methods = {0x00};
    std::string sni_hostname = "example.com";
    std::vector<uint8_t> extensions = build_sni_extension(sni_hostname);
    
    // 构建有效的Client Hello
    std::vector<uint8_t> data = build_tls_client_hello(
        0x0303, random, session_id, cipher_suites, compression_methods, extensions);
    
    std::string_view sv(reinterpret_cast<char*>(data.data()), data.size());
    EXPECT_EQ(parse_tls_client_hello(0, data.size(), &info, sv), KTAG_STS_DECODE_TLS_ERROR);
}

TEST (KTAG_DECODE, PARSE_TLS_NullDataInput) {
    packet_info info = {};
    EXPECT_EQ (parse_tls (nullptr, &info, 5), KTAG_STS_INVALID_PCAP_PACKET);
}

class DecodeTcpTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 初始化TCP头部
        tcp_hdr = {};
        tcp_hdr.th_off = 5; // 标准TCP头部长度 (5 * 4 = 20字节)
        
        // 初始化数据包信息
        info.header_len = 0;
        info.payload_len = 0;
        info.packet_size = sizeof(tcp_hdr) + 100; // 头部 + 100字节载荷
    }

    // 创建完整的数据包
    std::vector<u_char> create_packet(bool include_payload = true) {
        std::vector<u_char> packet(sizeof(tcp_hdr));
        memcpy(packet.data(), &tcp_hdr, sizeof(tcp_hdr));
        
        if (include_payload) {
            packet.resize(info.packet_size);
            // 添加模拟载荷
            for (size_t i = sizeof(tcp_hdr); i < packet.size(); i++) {
                packet[i] = 'A' + (i % 26);
            }
        }
        return packet;
    }

    tcphdr tcp_hdr;
    packet_info info;
};

TEST_F(DecodeTcpTest, NullPacket) {
    auto result = decode_tcp(nullptr, &info);
    EXPECT_EQ(result, KTAG_STS_INVALID_PCAP_PACKET);
}

TEST_F(DecodeTcpTest, BasicTcpDecoding) {
    tcp_hdr.th_sport = htons(12345);
    tcp_hdr.th_dport = htons(80);
    tcp_hdr.th_flags = TH_SYN;
    
    auto packet = create_packet();
    auto result = decode_tcp(packet.data(), &info);
    
    EXPECT_EQ(result, KTAG_STS_NO_ERR);
    EXPECT_EQ(info.tuple.src_port, htons(12345));
    EXPECT_EQ(info.tuple.dst_port, htons(80));
    EXPECT_EQ(info.tcp_flag, TH_SYN);
    EXPECT_EQ((int)info.header_len, tcp_hdr.th_off * UNIT_BYTES_FOUR);
    EXPECT_EQ(info.payload_len, info.packet_size - info.header_len);
}

TEST (KTAG_DECODE, ParseDomain_NullDataReturnsEmptyString) {
    const u_char *data = nullptr; // 空指针输入
    size_t data_len = 0;
    size_t pos = 0;
    std::string result = parse_domain(data, pos, data_len);
    // 验证返回空字符串，pos 不变
    EXPECT_EQ(result, "");
    EXPECT_EQ (pos, (size_t)0);
}

TEST (KTAG_DECODE, ParseDomain_ValidDomainWithoutCompression) {
    // 域名格式：[7] example [3] com [0] → "example.com."
    uint8_t data [] = {
    7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', // "example"（长度 7）
    3, 'c', 'o', 'm', // "com"（长度 3）
    0 // 结束标志
    };
    size_t data_len = sizeof(data);
    size_t pos = 0; // 初始位置：数据起始处
    std::string result = parse_domain(data, pos, data_len);
    // 验证解析结果和 pos 移动距离
    EXPECT_EQ (result, "example.com."); // 注意末尾有 '.'
    EXPECT_EQ (pos, sizeof (data)); //pos 应移动到数据末尾（7+1 + 3+1 + 1 = 13 字节）
}

class DecodeUdpTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 初始化UDP头部
        udp_hdr = {};
        
        // 初始化DNS头部
        dns_hdr = {};
        dns_hdr.ancount = htons(0);
        
        // 初始化数据包信息
        info.header_len = 0;
        info.payload_len = 0;
        info.packet_size = sizeof(udp_hdr) + sizeof(dns_hdr);
        
        // 初始化域名表
        domain_table = new domain_name_hash();
    }
    
    void TearDown() override {
        delete domain_table;
    }

    // 创建DNS响应包
    std::vector<u_char> create_dns_packet(
        uint16_t ancount = 0,
        uint16_t type = DNS_RESPOND_IPV4_TYPE,
        const std::string& domain = "example.com"
    ) {
        std::vector<u_char> packet;
        
        // UDP头部
        packet.resize(sizeof(udp_hdr));
        memcpy(packet.data(), &udp_hdr, sizeof(udp_hdr));
        
        // DNS头部
        dns_hdr.ancount = htons(ancount);
        size_t dns_pos = packet.size();
        packet.resize(packet.size() + sizeof(dns_hdr));
        memcpy(packet.data() + dns_pos, &dns_hdr, sizeof(dns_hdr));
        
        // 问题部分 (域名)
        size_t q_pos = packet.size();
        std::string qname;
        size_t start = 0, end;
        while ((end = domain.find('.', start)) != std::string::npos) {
            uint8_t len = end - start;
            packet.push_back(len);
            packet.insert(packet.end(), domain.begin() + start, domain.begin() + end);
            start = end + 1;
        }
        uint8_t len = domain.size() - start;
        packet.push_back(len);
        packet.insert(packet.end(), domain.begin() + start, domain.end());
        packet.push_back(0); // 结束标记
        
        // 问题类型和类 (IN, A)
        packet.push_back(0); packet.push_back(type); // QTYPE
        packet.push_back(0); packet.push_back(1);    // QCLASS (IN)
        
        // 回答部分
        for (int i = 0; i < ancount; i++) {
            // 资源记录头 (使用指针压缩: 0xC0 | offset)
            packet.push_back(0xC0);
            packet.push_back(static_cast<uint8_t>(q_pos));
            
            // 资源记录类型和类
            uint16_t rr_type = htons(type);
            packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_type), 
                         reinterpret_cast<u_char*>(&rr_type) + 2);
            
            uint16_t rr_class = htons(1); // IN
            packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_class), 
                         reinterpret_cast<u_char*>(&rr_class) + 2);
            
            uint32_t rr_ttl = htonl(300);
            packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_ttl), 
                         reinterpret_cast<u_char*>(&rr_ttl) + 4);
            
            // 数据长度和数据
            if (type == DNS_RESPOND_IPV4_TYPE) {
                uint16_t rr_len = htons(4);
                packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_len), 
                             reinterpret_cast<u_char*>(&rr_len) + 2);
                
                uint32_t ip = htonl(0x0A000001); // 10.0.0.1
                packet.insert(packet.end(), reinterpret_cast<u_char*>(&ip), 
                             reinterpret_cast<u_char*>(&ip) + 4);
            } else if (type == DNS_RESPOND_IPV6_TYPE) {
                uint16_t rr_len = htons(16);
                packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_len), 
                             reinterpret_cast<u_char*>(&rr_len) + 2);
                
                const u_char ipv6[16] = {0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x01};
                packet.insert(packet.end(), ipv6, ipv6 + 16);
            }
        }
        
        info.packet_size = packet.size();
        return packet;
    }

    std::vector<u_char> create_dns_packet_IPV6(
        uint16_t ancount = 0,
        uint16_t type = DNS_RESPOND_IPV6_TYPE,
        const std::string& domain = "example.com"
    ) {
        std::vector<u_char> packet;
        
        // UDP头部
        packet.resize(sizeof(udp_hdr));
        memcpy(packet.data(), &udp_hdr, sizeof(udp_hdr));
        
        // DNS头部
        dns_hdr.ancount = htons(ancount);
        size_t dns_pos = packet.size();
        packet.resize(packet.size() + sizeof(dns_hdr));
        memcpy(packet.data() + dns_pos, &dns_hdr, sizeof(dns_hdr));
        
        // 问题部分 (域名)
        size_t q_pos = packet.size();
        std::string qname;
        size_t start = 0, end;
        while ((end = domain.find('.', start)) != std::string::npos) {
            uint8_t len = end - start;
            packet.push_back(len);
            packet.insert(packet.end(), domain.begin() + start, domain.begin() + end);
            start = end + 1;
        }
        uint8_t len = domain.size() - start;
        packet.push_back(len);
        packet.insert(packet.end(), domain.begin() + start, domain.end());
        packet.push_back(0); // 结束标记
        
        // 问题类型和类 (IN, A)
        packet.push_back(0); packet.push_back(type); // QTYPE
        packet.push_back(0); packet.push_back(1);    // QCLASS (IN)
        
        // 回答部分
        for (int i = 0; i < ancount; i++) {
            // 资源记录头 (使用指针压缩: 0xC0 | offset)
            packet.push_back(0xC0);
            packet.push_back(static_cast<uint8_t>(q_pos));
            
            // 资源记录类型和类
            uint16_t rr_type = htons(type);
            packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_type), 
                         reinterpret_cast<u_char*>(&rr_type) + 2);
            
            uint16_t rr_class = htons(1); // IN
            packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_class), 
                         reinterpret_cast<u_char*>(&rr_class) + 2);
            
            uint32_t rr_ttl = htonl(300);
            packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_ttl), 
                         reinterpret_cast<u_char*>(&rr_ttl) + 4);
            
            // 数据长度和数据
            if (type == DNS_RESPOND_IPV4_TYPE) {
                uint16_t rr_len = htons(4);
                packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_len), 
                             reinterpret_cast<u_char*>(&rr_len) + 2);
                
                uint32_t ip = htonl(0x0A000001); // 10.0.0.1
                packet.insert(packet.end(), reinterpret_cast<u_char*>(&ip), 
                             reinterpret_cast<u_char*>(&ip) + 4);
            } else if (type == DNS_RESPOND_IPV6_TYPE) {
                uint16_t rr_len = htons(16);
                packet.insert(packet.end(), reinterpret_cast<u_char*>(&rr_len), 
                             reinterpret_cast<u_char*>(&rr_len) + 2);
                
                const u_char ipv6[16] = {0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x01};
                packet.insert(packet.end(), ipv6, ipv6 + 16);
            }
        }
        
        info.packet_size = packet.size();
        return packet;
    }

    udphdr udp_hdr;
    dns_header dns_hdr;
    packet_info info;
    domain_name_hash* domain_table;
};

TEST_F(DecodeUdpTest,DecodeUdp_NullPacket) {
    auto result = decode_udp(nullptr, &info, domain_table);
    EXPECT_EQ(result, KTAG_STS_INVALID_PCAP_PACKET);
}

TEST_F(DecodeUdpTest, DecodeUdp_NullDomainTable) {
    std::vector<u_char> packet(100); // 任意有效数据
    auto result = decode_udp(packet.data(), &info, nullptr);
    EXPECT_EQ(result, KTAG_STS_NULL_PTR_ERR);
}

TEST_F(DecodeUdpTest, DecodeUdp_BasicUdpDecoding) {
    udp_hdr.uh_sport = htons(12345);
    udp_hdr.uh_dport = htons(80);
    
    std::vector<u_char> packet(sizeof(udp_hdr));
    memcpy(packet.data(), &udp_hdr, sizeof(udp_hdr));
    info.packet_size = packet.size();
    
    auto result = decode_udp(packet.data(), &info, domain_table);
    
    EXPECT_EQ(result, KTAG_STS_NO_ERR);
    EXPECT_EQ(info.tuple.src_port, htons(12345));
    EXPECT_EQ(info.tuple.dst_port, htons(80));
    EXPECT_EQ((int)info.header_len, UDP_HEADER_LEN);
    EXPECT_EQ(info.payload_len, 0);
}

TEST_F(DecodeUdpTest, DecodeUdp_DnsPacketIPV6) {
    udp_hdr.uh_sport = htons(DNS_PROTOCOL_PORT);
    udp_hdr.uh_dport = htons(12345);
    
    auto packet = create_dns_packet_IPV6(100);
    auto result = decode_udp(packet.data(), &info, domain_table);
    
    EXPECT_EQ(result, KTAG_STS_NO_ERR);
}

TEST_F(DecodeUdpTest, DecodeUdp_DnsPacketNoAnswers) {
    udp_hdr.uh_sport = htons(DNS_PROTOCOL_PORT);
    udp_hdr.uh_dport = htons(12345);
    
    auto packet = create_dns_packet(0);
    auto result = decode_udp(packet.data(), &info, domain_table);
    
    EXPECT_EQ(result, KTAG_STS_NO_ERR);
}

TEST(KTAG_DECODE, DecodeEthernet_NullPacket) {
    packet_info info = {};
    domain_name_hash table;
    EXPECT_EQ(decode_ethernet(nullptr, &info, &table), KTAG_STS_INVALID_PCAP_PACKET);
}

TEST(KTAG_DECODE, DecodeLinkLayer_NullPacket)
{
    packet_info info = {};
    domain_name_hash table;
    EXPECT_EQ(decode_link_layer(nullptr, &info, DLT_EN10MB, &table), KTAG_STS_INVALID_PCAP_PACKET);
}

TEST(KTAG_DECODE, DecodeLinkLayer_DefualtLinkType)
{
    pcap_info p_info = get_packet();
    packet_test *p_test = p_info.packet_ret;
    KTAGResult res = decode_link_layer(p_test->packet, p_test->info, 100, p_test->domain_name_table);
    EXPECT_EQ(res, KTAG_STS_NOT_SUPPORT);
}


TEST(KTAG_DECODE, DecodeSll_NullPacket)
{
    packet_info info = {};
    domain_name_hash table;
    EXPECT_EQ(decode_sll(nullptr, &info, &table), KTAG_STS_INVALID_PCAP_PACKET);
}

TEST(KTAG_DECODE, DecodeSll_NotIPV4)
{
    pcap_info p_info = get_packet();
    packet_test *p_test = p_info.packet_ret;
    EXPECT_EQ(KTAG_STS_NOT_SUPPORT, decode_sll(p_test->packet, p_test->info, p_test->domain_name_table));
}

TEST(KTAG_DECODE, DecodeIpv4_NullPacket)
{
    pcap_info p_info = get_packet();
    packet_test *p_test = p_info.packet_ret;
    EXPECT_EQ(KTAG_STS_INVALID_PCAP_PACKET, decode_ipv4(nullptr, p_test->info, p_test->domain_name_table));
}

TEST(KTAG_DECODE, DecodeSll_NotSupport)
{
    pcap_info p_info = get_packet();
    packet_test *p_test = p_info.packet_ret;
    sll_header *sll_hdr = new sll_header;
    sll_hdr->sll_protocol = htons(ETHERTYPE_IP);
    std::cout << "sll_protocol: " << ntohs(sll_hdr->sll_protocol) << std::endl;
    const u_char *packet = reinterpret_cast<const u_char *>(sll_hdr);
    EXPECT_EQ(KTAG_STS_NOT_SUPPORT, decode_sll(packet, p_test->info, p_test->domain_name_table));
}

TEST(KTAG_DECODE, DecodeLinkLayer_caseSLL)
{
    pcap_info p_info = get_packet();
    packet_test *p_test = p_info.packet_ret;
    p_test->link_type = DLT_LINUX_SLL;
    EXPECT_EQ(KTAG_STS_NOT_SUPPORT, decode_link_layer(p_test->packet, p_test->info, p_test->link_type, p_test->domain_name_table));
}