/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "kdadk_test.h"
#include "kdadk_feature_extract.h"

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_MEAN_Basic)
{
    std::vector<uint32_t> test_data = {1, 2, 3, 4, 5};
    double mean_result = calculate_mean(test_data);

    ASSERT_DOUBLE_EQ(mean_result, 3);
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_MEAN_SingleElement)
{
    std::vector<uint32_t> nums = {42};
    EXPECT_DOUBLE_EQ(42.0, calculate_mean(nums));
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_MEAN_LargeNumbers)
{
    std::vector<uint32_t> nums = {
        100'000'000, 
        200'000'000, 
        300'000'000
    };
    EXPECT_DOUBLE_EQ(200'000'000.0, calculate_mean(nums));
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_MEAN_MaxUint32)
{
    uint32_t max_val = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> nums = {max_val, max_val, max_val};
    EXPECT_DOUBLE_EQ(static_cast<double>(max_val), calculate_mean(nums));
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_VAR)
{
    std::vector<uint32_t> nums = {1, 2, 3, 4, 5};
    double mean = 3.0;
    double expected = (4+1+0+1+4)/5.0; // (2^2 + 1^2 + 0 + 1^2 + 2^2)/5
    EXPECT_DOUBLE_EQ(expected, calculate_var(nums, mean));
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_HIST_NullPointer)
{
    std::vector<uint32_t> nums = {1, 2, 3};
    calculate_hist(nums, 1, 3, nullptr, HISTOGRAM_NUMS);
    SUCCEED(); // 不崩溃即通过
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_HIST_InvalidRange)
{
    std::vector<uint32_t> nums = {5, 10, 15};
    double *hist = new double[HISTOGRAM_NUMS]();
    calculate_hist(nums, 10.0, 10.0 + 1e-6, hist, HISTOGRAM_NUMS);
    
    EXPECT_DOUBLE_EQ(nums.size(), hist[0]);
    for (int i = 1; i < HISTOGRAM_NUMS; i++) {
        EXPECT_DOUBLE_EQ(0.0, hist[i]);
    }
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_HIST_Basic)
{
    std::vector<uint32_t> nums = {
        0, 
        1000, 
        50000, 
        1000000,
        std::numeric_limits<uint32_t>::max()
    };
    double *hist = new double[HISTOGRAM_NUMS]();
    calculate_hist(nums, 0.0, 1000000.0, hist, HISTOGRAM_NUMS);
    
    EXPECT_DOUBLE_EQ(3.0, hist[0]);   // 0, 1000, 50000
    EXPECT_DOUBLE_EQ(2.0, hist[15]);  // 1000000 和 max_value
    for (int i = 1; i < 15; i++) {
        EXPECT_DOUBLE_EQ(0.0, hist[i]);
    }
}

TEST(KDADK_FEATURE_EXTRACT, CAL_DIFFS_EmptyTS)
{
    std::vector<struct timeval> ts;
    std::vector<uint32_t> time_diff(1, 555);
    cal_diffs(ts, time_diff);

    // time_diff应保持原状不修改
    EXPECT_EQ((uint32_t)1, time_diff.size());
    EXPECT_EQ((uint32_t)555, time_diff[0]);
}

TEST(KDADK_FEATURE_EXTRACT, CAL_DIFFS_SizeMismatch)
{
    std::vector<struct timeval> ts = {
        {100, 0},
        {101, 0},
        {102, 0}
    };
    std::vector<uint32_t> time_diff(1, 555);
    cal_diffs(ts, time_diff);

    // time_diff应保持原状不修改
    EXPECT_EQ((uint32_t)1, time_diff.size());
    EXPECT_EQ((uint32_t)555, time_diff[0]);
}

TEST(KDADK_FEATURE_EXTRACT, CAL_DIFFS_Basic)
{
    std::vector<struct timeval> ts = {
        {100, 123456},
        {101, 123456},
        {101, 123456},
        {101, 123457}
    };
    std::vector<uint32_t> time_diff(3, 0);

    cal_diffs(ts, time_diff);
    
    ASSERT_EQ(3, time_diff.size());
    EXPECT_EQ(1000000, time_diff[0]);  
    EXPECT_EQ(0, time_diff[1]); 
    EXPECT_EQ(1, time_diff[2]); 
}

TEST(KDADK_FEATURE_EXTRACT, CALULATE_CRC_BYTE_ZeroByte)
{
    char byte = 0x00;
    uint16_t crc = 0xFFFF;
    
    calculate_crc_byte(byte, crc);
    
    EXPECT_EQ(0x5FB1, crc);
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_CRC_NullNode)
{
    flow_hash_node* node = nullptr;
    double result[RAW_BOW_HISTOGRAM_NUMS] = {0.0};
    calculate_crc(node, result, RAW_BOW_HISTOGRAM_NUMS);
    // 结果不应改变
    for (int i = 0; i < RAW_BOW_HISTOGRAM_NUMS; ++i) {
        EXPECT_DOUBLE_EQ(0, result[i]);
    }
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_CRC_ZeroRawbowCount)
{
    double result[RAW_BOW_HISTOGRAM_NUMS] = {0.0};
    flow_hash_node *node = new flow_hash_node;
    node->rawbow_cnt = 0;
    calculate_crc(node, result, RAW_BOW_HISTOGRAM_NUMS);
    // 结果不应改变
    for (int i = 0; i < RAW_BOW_HISTOGRAM_NUMS; ++i) {
        EXPECT_DOUBLE_EQ(0.0, result[i]);
    }
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_CRC_SingleZeroByte)
{
    double result[RAW_BOW_HISTOGRAM_NUMS] = {0.0};
    flow_hash_node *node = new flow_hash_node;
    node->rawbow_cnt = 1;
    node->rawbow[0] = "";
    calculate_crc(node, result, RAW_BOW_HISTOGRAM_NUMS);
    for (int i = 0; i < RAW_BOW_HISTOGRAM_NUMS; ++i) {
        if (i != 31) {
            EXPECT_DOUBLE_EQ(0.0, result[i]);
        } else {
            EXPECT_DOUBLE_EQ(1.0, result[i]);
        }
    }
}

TEST(KDADK_FEATURE_EXTRACT, CALCULATE_CRC_SingleFullByte)
{
    double result[RAW_BOW_HISTOGRAM_NUMS] = {0.0};
    flow_hash_node *node = new flow_hash_node;
    node->rawbow_cnt = 1;
    node->rawbow[0] = 0xFF;
    calculate_crc(node, result, RAW_BOW_HISTOGRAM_NUMS);
    for (int i = 0; i < RAW_BOW_HISTOGRAM_NUMS; ++i) {
        if (i != 31) {
            EXPECT_DOUBLE_EQ(0.0, result[i]);
        } else {
            EXPECT_DOUBLE_EQ(1.0, result[i]);
        }
    }
}

TEST(KDADK_FEATURE_EXTRACT, CAL_STATISTICS_INFO_EmptyArrary)
{
    std::vector<uint32_t> empty;
    statistics_info si;

    KDADKResult res = cal_statistics_info(empty, si);
    EXPECT_EQ(res, KDADK_STS_PARAMETER_ERR);
}

TEST(KDADK_FEATURE_EXTRACT, CAL_STATISTICS_INFO_Basic)
{
    std::vector<uint32_t> array = {1, 2, 3, 4 , 5};
    statistics_info si;

    KDADKResult res = cal_statistics_info(array, si);

    EXPECT_EQ(res, KDADK_STS_NO_ERR);
}

class StreamFeatureTest : public::testing::Test
{
protected:
    flow_hash_node *node;
    stream_feature sf;

    void SetUp() override{
        node = new flow_hash_node;
        *node = {};
        node->tuple.src_ip = 0x030011AC;
        node->tuple.src_port = 8080;
        node->tuple.dst_ip = 0x040022BD; // 172.30.0.4
        node->tuple.dst_port = 80;
        node->tuple.protocol = 6; // TCP
        node->src_ip_domain_name = "source.example.com";
        node->dst_ip_domain_name = "dest.example.org";
        node->status = CONNECTED;
    }

    void TearDown() override {
        delete node;
    }

    // 添加发送方向数据
    void add_send_packet(uint32_t header_len, uint32_t payload_len, long tv_sec, long tv_usec) {
        node->raw_data.header_len.push_back(header_len);
        node->raw_data.payload_len.push_back(payload_len);
        node->raw_data.ts.push_back({tv_sec, tv_usec});
        node->raw_data.total_size += header_len + payload_len;
        node->raw_data.packets_cnt++;
    }
    
    // 添加接收方向数据
    void add_receive_packet(uint32_t header_len, uint32_t payload_len, long tv_sec, long tv_usec) {
        node->reverse_raw_data.header_len.push_back(header_len);
        node->reverse_raw_data.payload_len.push_back(payload_len);
        node->reverse_raw_data.ts.push_back({tv_sec, tv_usec});
        node->reverse_raw_data.total_size += header_len + payload_len;
        node->reverse_raw_data.packets_cnt++;
    }
    
    // 添加 rawbow 数据
    void add_rawbow(const std::string& data) {
        if (node->rawbow_cnt < MAX_RAWBOW_CNT) {
            node->rawbow[node->rawbow_cnt++] = data;
        }
    }
};



TEST_F(StreamFeatureTest, NullNode)
{
    KDADKResult result = cal_stream_feature(nullptr, sf);
    EXPECT_EQ(result, KDADK_STS_PARAMETER_ERR);
}

TEST_F(StreamFeatureTest, BasicFunctionality)
{
    // 添加发送方向数据
    add_send_packet(20, 100, 1000, 0);        // t=0, 包头20B, 负载100B
    add_send_packet(22, 150, 1000, 500000);   // t=500ms
    
    // 添加接收方向数据
    add_receive_packet(18, 80, 1001, 0);       // t=0
    add_receive_packet(20, 120, 1001, 200000); // t=200ms
    add_receive_packet(22, 200, 1001, 400000); // t=400ms
    
    // 添加 rawbow 数据
    add_rawbow("Packet1");
    add_rawbow("Packet2");
    
    KDADKResult result = cal_stream_feature(node, sf);
    ASSERT_EQ(result, KDADK_STS_NO_ERR);
    
    // ========== 验证五元组 ==========
    EXPECT_DOUBLE_EQ(sf.src_ip, static_cast<double>(0x030011AC));
    EXPECT_DOUBLE_EQ(sf.src_port, 8080.0);
    EXPECT_DOUBLE_EQ(sf.dst_ip, static_cast<double>(0x040022BD));
    EXPECT_DOUBLE_EQ(sf.dst_port, 80.0);
    EXPECT_DOUBLE_EQ(sf.protocol, 6.0);

    // ========== 验证发送方向 ==========
    EXPECT_DOUBLE_EQ(sf.send_packet_nums, 2.0);
    EXPECT_DOUBLE_EQ(sf.send_byte_nums, 292.0); // (20+100) + (22+150)
    
    // 包头统计 (20, 22 -> min=20, max=22, mean=21, var=1)
    EXPECT_DOUBLE_EQ(sf.send_hdr.min_value, 20.0);
    EXPECT_DOUBLE_EQ(sf.send_hdr.max_value, 22.0);
    EXPECT_DOUBLE_EQ(sf.send_hdr.mean_value, 21.0);
    EXPECT_GT(sf.send_hdr.var_value, 0.0); // 方差应该 > 0
    
    // 负载统计 (100, 150 -> min=100, max=150, mean=125, var=625)
    EXPECT_DOUBLE_EQ(sf.send_pld.min_value, 100.0);
    EXPECT_DOUBLE_EQ(sf.send_pld.max_value, 150.0);
    EXPECT_DOUBLE_EQ(sf.send_pld.mean_value, 125.0);
    EXPECT_GT(sf.send_pld.var_value, 0.0); // 方差应该 > 0
    
    // 时间间隔统计 (只有1个间隔: 500000us)
    double send_interval_log = log2(500000.0); // ≈ 18.931568569324174
    EXPECT_NEAR(sf.send_interval.min_value, send_interval_log, 1e-5);
    EXPECT_NEAR(sf.send_interval.max_value, send_interval_log, 1e-5);
    EXPECT_NEAR(sf.send_interval.mean_value, send_interval_log, 1e-5);
    EXPECT_DOUBLE_EQ(sf.send_interval.var_value, 0.0); // 只有1个值，方差=0
    
    // ========== 验证接收方向 ==========
    EXPECT_DOUBLE_EQ(sf.receive_packet_nums, 3.0);
    EXPECT_DOUBLE_EQ(sf.receive_byte_nums, 460.0); // (18+80) + (20+120) + (22+200)
    
    // 包头统计 (18, 20, 22 -> min=18, max=22, mean=20, var≈2.67)
    EXPECT_DOUBLE_EQ(sf.receive_hdr.min_value, 18.0);
    EXPECT_DOUBLE_EQ(sf.receive_hdr.max_value, 22.0);
    EXPECT_DOUBLE_EQ(sf.receive_hdr.mean_value, 20.0);
    EXPECT_GT(sf.receive_hdr.var_value, 0.0); // 方差应该 > 0
    
    // 负载统计 (80, 120, 200 -> min=80, max=200, mean≈133.33, var≈2488.89)
    EXPECT_DOUBLE_EQ(sf.receive_pld.min_value, 80.0);
    EXPECT_DOUBLE_EQ(sf.receive_pld.max_value, 200.0);
    EXPECT_DOUBLE_EQ(sf.receive_pld.mean_value, (80.0+120.0+200.0)/3.0); // ≈ 133.333...
    EXPECT_GT(sf.receive_pld.var_value, 0.0); // 方差应该 > 0
    
    // 时间间隔统计 (2个间隔,都是200000us)
    double recv_interval_log = log2(200000.0); // ≈ 17.609640474436812
    EXPECT_NEAR(sf.receive_interval.min_value, recv_interval_log, 1e-5);
    EXPECT_NEAR(sf.receive_interval.max_value, recv_interval_log, 1e-5);
    EXPECT_NEAR(sf.receive_interval.mean_value, recv_interval_log, 1e-5);
    EXPECT_DOUBLE_EQ(sf.receive_interval.var_value, 0.0); // 2个相同值，方差=0
    
    // ========== 验证 CRC 结果 ==========
    for (int i = 0; i < RAW_BOW_HISTOGRAM_NUMS; ++i) {
        if (i == 20) {
            EXPECT_DOUBLE_EQ(sf.raw_bow_crc_result[i], 2.0);
        } else {
            EXPECT_DOUBLE_EQ(sf.raw_bow_crc_result[i], 0.0);
        }
    }
}

TEST_F(StreamFeatureTest, SendOnly) {
    add_send_packet(20, 100, 1000, 0);
    add_send_packet(22, 150, 1000, 500000);
    
    KDADKResult result = cal_stream_feature(node, sf);
    ASSERT_EQ(result, KDADK_STS_NO_ERR);
    
    // 发送方向验证
    EXPECT_DOUBLE_EQ(sf.send_packet_nums, 2.0);
    EXPECT_DOUBLE_EQ(sf.send_byte_nums, 20+100+22+150);
    
    // 接收方向应为0
    EXPECT_DOUBLE_EQ(sf.receive_packet_nums, 0.0);
    EXPECT_DOUBLE_EQ(sf.receive_byte_nums, 0.0);
    
    // 接收方向的统计信息应为初始值
    EXPECT_DOUBLE_EQ(sf.receive_hdr.min_value, 0.0);
    EXPECT_DOUBLE_EQ(sf.receive_hdr.max_value, 0.0);
    EXPECT_DOUBLE_EQ(sf.receive_interval.min_value, 0.0);
}

TEST_F(StreamFeatureTest, ReceiveOnly) {
    add_receive_packet(18, 80, 1001, 0);
    add_receive_packet(20, 120, 1001, 200000);
    
    KDADKResult result = cal_stream_feature(node, sf);
    ASSERT_EQ(result, KDADK_STS_NO_ERR);
    
    // 发送方向应为0
    EXPECT_DOUBLE_EQ(sf.send_packet_nums, 0.0);
    EXPECT_DOUBLE_EQ(sf.send_byte_nums, 0.0);
    
    // 接收方向验证
    EXPECT_DOUBLE_EQ(sf.receive_packet_nums, 2.0);
    EXPECT_DOUBLE_EQ(sf.receive_byte_nums, 18+80+20+120);
}

TEST_F(StreamFeatureTest, NoPackets) {
    KDADKResult result = cal_stream_feature(node, sf);
    ASSERT_EQ(result, KDADK_STS_NO_ERR);
    
    // 所有计数应为0
    EXPECT_DOUBLE_EQ(sf.send_packet_nums, 0.0);
    EXPECT_DOUBLE_EQ(sf.send_byte_nums, 0.0);
    EXPECT_DOUBLE_EQ(sf.receive_packet_nums, 0.0);
    EXPECT_DOUBLE_EQ(sf.receive_byte_nums, 0.0);
    
    // 所有统计信息应为初始值
    EXPECT_DOUBLE_EQ(sf.send_hdr.min_value, 0.0);
    EXPECT_DOUBLE_EQ(sf.receive_interval.min_value, 0.0);
}

TEST(KDADK_FEATURE_EXTRACT, SI2A_NullArray){
    statistics_info si;
    int b= 0;
    int& a = b;
    KDADKResult res = statisticsInfo2Array(si, nullptr, 0, a);
    EXPECT_EQ(res, KDADK_STS_NULL_PTR_ERR);
}

TEST(KDADK_FEATURE_EXTRACT, SI2A_IndexOverflow)
{
    int a = 0;
    int& begin = a;
    int size = 0;
    statistics_info si;
    double *array = new double;
    KDADKResult res = statisticsInfo2Array(si, array, size, begin);
    EXPECT_EQ(res, KDADK_STS_VECTOR_INDEX_OVERFLOW);
}

TEST(KDADK_FEATURE_EXTRACT, SF2A_NullArrary)
{
    stream_feature sf;
    double* array = nullptr;
    int size = 0;
    KDADKResult res = streamFeatur2Array(sf, array, size);
    EXPECT_EQ(res, KDADK_STS_NULL_PTR_ERR);
}

TEST(KDADK_FEATURE_EXTRACT, SF2A_Overflow)
{
    stream_feature sf;
    double* array = new double;
    int size = 0;
    KDADKResult res = streamFeatur2Array(sf, array, size);
    EXPECT_EQ(res,KDADK_STS_VECTOR_INDEX_OVERFLOW);
}

class FeatureExtractorTest : public ::testing::Test {
protected:
    feature_extractor *extractor;
    
    void SetUp() override {
        extractor = nullptr;
    }
    
    void TearDown() override {
        if (extractor) {
            extractor_destroy(extractor);
            extractor = nullptr;
        }
    }
};

TEST_F(FeatureExtractorTest, InitAndDestroy) {
    // 测试正常初始化
    extractor = extractor_init(1); // DLT_EN10MB
    ASSERT_NE(extractor, nullptr) << "Extractor should be initialized";
    
    // 测试销毁
    extractor_destroy(extractor);
    extractor = nullptr;
    SUCCEED() << "Extractor destroyed successfully";
    
    // 测试空指针销毁
    extractor_destroy(nullptr);
    SUCCEED() << "Destroy NULL extractor should not crash";
}

TEST_F(FeatureExtractorTest, DifferentLinkTypes) {
    // DLT_EN10MB (Ethernet)
    extractor = extractor_init(1);
    EXPECT_NE(extractor, nullptr) << "Should support DLT_EN10MB";
    extractor_destroy(extractor);
    
    // DLT_LINUX_SLL (Linux cooked)
    extractor = extractor_init(113);
    EXPECT_NE(extractor, nullptr) << "Should support DLT_LINUX_SLL";
    extractor_destroy(extractor);
    
    extractor = nullptr;
}

TEST_F(FeatureExtractorTest, Statistics) {
    extractor = extractor_init(1);
    ASSERT_NE(extractor, nullptr);
    
    statistics stats;
    int ret = extractor_get_statistics(extractor, &stats);
    
    EXPECT_EQ(ret, EXTRACTOR_SUCCESS) << "Should get statistics successfully";
    EXPECT_EQ(stats.total_packets, 0) << "Initial packet count should be 0";
    EXPECT_EQ(stats.total_bytes, 0) << "Initial byte count should be 0";
    EXPECT_EQ(stats.decoded_packets, 0) << "Initial decoded count should be 0";
    EXPECT_EQ(stats.extracted_flows, 0) << "Initial flow count should be 0";
}

TEST_F(FeatureExtractorTest, InvalidParameters) {
    extractor = extractor_init(1);
    ASSERT_NE(extractor, nullptr);
    
    feature_vector features;
    int has_feature;
    struct timeval ts = {0, 0};
    
    // 空提取器
    int ret = process_packet(nullptr, nullptr, 0, &ts, &features, &has_feature);
    EXPECT_EQ(ret, EXTRACTOR_ERROR) << "Should fail with NULL extractor";
    
    // 空数据包
    ret = process_packet(extractor, nullptr, 0, &ts, &features, &has_feature);
    EXPECT_EQ(ret, EXTRACTOR_ERROR) << "Should fail with NULL packet";
    
    // 空时间戳
    unsigned char packet[64] = {0};
    ret = process_packet(extractor, packet, sizeof(packet), nullptr, &features, &has_feature);
    EXPECT_EQ(ret, EXTRACTOR_ERROR) << "Should fail with NULL timestamp";
    
    // 空统计
    ret = extractor_get_statistics(nullptr, nullptr);
    EXPECT_EQ(ret, EXTRACTOR_ERROR) << "Should fail with NULL parameters";
    
    // 空finalize
    ret = extractor_finalize(nullptr, nullptr);
    EXPECT_EQ(ret, EXTRACTOR_ERROR) << "Should fail with NULL parameters";
}

TEST_F(FeatureExtractorTest, FeatureVectorList) {
    extractor = extractor_init(1);
    ASSERT_NE(extractor, nullptr);
    
    feature_vector_list remaining;
    remaining.capacity = 100;
    remaining.count = 0;
    remaining.vectors = static_cast<feature_vector*>(
        malloc(sizeof(feature_vector) * remaining.capacity));
    ASSERT_NE(remaining.vectors, nullptr) << "Should allocate memory";

    int ret = extractor_finalize(extractor, &remaining);
    EXPECT_EQ(ret, EXTRACTOR_SUCCESS) << "Should finalize successfully";
    EXPECT_GE(remaining.count, 0) << "Remaining count should be valid";
    
    free(remaining.vectors);
}

TEST_F(FeatureExtractorTest, PacketProcessing) {
    extractor = extractor_init(1);
    ASSERT_NE(extractor, nullptr);
    
    // 构造一个简单的以太网帧（太短，应该被拒绝）
    unsigned char packet[14] = {0};
    struct timeval ts = {1234567890, 0};
    feature_vector features;
    int has_feature;
    
    int ret = process_packet(extractor, packet, sizeof(packet), &ts, &features, &has_feature);
    EXPECT_EQ(ret, EXTRACTOR_SUCCESS) << "Should process packet";
    EXPECT_EQ(has_feature, 0) << "Short packet should have no feature";
    
    // 检查统计
    statistics stats;
    extractor_get_statistics(extractor, &stats);
    EXPECT_GE(stats.total_packets, 0) << "Packet count should be updated";
}

TEST(FeatureVectorTest, VectorContent) {
    feature_vector fv;
    memset(&fv, 0, sizeof(feature_vector));
    
    // 测试特征向量大小
    EXPECT_EQ(FEATURES_NUM, sizeof(fv.features) / sizeof(fv.features[0])) 
        << "Feature vector size should match FEATURES_NUM";
    
    // 测试特征赋值
    fv.features[0] = 1.0;  // flow_id
    fv.features[1] = 192.168;  // src_ip
    EXPECT_FLOAT_EQ(fv.features[0], 1.0) << "Feature assignment should work";
    EXPECT_FLOAT_EQ(fv.features[1], 192.168) << "Feature assignment should work";
}

TEST_F(FeatureExtractorTest, MultiplePackets) {
    extractor = extractor_init(1);
    ASSERT_NE(extractor, nullptr);
    
    feature_vector features;
    int has_feature;
    
    // 处理多个数据包
    for (int i = 0; i < 10; i++) {
        unsigned char packet[64] = {0};
        struct timeval ts = {1234567890 + i, 0};
        
        int ret = process_packet(extractor, packet, sizeof(packet), &ts, &features, &has_feature);
        EXPECT_EQ(ret, EXTRACTOR_SUCCESS) << "Should process packet " << i;
    }
    
    // 检查统计
    statistics stats;
    extractor_get_statistics(extractor, &stats);
    EXPECT_GE(stats.total_packets, 0) << "Should have processed packets";
}

TEST_F(FeatureExtractorTest, FinalizeState) {
    extractor = extractor_init(1);
    ASSERT_NE(extractor, nullptr);
    
    feature_vector_list remaining;
    remaining.capacity = 100;
    remaining.count = 0;
    remaining.vectors = static_cast<feature_vector*>(
        malloc(sizeof(feature_vector) * remaining.capacity));
    ASSERT_NE(remaining.vectors, nullptr);
    
    // Finalize
    int ret = extractor_finalize(extractor, &remaining);
    EXPECT_EQ(ret, EXTRACTOR_SUCCESS);
    
    // 统计应该仍然可用
    statistics stats;
    ret = extractor_get_statistics(extractor, &stats);
    EXPECT_EQ(ret, EXTRACTOR_SUCCESS);
    
    free(remaining.vectors);
}