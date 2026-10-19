#include "ktag_feature.h"
#include "ktag_feature_plugin.h"
#include "ktag_traffic_features.h"
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

// Exercise packet processing and plugin callbacks through the public Feature API.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0U || size > UINT32_MAX) return 0;
    auto *registry = ktag_feature_registry_create();
    auto *schema = ktag_feature_schema_create_legacy_v1();
    const char *path = std::getenv("KTAG_LEGACY_FEATURE_PLUGIN");
    if (!path || !*path) path = KTAG_FUZZ_FEATURE_PLUGIN;
    auto *plugin = registry ? ktag_feature_plugin_load(path, registry) : nullptr;
    if (!registry || !schema || !plugin) std::abort();
    ktag_feature_config config{};
    config.struct_size = sizeof(config); config.api_version = KTAG_API_VERSION;
    config.registry = registry; config.schema = schema;
    config.collect_annotations = 1U;
    config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
    auto *context = ktag_feature_create(&config);
    auto *batch = ktag_feature_batch_create(schema, 1U);
    if (!context || !batch) std::abort();
    ktag_packet packet{};
    packet.struct_size = sizeof(packet); packet.api_version = KTAG_API_VERSION;
    packet.data = data; packet.captured_length = packet.original_length = static_cast<uint32_t>(size);
    packet.link_type = (data[0] & 1U) ? 1U : 113U;
    packet.sequence = 1U; packet.timestamp.seconds = 1;
    (void)ktag_feature_process_packet(context, &packet, batch);
    (void)ktag_feature_flush(context, batch);
    for (uint32_t i = 0U; i < ktag_feature_batch_get_count(batch); ++i) {
        ktag_feature_record record{};
        if (ktag_feature_batch_get_record(batch, i, &record) != KTAG_SUCCESS) std::abort();
        const char *annotation = nullptr;
        (void)ktag_feature_batch_get_annotation(batch, i, KTAG_TRAFFIC_RAWBOW_ANNOTATION, &annotation);
        if (auto *engine = fuzz_inference_engine()) {
            feature_vector vector{};
            if (record.value_count > FEATURES_NUM) std::abort();
            std::memcpy(vector.features, record.values, record.value_count * sizeof(float));
            feature_vector_list list{&vector, 1U, 1U};
            int prediction = 0;
            inference_result result{&prediction, 0U};
            (void)inference_predict(engine, &list, &result);
        }
    }
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_plugin_unload(plugin);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
    return 0;
}
