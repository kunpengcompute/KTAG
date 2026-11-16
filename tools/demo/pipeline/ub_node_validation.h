/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */
#ifndef KTAG_DEMO_UB_NODE_VALIDATION_H
#define KTAG_DEMO_UB_NODE_VALIDATION_H

#include <string>
#include <vector>

// The deployed UB SDK sorts node indices by hostname, but its peer mapping
// expects the User after the Providers. Reject unsafe names before SDK init.
// Callers validate non-empty, unique names and exactly one User first.
inline bool ktag_demo_validate_ub_hostname_order(
    const std::vector<std::string> &providers, const std::string &user,
    std::string *error) {
    for (const auto &provider : providers) {
        if (provider >= user) {
            if (error != nullptr) {
                *error = "UB hostname order invalid: Provider/Frontend '" + provider +
                         "' must sort before User/Backend '" + user +
                         "' (case-sensitive lexicographic order); required by the deployed UB SDK";
            }
            return false;
        }
    }
    return true;
}

#endif /* KTAG_DEMO_UB_NODE_VALIDATION_H */
