/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "ktag_test.h"

TEST(KTAG_DOMAIN_NAME_HASH, Table_Create_NullParameter)
{
    KTAGResult res = domain_name_table_create(nullptr);
    EXPECT_EQ(res, KTAG_STS_PARAMETER_ERR);
}

TEST(KTAG_DOMAIN_NAME_HASH, Table_Create_Basic)
{
    domain_name_hash *domain_name_table = nullptr;
    KTAGResult res = domain_name_table_create(&domain_name_table);
    EXPECT_EQ(res, KTAG_STS_NO_ERR);
}

TEST(KTAG_DOMAIN_NAME_HASH, Table_Add_NullParameter)
{
    std::string s = "";
    std::string& value = s;
    KTAGResult res = domain_name_table_add(nullptr, 0, value);
    EXPECT_EQ(res, KTAG_STS_PARAMETER_ERR);
}

TEST(KTAG_DOMAIN_NAME_HASH, Table_Add_IndexOverflow)
{
    domain_name_hash *domain_name_table = new domain_name_hash;
    domain_name_table->domain_name_buckets.insert({0, "0"});
    domain_name_table->domain_name_buckets.insert({1, "1"});
    domain_name_table->table_max_size = 2;
    std::string s = "2";
    std::string& value = s;
    KTAGResult res = domain_name_table_add(domain_name_table, 2, value);
    EXPECT_EQ(res, KTAG_STS_VECTOR_INDEX_OVERFLOW);
}

TEST(KTAG_DOMAIN_NAME_HASH, Table_Destroy_NullPointer)
{
    KTAGResult res = domain_name_table_destroy(nullptr);
    EXPECT_EQ(res, KTAG_STS_NULL_PTR_ERR);
}

TEST(KTAG_DOMAIN_NAME_HASH, Table_Destroy_Basic)
{
    domain_name_hash *domain_name_table = new domain_name_hash;
    KTAGResult res = domain_name_table_destroy(domain_name_table);
    EXPECT_EQ(res, KTAG_STS_NO_ERR);
}

TEST(KTAG_DOMAIN_NAME_HASH, Get_Domain_By_Ip_NullPointer)
{
    bool status = true;
    bool &is_exist = status;
    KTAGResult res = get_domain_by_ip(nullptr, nullptr, is_exist);
    EXPECT_EQ(res, KTAG_STS_PARAMETER_ERR);
}

TEST(KTAG_DOMAIN_NAME_HASH, Get_Domain_By_Ip_Basic)
{
    bool status = false;
    bool &is_exist = status;
    flow_hash_node *node = new flow_hash_node;
    domain_name_hash *domain_name_table = new domain_name_hash;
    KTAGResult res = get_domain_by_ip(node, domain_name_table, is_exist);
    EXPECT_EQ(res, KTAG_STS_NO_ERR);
}

TEST(KTAG_DOMAIN_NAME_HASH, Get_Domain_By_Ip_NoDstNoSrc)
{
    bool status = false;
    bool &is_exist = status;
    flow_hash_node *node = new flow_hash_node;
    domain_name_hash *domain_name_table = new domain_name_hash;
    domain_name_table->domain_name_buckets.insert({10, "10"});
    node->tuple.dst_ip = 4;
    node->tuple.src_ip = 2;
    KTAGResult res = get_domain_by_ip(node, domain_name_table, is_exist);
    EXPECT_EQ(res, KTAG_STS_NO_ERR);
}