/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#ifndef KTAG_DEMO_INFERENCE_SESSION_H
#define KTAG_DEMO_INFERENCE_SESSION_H

#include <memory>
#include <string>
#include <vector>

#include "ktag_feature.h"
#include "ktag_inference.h"
#include "ktag_legacy_types.h"

/* Private bridge for the historical Demo file format. All model input
 * selection, preprocessing and prediction belong to the public Inference API. */
class ktag_demo_inference_session {
public:
    static std::unique_ptr<ktag_demo_inference_session> create(
        const char *config_path, std::string *error);
    ~ktag_demo_inference_session();
    ktag_demo_inference_session(const ktag_demo_inference_session &) = delete;
    ktag_demo_inference_session &operator=(const ktag_demo_inference_session &) = delete;

    bool predict(const feature_vector_list &features,
                 std::vector<feature_vector> *accepted,
                 std::vector<int> *labels, std::string *error);

private:
    ktag_demo_inference_session() = default;
    ktag_feature_batch *features_ = nullptr;
    ktag_prediction_batch *predictions_ = nullptr;
    ktag_inference_context *context_ = nullptr;
};

#endif
