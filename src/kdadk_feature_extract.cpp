/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */
#include "kdadk_feature_extract.h"
#include "kdadk_export.h"
#include <cstdlib>
#include <cstring>

namespace {
void free_flow_hash_node(flow_hash_node *node)
{
    if (node == nullptr) {
        return;
    }
    node->raw_data.header_len.clear();
    node->raw_data.header_len.shrink_to_fit();
    node->raw_data.payload_len.clear();
    node->raw_data.payload_len.shrink_to_fit();
    node->raw_data.ts.clear();
    node->raw_data.ts.shrink_to_fit();
    node->raw_data.http_uris.clear();
    node->raw_data.http_hosts.clear();
    node->raw_data.http_urls.clear();
    node->raw_data.tls_infos.clear();

    node->reverse_raw_data.header_len.clear();
    node->reverse_raw_data.header_len.shrink_to_fit();
    node->reverse_raw_data.payload_len.clear();
    node->reverse_raw_data.payload_len.shrink_to_fit();
    node->reverse_raw_data.ts.clear();
    node->reverse_raw_data.ts.shrink_to_fit();
    node->reverse_raw_data.http_uris.clear();
    node->reverse_raw_data.http_hosts.clear();
    node->reverse_raw_data.http_urls.clear();
    node->reverse_raw_data.tls_infos.clear();

    node->src_ip_domain_name.clear();
    node->dst_ip_domain_name.clear();

    for (uint32_t i = 0; i < MAX_RAWBOW_CNT; i++) {
        node->rawbow[i].clear();
    }

    delete node;
}
}

static int feature_list_add_internal(feature_vector_list *list, const feature_vector *fv)
{
    if (!list || !fv)
        return EXTRACTOR_ERROR;

    if (list->count >= list->capacity) {
        fprintf(stderr, "Error: feature_vector_list capacity exceeded (capacity=%u)\n", list->capacity);
        return EXTRACTOR_ERROR;
    }

    memcpy(&list->vectors[list->count], fv, sizeof(feature_vector));
    list->count++;

    return EXTRACTOR_SUCCESS;
}

static int rawbow_list_add_internal(rawbow_list *list, const rawbow *rb)
{
    if (!list || !rb)
        return EXTRACTOR_ERROR;

    if (list->count >= list->capacity) {
        fprintf(stderr, "Error: rawbow_list capacity exceeded (capacity=%u)\n", list->capacity);
        return EXTRACTOR_ERROR;
    }

    memcpy(&list->rawbows[list->count], rb, sizeof(rawbow));
    list->count++;

    return EXTRACTOR_SUCCESS;
}

KDADK_API feature_extractor *extractor_init(int link_type)
{
    if (link_type != DLT_EN10MB && link_type != DLT_LINUX_SLL) {
        fprintf(stderr, "Error: Unsupported link type %d\n", link_type);
        return NULL;
    }

    feature_extractor *extractor = static_cast<feature_extractor *>(malloc(sizeof(feature_extractor)));
    if (!extractor)
        return NULL;

    memset(extractor, 0, sizeof(feature_extractor));
    extractor->link_type = link_type;
    extractor->next_flow_id = 1;

    if (hash_table_create(&extractor->hash_table, HASH_BUCKETS) || !extractor->hash_table) {
        free(extractor);
        return NULL;
    }

    if (domain_name_table_create(&extractor->domain_name_table) || !extractor->domain_name_table) {
        hash_table_destroy(extractor->hash_table);
        free(extractor);
        return NULL;
    }

    extractor->initialized = 1;
    extractor_reset_statistics(extractor);

    return extractor;
}

static int process_packet_internal(feature_extractor *extractor, const unsigned char *packet, uint32_t packet_len,
                                   const struct timeval *ts, feature_vector *features, rawbow *rawbows,
                                   int *has_feature, int extract_rawbow)
{
    if (!extractor || !extractor->initialized || !packet || !ts || !features || !has_feature) {
        return EXTRACTOR_ERROR;
    }

    *has_feature = 0;
    memset(features, 0, sizeof(feature_vector));
    if (extract_rawbow && rawbows) {
        memset(rawbows, 0, sizeof(rawbow));
    }

    extractor->stats.total_packets++;
    extractor->stats.total_bytes += packet_len;

    packet_info info = {};
    info.packet_size = packet_len;
    info.ts = *ts;

    KDADKResult ret = decode_link_layer(packet, &info, extractor->link_type, extractor->domain_name_table);
    if (ret) {
        return EXTRACTOR_SUCCESS; /* 解析失败跳过 */
    }

    extractor->stats.decoded_packets++;

    flow_hash_node *node = NULL;
    if (info.tuple.protocol == IPPROTO_TCP) {
        node = process_tcp_packet(extractor->hash_table, &info);
    } else if (info.tuple.protocol == IPPROTO_UDP) {
        node = process_udp_packet(extractor->hash_table, &info);
    } else {
        printf("only support TCP or UDP packet\n");
        return EXTRACTOR_SUCCESS;
    }

    if (!node) {
        return EXTRACTOR_SUCCESS;
    }

    process_rawbow(extractor->hash_table, &info, node, extractor->domain_name_table);

    if (info.finish_flag) {
        remove_node_from_hash_bucket(extractor->hash_table, node);

        float fv[FEATURE_MAX_SIZE];
        char rb[RAWBOW_MAX_LEN];
        uint32_t rb_len = 0;

        ret = feature_extract_online(node, fv, rb, &rb_len);
        if (ret == KDADK_STS_NO_ERR) {
            uint64_t flow_id = extractor->next_flow_id++;

            features->features[0] = (float)flow_id;

            memcpy(&features->features[1], fv, sizeof(float) * FEATURE_MAX_SIZE);

            if (extract_rawbow && rawbows) {
                memcpy(rawbows->rawbow, rb, rb_len + 1);
                rawbows->len = rb_len;
            }

            *has_feature = 1;
            extractor->stats.extracted_flows++;
        }

        free_flow_hash_node(node);
    }

    return EXTRACTOR_SUCCESS;
}

KDADK_API int process_packet(feature_extractor *extractor, const unsigned char *packet, uint32_t packet_len,
                   const struct timeval *ts, feature_vector *features, int *has_feature)
{
    return process_packet_internal(extractor, packet, packet_len, ts, features, NULL, has_feature, 0);
}

KDADK_API int process_packet_with_rawbow(feature_extractor *extractor, const unsigned char *packet, uint32_t packet_len,
                               const struct timeval *ts, feature_vector *features, rawbow *rawbows, int *has_feature)
{
    if (!rawbows) {
        return EXTRACTOR_ERROR;
    }

    return process_packet_internal(extractor, packet, packet_len, ts, features, rawbows, has_feature, 1);
}

static int finalize_internal(feature_extractor *extractor, feature_vector_list *features, rawbow_list *rawbows,
                             int extract_rawbow)
{
    if (!extractor || !extractor->initialized || !extractor->hash_table || !features) {
        return EXTRACTOR_ERROR;
    }

    if (!features->vectors || features->capacity == 0) {
        fprintf(stderr, "Error: feature_vector_list not initialized (vectors=%p, capacity=%u)\n",
                (void *)features->vectors, features->capacity);
        return EXTRACTOR_ERROR;
    }

    if (extract_rawbow && rawbows) {
        if (!rawbows->rawbows || rawbows->capacity == 0) {
            fprintf(stderr, "Error: rawbow_list not initialized (rawbows=%p, capacity=%u)\n", (void *)rawbows->rawbows,
                    rawbows->capacity);
            return EXTRACTOR_ERROR;
        }
    }

    features->count = 0;
    if (extract_rawbow && rawbows) {
        rawbows->count = 0;
    }

    for (uint32_t i = 0; i < extractor->hash_table->bucket_num; i++) {
        flow_hash_node *node = extractor->hash_table->buckets[i].nodes;

        while (node) {
            float fv[FEATURE_MAX_SIZE];
            char rb[RAWBOW_MAX_LEN];
            uint32_t rb_len = 0;

            if (feature_extract_online(node, fv, rb, &rb_len) == KDADK_STS_NO_ERR) {
                uint64_t flow_id = extractor->next_flow_id++;

                feature_vector temp_fv;
                temp_fv.features[0] = (float)flow_id;
                memcpy(&temp_fv.features[1], fv, sizeof(float) * FEATURE_MAX_SIZE);

                if (feature_list_add_internal(features, &temp_fv) != EXTRACTOR_SUCCESS) {
                    fprintf(stderr, "Warning: Stopped processing remaining flows due to capacity limit\n");
                    extractor->hash_table->buckets[i].nodes = node;
                    goto cleanup;
                }

                if (extract_rawbow && rawbows) {
                    rawbow temp_rb;
                    memcpy(temp_rb.rawbow, rb, rb_len + 1);
                    temp_rb.len = rb_len;

                    if (rawbow_list_add_internal(rawbows, &temp_rb) != EXTRACTOR_SUCCESS) {
                        fprintf(stderr, "Warning: Stopped processing remaining flows due to capacity limit\n");
                        extractor->hash_table->buckets[i].nodes = node;
                        goto cleanup;
                    }
                }

                extractor->stats.extracted_flows++;
            }

            flow_hash_node *current = node;
            node = node->link_next;
            free_flow_hash_node(current);
        }

        extractor->hash_table->buckets[i].nodes = NULL;
    }

    return EXTRACTOR_SUCCESS;

cleanup:
    for (uint32_t i = 0; i < extractor->hash_table->bucket_num; i++) {
        flow_hash_node *node = extractor->hash_table->buckets[i].nodes;
        while (node) {
            flow_hash_node *current = node;
            node = node->link_next;
            free_flow_hash_node(current);
        }
        extractor->hash_table->buckets[i].nodes = NULL;
    }

    return EXTRACTOR_ERROR;
}

KDADK_API int extractor_finalize(feature_extractor *extractor, feature_vector_list *features)
{
    return finalize_internal(extractor, features, NULL, 0);
}

KDADK_API int extractor_finalize_with_rawbow(feature_extractor *extractor, feature_vector_list *features, rawbow_list *rawbows)
{
    if (!rawbows) {
        return EXTRACTOR_ERROR;
    }

    return finalize_internal(extractor, features, rawbows, 1);
}

KDADK_API int extractor_get_statistics(feature_extractor *extractor, statistics *stats)
{
    if (!extractor || !stats) {
        fprintf(stderr, "Error: Invalid extractor or stats pointer\n");
        return EXTRACTOR_ERROR;
    }

    try {
        *stats = extractor->stats;
        return EXTRACTOR_SUCCESS;
    } catch (const std::exception &e) {
        fprintf(stderr, "Error getting statistics: %s\n", e.what());
        return EXTRACTOR_ERROR;
    }
}

KDADK_API void extractor_reset_statistics(feature_extractor *extractor)
{
    if (!extractor) {
        return;
    }
    memset(&extractor->stats, 0, sizeof(statistics));
}

KDADK_API void extractor_destroy(feature_extractor *extractor)
{
    if (!extractor) {
        return;
    }

    if (extractor->hash_table) {
        hash_table_destroy(extractor->hash_table);
    }
    if (extractor->domain_name_table) {
        domain_name_table_destroy(extractor->domain_name_table);
    }

    free(extractor);
}