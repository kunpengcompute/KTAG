/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_PREDICTION_BATCH_INTERNAL_H
#define KTAG_PREDICTION_BATCH_INTERNAL_H

#include <vector>

#include "ktag_inference.h"

struct ktag_prediction_batch {
    std::vector<ktag_prediction_record> records;
};

#endif /* KTAG_PREDICTION_BATCH_INTERNAL_H */
