#include "common.h"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>

namespace tcp_bench {
void load_feature_plugins(const Config &config, ktag_feature_registry *registry,
                          FeaturePlugins &plugins) {
  require(registry != nullptr, "cannot create feature registry");
  auto paths = config.plugin_paths;
  if (paths.empty()) {
    if (const char *override_path = std::getenv("KTAG_TRAFFIC_FEATURE_PLUGIN")) {
      require(*override_path != '\0', "KTAG_TRAFFIC_FEATURE_PLUGIN is empty");
      paths.emplace_back(override_path);
    } else {
      Dl_info library{};
      require(dladdr(reinterpret_cast<void *>(&ktag_feature_registry_create), &library) != 0 &&
                  library.dli_fname != nullptr,
              "cannot locate libktag_feature; configure pipeline.plugins");
      paths.push_back((std::filesystem::path(library.dli_fname).parent_path() /
                       "plugins/libktag_traffic_features.so").string());
    }
  }
  for (const auto &path : paths) {
    Handle<ktag_loaded_feature_plugin, ktag_feature_plugin_unload> plugin(
        ktag_feature_plugin_load(path.c_str(), registry), ktag_feature_plugin_unload);
    require(bool(plugin), "feature plugin " + path + ": " +
                              ktag_feature_plugin_get_last_error());
    plugins.push_back(std::move(plugin));
  }
}
} // namespace tcp_bench
