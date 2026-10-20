#include "ktag_feature.h"
#include "ktag_feature_plugin.h"
#include "ktag_traffic_features.h"
#include "ktag_inference.h"

#include <stddef.h>
#include <stdint.h>
#include <cstdio>
#include <cstdlib>

namespace {
struct inference_holder {
    ktag_inference_context *context = nullptr;
    ktag_prediction_batch *predictions = nullptr;

    explicit inference_holder(const ktag_feature_schema *schema) {
        const char *package = std::getenv("KTAG_FUZZ_MODEL_PACKAGE");
        if (!package || !*package) return;
        ktag_inference_config config{};
        config.struct_size = sizeof(config);
        config.api_version = KTAG_API_VERSION;
        config.feature_schema = schema;
        config.model_package_path = package;
        config.intra_op_threads = config.inter_op_threads = 1U;
        context = ktag_inference_create(&config);
        if (!context) {
            std::fprintf(stderr, "Fuzz model package initialization failed: %s\n",
                         ktag_inference_get_last_error());
            std::abort();
        }
        predictions = ktag_prediction_batch_create(1U);
        if (!predictions) std::abort();
    }
    ~inference_holder() {
        ktag_prediction_batch_destroy(predictions);
        ktag_inference_destroy(context);
    }
};

inference_holder &fuzz_inference(const ktag_feature_schema *schema) {
    // Context owns its resolved schema plan; schema is borrowed only on create.
    static inference_holder holder(schema);
    return holder;
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
    auto &inference = fuzz_inference(schema);
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
    }
    if (inference.context) {
        (void)ktag_inference_predict(inference.context, batch, inference.predictions);
    }
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(context);
    ktag_feature_plugin_unload(plugin);
    ktag_feature_schema_destroy(schema);
    ktag_feature_registry_destroy(registry);
    return 0;
}
