/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "kdadk_test.h"

const int MAX_TOKEN_LENGTH = 20;

TEST(KDADK_FLOW_HASH, HashTableCreateNullPacket)
{
    EXPECT_EQ(hash_table_create(nullptr, 1), KDADK_STS_PARAMETER_ERR);
}

TEST(KDADK_FLOW_HASH, HashTableAddNullptr)
{
    EXPECT_EQ(nullptr, hash_table_add(nullptr, nullptr));
}

TEST(KDADK_FLOW_HASH, HashTableDestroyNullptr)
{
    EXPECT_EQ(hash_table_destroy(nullptr), KDADK_STS_NULL_PTR_ERR);
}

TEST(KDADK_FLOW_HASH, ChangeNodeStatusNullptr)
{
    EXPECT_EQ(change_node_status(nullptr, nullptr, 1), nullptr);
}

TEST(KDADK_FLOW_HASH, AddPacketToFlowNode_WrongIpAddr)
{
    flow_hash_node *node = new flow_hash_node;
    *node = {};
    node->flow_src_ip = 0x030011AC;
    packet_info info_obj;
    info_obj = {};
    packet_info *info = &info_obj;
    info->tuple.src_ip = 0;
    info->tuple.dst_ip = 0;
    add_packet_to_flow_node(node, info);
    EXPECT_EQ((uint32_t)0, node->reverse_raw_data.packets_cnt);
    delete node;
}

TEST(KDADK_FLOW_HASH, RemoveNodeFromHashBucketNullptr)
{
    EXPECT_EQ(remove_node_from_hash_bucket(nullptr, nullptr), KDADK_STS_NULL_PTR_ERR);
}

TEST(KDADK_FLOW_HASH, AddPacketToFlowNode_SrcIpVersionNot0)
{
    flow_hash_node *node = new flow_hash_node;
    node->flow_src_ip = 0x030011AC;
    packet_info *info = new packet_info;
    info->tuple.src_ip = 0x030011AC;
    info->tls_info.client_version = 1;
    add_packet_to_flow_node(node, info);
    EXPECT_EQ(node->raw_data.tls_infos.size(), (uint32_t)1);
}

TEST(KDADK_FLOW_HASH, AddPacketToFlowNode_DstIpVersionNot0)
{
    flow_hash_node *node = new flow_hash_node;
    node->flow_src_ip = 0x030011AC;
    packet_info *info = new packet_info;
    info->tuple.dst_ip = 0x030011AC;
    info->tls_info.client_version = 1;
    add_packet_to_flow_node(node, info);
    EXPECT_EQ(node->reverse_raw_data.tls_infos.size(), (uint32_t)1);
}

TEST(TokenizeUrlTest, AllBranches)
{
    // 1. 空输入
    EXPECT_TRUE(tokenize_url("").empty());

    // 2. 仅包含分隔符
    EXPECT_TRUE(tokenize_url("////??,,==&&..::").empty());

    // 3. 无分隔符的纯文本
    {
        std::vector<std::string> expected = {"helloworld"};
        EXPECT_EQ(tokenize_url("helloworld"), expected);
    }

    // 4. 标准URL解析
    {
        std::vector<std::string> expected = {"https", "www", "example", "com", "api", "v1", "users", "ID", "123"};
        EXPECT_EQ(tokenize_url("https://www.example.com/api/v1/users?ID=123"), expected);
    }

    // 5. 连续分隔符处理
    {
        std::vector<std::string> expected = {"user", "name", "id"};
        EXPECT_EQ(tokenize_url("user//name==id??"), expected);
    }

    // 6. 分隔符在开头
    {
        std::vector<std::string> expected = {"example","com", "path", "file"};
        EXPECT_EQ(tokenize_url(":://example.com/path/file"), expected);
    }

    // 7. 分隔符在结尾
    {
        std::vector<std::string> expected = {"query", "param"};
        EXPECT_EQ(tokenize_url("query=param/.,&?"), expected);
    }

    // 8. 包含特殊字符的令牌
    {
        std::vector<std::string> expected = {"user_name123", "pass-word", "my@data"};
        EXPECT_EQ(tokenize_url("user_name123:pass-word,my@data"), expected);
    }
}

TEST(IsAllDigitsTest, AllBranches)
{
    // 1. 全数字（有效情况）
    EXPECT_TRUE(is_all_digits("0"));
    EXPECT_TRUE(is_all_digits("1234567890"));
    EXPECT_TRUE(is_all_digits("987654321"));

    // 2. 非全数字（无效情况）
    EXPECT_FALSE(is_all_digits("123a"));       // 包含小写字母
    EXPECT_FALSE(is_all_digits("A456"));       // 包含大写字母
    EXPECT_FALSE(is_all_digits("12.34"));      // 包含小数点
    EXPECT_FALSE(is_all_digits("12 34"));      // 包含空格
    EXPECT_FALSE(is_all_digits("12-34"));      // 包含连字符
    EXPECT_FALSE(is_all_digits("abc"));        // 纯字母
    EXPECT_FALSE(is_all_digits("!@#"));        // 纯特殊字符
    EXPECT_FALSE(is_all_digits("12\n34"));     // 包含控制字符
}

TEST(FilterTokensTest, AllBranches)
{
    // 1. 空令牌列表
    EXPECT_TRUE(filter_tokens({}).empty());

    // 2. 长度超过MAX_TOKEN_LENGTH（20）的令牌
    {
        std::string long_token(21, 'a');  // 21个字符
        EXPECT_TRUE(filter_tokens({long_token}).empty());
    }

    // 3. 长度等于MAX_TOKEN_LENGTH（20）的令牌（应保留）
    {
        std::string boundary_token(20, 'b');  // 20个字符
        std::vector<std::string> expected = {boundary_token};
        EXPECT_EQ(filter_tokens({boundary_token}), expected);
    }

    // 4. 全数字令牌（应过滤）
    EXPECT_TRUE(filter_tokens({"0", "123", "456789"}).empty());

    // 5. 包含在INVALID_WORDS中的令牌（应过滤）
    {
        std::vector<std::string> invalid = {"http", "https", "com", "cn", "www", "net", "org"};
        EXPECT_TRUE(filter_tokens(invalid).empty());
    }

    // 6. 大小写变体的无效词（应过滤）
    {
        std::vector<std::string> case_variants = {"Http", "HTTPS", "Com", "WWW", "Net"};
        EXPECT_TRUE(filter_tokens(case_variants).empty());
    }

    // 7. 有效的令牌（应保留）
    {
        std::vector<std::string> valid = {"example", "test123", "User", "Path_Name", "query1"};
        EXPECT_EQ(filter_tokens(valid), valid);  // 全部保留
    }

    // 8. 混合类型的令牌
    {
        std::vector<std::string> mixed = {
            "valid",                  // 有效
            std::string(21, 'x'),     // 太长（21字符）
            "HTTP",                   // 无效词
            "12345",                  // 全数字
            "Example",                // 有效
            "COM",                    // 无效词
            std::string(20, 'y'),     // 刚好20字符（有效）
            "query_data"              // 有效（非完整无效词）
        };
        std::vector<std::string> expected = {"valid", "Example", std::string(20, 'y'), "query_data"};
        EXPECT_EQ(filter_tokens(mixed), expected);
    }
}