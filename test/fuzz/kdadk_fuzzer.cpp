#include <string>
#include <vector>
#include "kdadk_feature_extract.h"
#include "kdadk_inference.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <cstdlib>
#include <iostream>

// LibFuzzer目标函数
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // 测试extractor_init
    int linkType = 0;
    if (size >= 4) {
        linkType = *(int*)data;
    } else {
        linkType = rand() % 2000 - 500;
    }
    feature_extractor *extractor1 = extractor_init(linkType);
    extractor_destroy(extractor1);
    int random_flag = rand() % 2 == 0 ? 1 : 113;
    feature_extractor *extractor = extractor_init(random_flag);
    if (extractor) {
        // 测试process_packet
        struct timeval ts;
        gettimeofday(&ts, NULL);
        feature_vector features;
        int has_feature = 0;
        process_packet(extractor, data, size, &ts, &features, &has_feature);
        
        // 测试process_packet_with_rawbow
        rawbow rawbows;
        process_packet_with_rawbow(extractor, data, size, &ts, &features, &rawbows, &has_feature);
        
        // 测试extractor_get_statistics
        statistics stats;
        extractor_get_statistics(extractor, &stats);
        
        // 测试extractor_reset_statistics
        extractor_reset_statistics(extractor);
        
        // 测试extractor_finalize
        feature_vector_list featureList;
        featureList.capacity = 100;
        featureList.count = 0;
        featureList.vectors = static_cast<feature_vector*>(malloc(sizeof(feature_vector) * featureList.capacity));
        extractor_finalize(extractor, &featureList);
        if (featureList.vectors) {
            free(featureList.vectors);
        }
        
        // 测试extractor_finalize_with_rawbow
        feature_vector_list featureList2;
        featureList2.capacity = 100;
        featureList2.count = 0;
        featureList2.vectors = static_cast<feature_vector*>(malloc(sizeof(feature_vector) * featureList2.capacity));
        rawbow_list rawbowList;
        rawbowList.capacity = 100;
        rawbowList.count = 0;
        rawbowList.rawbows = static_cast<rawbow*>(malloc(sizeof(rawbow) * rawbowList.capacity));
        extractor_finalize_with_rawbow(extractor, &featureList2, &rawbowList);
        if (featureList2.vectors) {
            free(featureList2.vectors);
        }
        if (rawbowList.rawbows) {
            free(rawbowList.rawbows);
        }
        
        // 测试extractor_destroy
        extractor_destroy(extractor);
    }
    
    // 测试inference_init
    inference_config config = {"../../../src/config.yaml"};
    inference_engine *engine = inference_init(&config);
    if (engine) {
        // 测试inference_predict
        feature_vector_list features;
        features.capacity = 10;
        features.count = 1;
        features.vectors = static_cast<feature_vector*>(malloc(sizeof(feature_vector) * features.capacity));
        if (size > 0) {
            for (int i = 0; i < FEATURES_NUM && i < size; i++) {
                features.vectors[0].features[i] = data[i % size];
            }
        }
        inference_result result;
        result.count = 1;
        result.predictions = static_cast<int*>(malloc(sizeof(int) * result.count));
        inference_predict(engine, &features, &result);
        if (features.vectors) {
            free(features.vectors);
        }
        if (result.predictions) {
            free(result.predictions);
        }
        
        // 测试inference_destroy
        inference_destroy(engine);
    }
    
    // 测试空指针情况
    extractor_destroy(nullptr);
    inference_destroy(nullptr);
    
    return 0;
}
