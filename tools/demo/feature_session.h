/* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved. */
/* Demo-only conversion of Feature Batch records to frozen file/inference data.
 * No extraction algorithm or exported compatibility API lives here. */
#ifndef KTAG_DEMO_FEATURE_SESSION_H
#define KTAG_DEMO_FEATURE_SESSION_H
#include "ktag_feature.h"
#include "ktag_feature_plugin.h"
#include "ktag_legacy_types.h"
#include <sys/time.h>
class ktag_demo_feature_session {
public:
    static ktag_demo_feature_session *create(int link_type,
        ktag_domain_store *store = nullptr, const char *plugin_path = nullptr);
    ~ktag_demo_feature_session();
    ktag_demo_feature_session(const ktag_demo_feature_session &) = delete;
    ktag_demo_feature_session &operator=(const ktag_demo_feature_session &) = delete;
    int process(const unsigned char *, uint32_t, const timeval *, feature_vector *, int *);
    int process_with_rawbow(const unsigned char *, uint32_t, const timeval *, feature_vector *, rawbow *, int *);
    int flush(feature_vector_list *);
    int flush_with_rawbow(feature_vector_list *, rawbow_list *);
private:
    ktag_demo_feature_session() = default;
    int process_record(const unsigned char *, uint32_t, const timeval *, feature_vector *, rawbow *, int *);
    int copy_record(uint32_t, feature_vector *, rawbow *);
    int flush_records(feature_vector_list *, rawbow_list *);
    ktag_feature_context *context_ = nullptr;
    ktag_feature_batch *batch_ = nullptr;
    ktag_loaded_feature_plugin *plugin_ = nullptr;
    uint64_t sequence_ = 1;
    uint32_t link_type_ = 0;
};
#endif
