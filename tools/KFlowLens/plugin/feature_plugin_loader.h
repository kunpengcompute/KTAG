/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_FEATURE_PLUGIN_LOADER_H
#define KTAG_DEMO_FEATURE_PLUGIN_LOADER_H

#include <cstddef>
#include <string>
#include <vector>

#include "ktag_feature_plugin.h"

class ktag_demo_feature_plugin_loader {
public:
    ktag_demo_feature_plugin_loader() = default;
    ~ktag_demo_feature_plugin_loader();

    ktag_demo_feature_plugin_loader(
        const ktag_demo_feature_plugin_loader &) = delete;
    ktag_demo_feature_plugin_loader &operator=(
        const ktag_demo_feature_plugin_loader &) = delete;

    bool load(
        const std::string &path,
        ktag_feature_registry *registry,
        std::string *error);

    /* Contexts and the Registry must be destroyed before this call. */
    void unload_all();

    size_t size() const;
    const std::string &plugin_id(size_t index) const;

private:
    struct loaded_plugin {
        void *library = nullptr;
        ktag_feature_plugin_descriptor descriptor{};
        void *plugin_state = nullptr;
        std::string path;
        std::string id;
        std::string version;
    };

    std::vector<loaded_plugin> plugins_;
};

#endif /* KTAG_DEMO_FEATURE_PLUGIN_LOADER_H */
