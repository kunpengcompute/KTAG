#include "ktag_feature_extract.h"
#include "ktag_legacy_inference.h"

#include <stddef.h>
#include <stdint.h>
#include <cstdlib>
#include <cstring>

namespace {

struct inference_holder {
    inference_engine *engine = nullptr;

    inference_holder()
    {
        const char *config_path = std::getenv("KTAG_LEGACY_CONFIG");
        if (config_path != nullptr && config_path[0] != '\0') {
            inference_config config = {config_path};
            engine = inference_init(&config);
        }
    }

    ~inference_holder()
    {
        inference_destroy(engine);
    }
};

inference_engine *fuzz_inference_engine()
{
    static inference_holder holder;
    return holder.engine;
}

} // namespace

// LibFuzzer目标函数
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > UINT32_MAX - 16U) {
        return 0;
    }
    // 测试extractor_init
    int linkType = 0;
    if (size >= 4) {
        std::memcpy(&linkType, data, sizeof(linkType));
    } else {
        linkType = size != 0U && (data[0] & 1U) != 0U ?
            DLT_LINUX_SLL : DLT_EN10MB;
    }
    feature_extractor *extractor1 = extractor_init(linkType);
    extractor_destroy(extractor1);
    int random_flag = size > 1U && (data[1] & 1U) != 0U ?
        DLT_LINUX_SLL : DLT_EN10MB;
    feature_extractor *extractor = extractor_init(random_flag);
    if (extractor) {
        // 测试process_packet
        struct timeval ts;
        gettimeofday(&ts, NULL);
        feature_vector features;
        int has_feature = 0;
        
        // 生成符合链路层格式的数据包
        unsigned char *packet = NULL;
        uint32_t packet_len = 0;
        
        if (random_flag == DLT_EN10MB) {
            // 以太网帧格式：目标MAC(6) + 源MAC(6) + 类型(2) + 数据
            packet_len = size + 14; // 14字节以太网头部
            packet = (unsigned char *)malloc(packet_len);
            if (packet) {
                // 填充目标MAC
                memset(packet, 0x00, 6);
                // 填充源MAC
                memset(packet + 6, 0x11, 6);
                // 填充以太网类型（IP协议）
                packet[12] = 0x08;
                packet[13] = 0x00;
                // 填充数据
                memcpy(packet + 14, data, size);
            }
        } else if (random_flag == DLT_LINUX_SLL) {
            // Linux SLL格式：16字节头部 + 数据
            packet_len = size + 16; // 16字节SLL头部
            packet = (unsigned char *)malloc(packet_len);
            if (packet) {
                // 填充包类型
                packet[0] = 0x00;
                packet[1] = 0x01;
                // 填充ARP硬件类型
                packet[2] = 0x00;
                packet[3] = 0x01;
                // 填充链路层地址长度
                packet[4] = 6;
                // 填充链路层地址
                memset(packet + 5, 0x11, 8);
                packet[13] = 0x00;
                // 填充协议类型（IP协议）
                packet[14] = 0x08;
                packet[15] = 0x00;
                // 填充数据
                memcpy(packet + 16, data, size);
            }
        }
        
        if (packet) {
            process_packet(extractor, packet, packet_len, &ts, &features, &has_feature);
            
            // 测试process_packet_with_rawbow
            rawbow rawbows;
            process_packet_with_rawbow(extractor, packet, packet_len, &ts, &features, &rawbows, &has_feature);
            
            free(packet);
        }
        
        // 测试extractor_finalize
        feature_vector_list featureList;
        featureList.capacity = 100;
        featureList.vectors = static_cast<feature_vector*>(malloc(sizeof(feature_vector) * featureList.capacity));
        extractor_finalize(extractor, &featureList);
        if (featureList.vectors) {
            free(featureList.vectors);
        }
        
        // 测试extractor_finalize_with_rawbow
        feature_vector_list featureList2;
        featureList2.capacity = 100;
        featureList2.vectors = static_cast<feature_vector*>(malloc(sizeof(feature_vector) * featureList2.capacity));
        rawbow_list rawbowList;
        rawbowList.capacity = 100;
        rawbowList.rawbows = static_cast<rawbow*>(malloc(sizeof(rawbow) * rawbowList.capacity));
        extractor_finalize_with_rawbow(extractor, &featureList2, &rawbowList);
        if (featureList2.vectors) {
            free(featureList2.vectors);
        }
        if (rawbowList.rawbows) {
            free(rawbowList.rawbows);
        }
        
        // 测试extractor_get_statistics
        statistics stats;
        extractor_get_statistics(extractor, &stats);
        
        // 测试extractor_reset_statistics
        extractor_reset_statistics(extractor);

        // 测试extractor_destroy
        extractor_destroy(extractor);
    }
    
    // KTAG_LEGACY_CONFIG is optional. When present, reuse one engine instead
    // of loading the ONNX model for every fuzz input.
    inference_engine *engine = fuzz_inference_engine();
    if (engine) {
        // 测试inference_predict
        feature_vector vector{};
        feature_vector_list features{};
        features.capacity = 1;
        features.count = 1;
        features.vectors = &vector;
        if (size > 0) {
            for (int i = 0; i < FEATURES_NUM; i++) {
                vector.features[i] = data[static_cast<size_t>(i) % size];
            }
        }
        int prediction = 0;
        inference_result result{};
        result.count = 1;
        result.predictions = &prediction;
        inference_predict(engine, &features, &result);
    }
    
    // 测试空指针情况
    extractor_destroy(nullptr);
    inference_destroy(nullptr);
    
    return 0;
}
