/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "kdadk_multi_thread.h"
#include <cstdlib>
#include <cstring>

// 流哈希函数，基于5元组计算哈希值
static int calc_flow_hash(const u_char *packet, int packet_len, int thread_count) {
    // 检查参数
    if (!packet || packet_len <= 0 || thread_count <= 0) {
        return 0;
    }
    
    // 基于5元组计算哈希值，确保同一流的数据包被分配到同一个线程
    uint32_t hash = 0;
    
    // 尝试从数据包中提取5元组信息
    // 假设数据包包含以太网头(14字节) + IP头(至少20字节) + TCP/UDP头(至少20字节)
    if (packet_len >= 54) {
        // 跳过以太网头(14字节)
        const u_char *ip_header = packet + 14;
        
        // 检查IP版本
        uint8_t ip_version = (ip_header[0] >> 4) & 0x0F;
        if (ip_version == 4) {
            // IPv4
            uint8_t protocol = ip_header[9];
            uint32_t src_ip = *(uint32_t *)(ip_header + 12);
            uint32_t dst_ip = *(uint32_t *)(ip_header + 16);
            
            // 跳过IP头
            int ip_header_len = ((ip_header[0] & 0x0F) * 4);
            const u_char *transport_header = ip_header + ip_header_len;
            
            // 提取端口
            uint16_t src_port = 0;
            uint16_t dst_port = 0;
            
            if (protocol == 6 || protocol == 17) { // TCP or UDP
                src_port = *(uint16_t *)transport_header;
                dst_port = *(uint16_t *)(transport_header + 2);
            }
            
            // 计算哈希值，确保源和目标交换时哈希值相同
            if (src_ip > dst_ip || (src_ip == dst_ip && src_port > dst_port)) {
                hash = dst_ip ^ src_ip ^ (protocol << 24) ^ dst_port ^ src_port;
            } else {
                hash = src_ip ^ dst_ip ^ (protocol << 24) ^ src_port ^ dst_port;
            }
        } else if (ip_version == 6) {
            // IPv6 简化处理
            const u_char *src_ip = ip_header + 8;
            const u_char *dst_ip = ip_header + 24;
            uint8_t protocol = ip_header[6];
            
            // 计算哈希值
            for (int i = 0; i < 16; i++) {
                hash = hash * 31 + src_ip[i] + dst_ip[i];
            }
            hash ^= (protocol << 24);
        }
    }
    
    // 如果无法提取5元组，使用数据包内容计算哈希
    if (hash == 0) {
        int len = packet_len > 100 ? 100 : packet_len;
        for (int i = 0; i < len; i++) {
            hash = hash * 31 + packet[i];
        }
    }
    
    int thread_id = hash % thread_count;
    return thread_id;
}

// 工作线程函数
static void *worker_thread_func(void *arg) {
    // 检查参数是否有效
    if (!arg) {
        return NULL;
    }
    
    worker_thread *worker = (worker_thread *)arg;
    
    // 检查必要的指针
    if (!worker->extractor || !worker->features.vectors) {
        return NULL;
    }
    
    while (worker->running) {
        multi_thread_packet_info info;
        bool has_packet = false;
        
        // 从队列中获取数据包
        pthread_mutex_lock(&worker->queue_mutex);
        if (worker->queue_size > 0 && worker->queue_head) {
            // 获取队列头部节点
            packet_node *node = worker->queue_head;
            info = node->info;
            
            // 更新队列头
            worker->queue_head = node->next;
            if (!worker->queue_head) {
                worker->queue_tail = NULL;
            }
            worker->queue_size--;
            has_packet = true;
            
            // 释放节点
            free(node);
        } else {
            // 等待新数据包
            // printf("[DEBUG] 线程 %d: 等待新数据包\n", worker->thread_id);
            pthread_cond_wait(&worker->queue_cond, &worker->queue_mutex);
        }
        pthread_mutex_unlock(&worker->queue_mutex);
        
        if (has_packet) {
            // 处理数据包
            int ret;
            if (info.raw_payload) {
                // 处理带有 Rawbow 的数据包
                ret = process_packet_with_rawbow(worker->extractor, info.packet, info.packet_len, 
                                              &info.ts, info.features, info.raw_payload, info.has_feature);
            } else {
                // 处理普通数据包
                ret = process_packet(worker->extractor, info.packet, info.packet_len, 
                                   &info.ts, info.features, info.has_feature);
            }
            
            if (ret == EXTRACTOR_SUCCESS && *info.has_feature && worker->features.vectors) {
                // 保存特征向量和 Rawbow 数据
                pthread_mutex_lock(&worker->queue_mutex);
                if (worker->features.count >= worker->features.capacity) {
                    // 扩容
                    size_t new_capacity = worker->features.capacity * 2;
                    feature_vector *new_vectors = (feature_vector *)realloc(
                        worker->features.vectors, new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
                        worker->features.vectors = new_vectors;
                        worker->features.capacity = new_capacity;
                    } else {
                        // 扩容失败，释放内存并继续
                        pthread_mutex_unlock(&worker->queue_mutex);
                        free(info.packet);
                        free(info.features);
                        if (info.raw_payload) {
                            free(info.raw_payload);
                        }
                        free(info.has_feature);
                        continue;
                    }
                }
                if (worker->features.vectors) {
                    worker->features.vectors[worker->features.count++] = *info.features;
                }
                
                // 保存 Rawbow 数据
                if (info.raw_payload && worker->rawbows.rawbows) {
                    if (worker->rawbows.count >= worker->rawbows.capacity) {
                        // 扩容
                        size_t new_capacity = worker->rawbows.capacity * 2;
                        rawbow *new_rawbows = (rawbow *)realloc(
                            worker->rawbows.rawbows, new_capacity * sizeof(rawbow));
                        if (new_rawbows) {
                            worker->rawbows.rawbows = new_rawbows;
                            worker->rawbows.capacity = new_capacity;
                        }
                    }
                    if (worker->rawbows.rawbows) {
                        worker->rawbows.rawbows[worker->rawbows.count++] = *info.raw_payload;
                    }
                }
                pthread_mutex_unlock(&worker->queue_mutex);
            }
            
            // 释放内存
            free(info.packet);
            free(info.features);
            if (info.raw_payload) {
                free(info.raw_payload);
            }
            free(info.has_feature);
        }
    }
    
    // 处理队列中剩余的数据包
    pthread_mutex_lock(&worker->queue_mutex);
    packet_node *current = worker->queue_head;
    while (current) {
        packet_node *next = current->next;
        
        // 处理数据包
        multi_thread_packet_info info = current->info;
        int ret;
        if (info.raw_payload) {
            // 处理带有 Rawbow 的数据包
            ret = process_packet_with_rawbow(worker->extractor, info.packet, info.packet_len, 
                                          &info.ts, info.features, info.raw_payload, info.has_feature);
        } else {
            // 处理普通数据包
            ret = process_packet(worker->extractor, info.packet, info.packet_len, 
                               &info.ts, info.features, info.has_feature);
        }
        
        if (ret == EXTRACTOR_SUCCESS && *info.has_feature && worker->features.vectors) {
            // 保存特征向量
            if (worker->features.count >= worker->features.capacity) {
                // 扩容
                size_t new_capacity = worker->features.capacity * 2;
                feature_vector *new_vectors = (feature_vector *)realloc(
                    worker->features.vectors, new_capacity * sizeof(feature_vector));
                if (new_vectors) {
                    worker->features.vectors = new_vectors;
                    worker->features.capacity = new_capacity;
                }
            }
            if (worker->features.vectors) {
                worker->features.vectors[worker->features.count++] = *info.features;
            }
            
            // 保存 Rawbow 数据
            if (info.raw_payload && worker->rawbows.rawbows) {
                if (worker->rawbows.count >= worker->rawbows.capacity) {
                    // 扩容
                    size_t new_capacity = worker->rawbows.capacity * 2;
                    rawbow *new_rawbows = (rawbow *)realloc(
                        worker->rawbows.rawbows, new_capacity * sizeof(rawbow));
                    if (new_rawbows) {
                        worker->rawbows.rawbows = new_rawbows;
                        worker->rawbows.capacity = new_capacity;
                    }
                }
                if (worker->rawbows.rawbows) {
                    worker->rawbows.rawbows[worker->rawbows.count++] = *info.raw_payload;
                }
            }
        }
        
        // 释放数据包内存
        free(current->info.packet);
        free(current->info.features);
        if (current->info.raw_payload) {
            free(current->info.raw_payload);
        }
        free(current->info.has_feature);
        // 释放节点内存
        free(current);
        current = next;
    }
    
    // 重置队列
    worker->queue_head = NULL;
    worker->queue_tail = NULL;
    worker->queue_size = 0;
    pthread_mutex_unlock(&worker->queue_mutex);
    
    return NULL;
}

// 初始化多线程特征提取器
multi_thread_extractor *multi_extractor_init(int thread_count, int dlt) {
    if (thread_count <= 0) {
        thread_count = 4; // 默认4线程
    }
    
    // 自动决定是否启用共享 domain_name_table
    bool enable_shared_domain_table = (thread_count > 1);
    
    multi_thread_extractor *extractor = (multi_thread_extractor *)malloc(
        sizeof(multi_thread_extractor));
    if (!extractor) {
        return NULL;
    }
    
    extractor->thread_count = thread_count;
    extractor->workers = (worker_thread *)malloc(thread_count * sizeof(worker_thread));
    if (!extractor->workers) {
        free(extractor);
        return NULL;
    }
    
    pthread_mutex_init(&extractor->result_mutex, NULL);
    extractor->all_features.count = 0;
    extractor->all_features.capacity = 100000;
    extractor->all_features.vectors = (feature_vector *)malloc(
        extractor->all_features.capacity * sizeof(feature_vector));
    if (!extractor->all_features.vectors) {
        // 清理已分配的资源
        free(extractor->workers);
        pthread_mutex_destroy(&extractor->result_mutex);
        free(extractor);
        return NULL;
    }
    
    // 初始化 Rawbow 列表
    extractor->all_rawbows.count = 0;
    extractor->all_rawbows.capacity = 100000;
    extractor->all_rawbows.rawbows = (rawbow *)malloc(
        extractor->all_rawbows.capacity * sizeof(rawbow));
    if (!extractor->all_rawbows.rawbows) {
        // 清理已分配的资源
        free(extractor->workers);
        free(extractor->all_features.vectors);
        pthread_mutex_destroy(&extractor->result_mutex);
        free(extractor);
        return NULL;
    }
    
    // 创建共享域名表（如果启用）
    domain_name_hash *shared_domain_table = NULL;
    if (enable_shared_domain_table) {
        if (domain_name_table_create(&shared_domain_table) || !shared_domain_table) {
            // 清理已分配的资源
            free(extractor->workers);
            free(extractor->all_features.vectors);
            free(extractor->all_rawbows.rawbows);
            pthread_mutex_destroy(&extractor->result_mutex);
            free(extractor);
            return NULL;
        }
    }
    
    // 初始化工作线程
    for (int i = 0; i < thread_count; i++) {
        // 为每个 worker 分配独立的内存，确保内存对齐
        worker_thread *worker = &extractor->workers[i];
        
        // 初始化基本字段
        memset(worker, 0, sizeof(worker_thread));
        worker->thread_id = i;
        worker->running = false; // 先设置为 false，初始化完成后再设置为 true
        worker->queue_head = NULL;
        worker->queue_tail = NULL;
        worker->queue_size = 0;
        
        // 初始化特征提取器，传递共享域名表（如果启用）
        worker->extractor = extractor_init(dlt, shared_domain_table);
        if (!worker->extractor) {
            // 清理已创建的线程
            for (int j = 0; j < i; j++) {
                worker_thread *prev_worker = &extractor->workers[j];
                if (prev_worker->extractor) {
                    extractor_destroy(prev_worker->extractor);
                }
                if (prev_worker->features.vectors) {
                    free(prev_worker->features.vectors);
                }
                if (prev_worker->rawbows.rawbows) {
                    free(prev_worker->rawbows.rawbows);
                }
            }
            // 清理共享域名表（如果已创建）
            if (shared_domain_table) {
                domain_name_table_destroy(shared_domain_table);
            }
            free(extractor->workers);
            free(extractor->all_features.vectors);
            free(extractor->all_rawbows.rawbows);
            pthread_mutex_destroy(&extractor->result_mutex);
            free(extractor);
            return NULL;
        }
        
        // 初始化特征向量列表
        worker->features.count = 0;
        worker->features.capacity = 100000;
        worker->features.vectors = (feature_vector *)malloc(
            worker->features.capacity * sizeof(feature_vector));
        if (!worker->features.vectors) {
            // 清理已创建的线程
            for (int j = 0; j < i; j++) {
                worker_thread *prev_worker = &extractor->workers[j];
                if (prev_worker->extractor) {
                    extractor_destroy(prev_worker->extractor);
                }
                if (prev_worker->features.vectors) {
                    free(prev_worker->features.vectors);
                }
                if (prev_worker->rawbows.rawbows) {
                    free(prev_worker->rawbows.rawbows);
                }
            }
            // 清理当前线程
            extractor_destroy(worker->extractor);
            free(extractor->workers);
            free(extractor->all_features.vectors);
            free(extractor->all_rawbows.rawbows);
            pthread_mutex_destroy(&extractor->result_mutex);
            free(extractor);
            return NULL;
        }
        
        // 初始化 Rawbow 列表
        worker->rawbows.count = 0;
        worker->rawbows.capacity = 100000;
        worker->rawbows.rawbows = (rawbow *)malloc(
            worker->rawbows.capacity * sizeof(rawbow));
        if (!worker->rawbows.rawbows) {
            // 清理已创建的线程
            for (int j = 0; j <= i; j++) {
                worker_thread *prev_worker = &extractor->workers[j];
                if (prev_worker->extractor) {
                    extractor_destroy(prev_worker->extractor);
                }
                if (prev_worker->features.vectors) {
                    free(prev_worker->features.vectors);
                }
                if (prev_worker->rawbows.rawbows) {
                    free(prev_worker->rawbows.rawbows);
                }
            }
            free(extractor->workers);
            free(extractor->all_features.vectors);
            free(extractor->all_rawbows.rawbows);
            pthread_mutex_destroy(&extractor->result_mutex);
            free(extractor);
            return NULL;
        }
        
        // 初始化互斥锁
        pthread_mutexattr_t mutex_attr;
        if (pthread_mutexattr_init(&mutex_attr) != 0) {
            goto cleanup;
        }
        if (pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_NORMAL) != 0) {
            pthread_mutexattr_destroy(&mutex_attr);
            goto cleanup;
        }
        if (pthread_mutex_init(&worker->queue_mutex, &mutex_attr) != 0) {
            pthread_mutexattr_destroy(&mutex_attr);
            goto cleanup;
        }
        pthread_mutexattr_destroy(&mutex_attr);
        
        // 初始化条件变量
        pthread_condattr_t cond_attr;
        if (pthread_condattr_init(&cond_attr) != 0) {
            pthread_mutex_destroy(&worker->queue_mutex);
            goto cleanup;
        }
        if (pthread_cond_init(&worker->queue_cond, &cond_attr) != 0) {
            pthread_condattr_destroy(&cond_attr);
            pthread_mutex_destroy(&worker->queue_mutex);
            goto cleanup;
        }
        pthread_condattr_destroy(&cond_attr);
        
        // 设置运行状态
        worker->running = true;
        
        // 创建线程
        if (pthread_create(&worker->thread, NULL, worker_thread_func, worker) != 0) {
            worker->running = false;
            pthread_mutex_destroy(&worker->queue_mutex);
            pthread_cond_destroy(&worker->queue_cond);
            goto cleanup;
        }
        
        continue;
        
    cleanup:
        // 清理已创建的线程
        for (int j = 0; j <= i; j++) {
            worker_thread *cleanup_worker = &extractor->workers[j];
            if (cleanup_worker->extractor) {
                extractor_destroy(cleanup_worker->extractor);
            }
            if (cleanup_worker->features.vectors) {
                free(cleanup_worker->features.vectors);
            }
            if (cleanup_worker->rawbows.rawbows) {
                free(cleanup_worker->rawbows.rawbows);
            }
            // 尝试销毁互斥锁和条件变量（如果已初始化）
            pthread_mutex_destroy(&cleanup_worker->queue_mutex);
            pthread_cond_destroy(&cleanup_worker->queue_cond);
        }
        free(extractor->workers);
        free(extractor->all_features.vectors);
        free(extractor->all_rawbows.rawbows);
        pthread_mutex_destroy(&extractor->result_mutex);
        free(extractor);
        return NULL;
    }
    
    // 存储共享域名表指针和启用标志
    extractor->shared_domain_table = shared_domain_table;
    extractor->enable_shared_domain_table = enable_shared_domain_table;
    
    return extractor;
}

// 处理数据包
int multi_process_packet(multi_thread_extractor *extractor, const u_char *packet, 
                        int packet_len, const struct timeval *ts, 
                        feature_vector *features, int *has_feature) {
    static int packet_count = 0;
    if (!extractor || !packet || !ts || !features || !has_feature) {
        return EXTRACTOR_ERROR;
    }
    
    packet_count++;
    
    // 计算哈希值，确定线程
    int thread_id = calc_flow_hash(packet, packet_len, extractor->thread_count);
    
    // 检查线程ID是否有效
    if (thread_id < 0 || thread_id >= extractor->thread_count) {
        return EXTRACTOR_ERROR;
    }
    
    worker_thread *worker = &extractor->workers[thread_id];
    
    // 构造数据包信息
    multi_thread_packet_info info;
    // 复制数据包
    info.packet = (u_char *)malloc(packet_len);
    if (!info.packet) {
        return EXTRACTOR_ERROR;
    }
    memcpy(info.packet, packet, packet_len);
    info.packet_len = packet_len;
    info.ts = *ts;
    
    // 分配特征向量内存
    info.features = (feature_vector *)malloc(sizeof(feature_vector));
    if (!info.features) {
        free(info.packet);
        return EXTRACTOR_ERROR;
    }
    // 初始化特征向量
    memset(info.features, 0, sizeof(feature_vector));
    
    // 分配has_feature内存
    info.has_feature = (int *)malloc(sizeof(int));
    if (!info.has_feature) {
        free(info.packet);
        free(info.features);
        return EXTRACTOR_ERROR;
    }
    *info.has_feature = 0;
    
    // Rawbow 指针设为 NULL
    info.raw_payload = NULL;
    
    // 将数据包加入线程队列
    pthread_mutex_lock(&worker->queue_mutex);
    
    // 创建新节点
    packet_node *node = (packet_node *)malloc(sizeof(packet_node));
    if (!node) {
        pthread_mutex_unlock(&worker->queue_mutex);
        free(info.packet);
        free(info.features);
        free(info.has_feature);
        return EXTRACTOR_ERROR;
    }
    node->info = info;
    node->next = NULL;
    
    // 将节点加入队列
    if (!worker->queue_head) {
        worker->queue_head = node;
        worker->queue_tail = node;
    } else {
        worker->queue_tail->next = node;
        worker->queue_tail = node;
    }
    worker->queue_size++;
    
    pthread_cond_signal(&worker->queue_cond);
    pthread_mutex_unlock(&worker->queue_mutex);
    
    return EXTRACTOR_SUCCESS;
}

// 处理数据包（提取特征向量和 Rawbow）
int multi_process_packet_with_rawbow(multi_thread_extractor *extractor, const u_char *packet, 
                                   int packet_len, const struct timeval *ts, 
                                   feature_vector *features, rawbow *raw_payload, int *has_feature) {
    static int packet_count = 0;
    if (!extractor || !packet || !ts || !features || !raw_payload || !has_feature) {
        return EXTRACTOR_ERROR;
    }
    
    packet_count++;
    
    // 计算哈希值，确定线程
    int thread_id = calc_flow_hash(packet, packet_len, extractor->thread_count);
    
    // 检查线程ID是否有效
    if (thread_id < 0 || thread_id >= extractor->thread_count) {
        return EXTRACTOR_ERROR;
    }
    
    worker_thread *worker = &extractor->workers[thread_id];
    
    // 构造数据包信息
    multi_thread_packet_info info;
    // 复制数据包
    info.packet = (u_char *)malloc(packet_len);
    if (!info.packet) {
        return EXTRACTOR_ERROR;
    }
    memcpy(info.packet, packet, packet_len);
    info.packet_len = packet_len;
    info.ts = *ts;
    
    // 分配特征向量内存
    info.features = (feature_vector *)malloc(sizeof(feature_vector));
    if (!info.features) {
        free(info.packet);
        return EXTRACTOR_ERROR;
    }
    // 初始化特征向量
    memset(info.features, 0, sizeof(feature_vector));
    
    // 分配 Rawbow 内存
    info.raw_payload = (rawbow *)malloc(sizeof(rawbow));
    if (!info.raw_payload) {
        free(info.packet);
        free(info.features);
        return EXTRACTOR_ERROR;
    }
    // 初始化 Rawbow
    memset(info.raw_payload, 0, sizeof(rawbow));
    
    // 分配has_feature内存
    info.has_feature = (int *)malloc(sizeof(int));
    if (!info.has_feature) {
        free(info.packet);
        free(info.features);
        free(info.raw_payload);
        return EXTRACTOR_ERROR;
    }
    *info.has_feature = 0;
    
    // 将数据包加入线程队列
    pthread_mutex_lock(&worker->queue_mutex);
    
    // 创建新节点
    packet_node *node = (packet_node *)malloc(sizeof(packet_node));
    if (!node) {
        pthread_mutex_unlock(&worker->queue_mutex);
        free(info.packet);
        free(info.features);
        free(info.raw_payload);
        free(info.has_feature);
        return EXTRACTOR_ERROR;
    }
    node->info = info;
    node->next = NULL;
    
    // 将节点加入队列
    if (!worker->queue_head) {
        worker->queue_head = node;
        worker->queue_tail = node;
    } else {
        worker->queue_tail->next = node;
        worker->queue_tail = node;
    }
    worker->queue_size++;
    
    pthread_cond_signal(&worker->queue_cond);
    pthread_mutex_unlock(&worker->queue_mutex);
    
    return EXTRACTOR_SUCCESS;
}

// 完成特征提取
int multi_extractor_finalize(multi_thread_extractor *extractor, feature_vector_list *features) {
    if (!extractor || !features) {
        return EXTRACTOR_ERROR;
    }
    
    // 停止所有线程
    for (int i = 0; i < extractor->thread_count; i++) {
        worker_thread *worker = &extractor->workers[i];
        worker->running = false;
        pthread_cond_signal(&worker->queue_cond);
        pthread_join(worker->thread, NULL);
        
        // 合并线程已处理的特征
        pthread_mutex_lock(&worker->queue_mutex);
        pthread_mutex_lock(&extractor->result_mutex);
        if (extractor->all_features.vectors && worker->features.vectors) {
            for (uint32_t j = 0; j < worker->features.count; j++) {
                if (extractor->all_features.count >= extractor->all_features.capacity) {
                    // 扩容
                    size_t new_capacity = extractor->all_features.capacity * 2;
                    feature_vector *new_vectors = (feature_vector *)realloc(
                        extractor->all_features.vectors, new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
                        extractor->all_features.vectors = new_vectors;
                        extractor->all_features.capacity = new_capacity;
                    } else {
                        // 扩容失败，跳出循环
                        break;
                    }
                }
                extractor->all_features.vectors[extractor->all_features.count++] = worker->features.vectors[j];
            }
        }
        pthread_mutex_unlock(&extractor->result_mutex);
        pthread_mutex_unlock(&worker->queue_mutex);
        
        // 调用extractor_finalize获取剩余特征
        feature_vector_list remaining;
        remaining.count = 0;
        remaining.capacity = 100000;
        remaining.vectors = (feature_vector *)malloc(remaining.capacity * sizeof(feature_vector));
        
        if (remaining.vectors) {
            int ret = extractor_finalize(worker->extractor, &remaining);
            if (ret == EXTRACTOR_SUCCESS) {
                // 合并剩余特征
                pthread_mutex_lock(&extractor->result_mutex);
                if (extractor->all_features.vectors) {
                    for (uint32_t j = 0; j < remaining.count; j++) {
                        if (extractor->all_features.count >= extractor->all_features.capacity) {
                            // 扩容
                            size_t new_capacity = extractor->all_features.capacity * 2;
                            feature_vector *new_vectors = (feature_vector *)realloc(
                                extractor->all_features.vectors, new_capacity * sizeof(feature_vector));
                            if (new_vectors) {
                                extractor->all_features.vectors = new_vectors;
                                extractor->all_features.capacity = new_capacity;
                            } else {
                                // 扩容失败，跳出循环
                                break;
                            }
                        }
                        extractor->all_features.vectors[extractor->all_features.count++] = remaining.vectors[j];
                    }
                }
                pthread_mutex_unlock(&extractor->result_mutex);
            }
            free(remaining.vectors);
        }
    }
    
    // 复制结果到输出
    if (extractor->all_features.vectors) {
        features->count = extractor->all_features.count;
        features->capacity = extractor->all_features.capacity;
        features->vectors = extractor->all_features.vectors;
        
        // 注意：这里不释放all_features.vectors，由调用者负责释放
        extractor->all_features.vectors = NULL;
        extractor->all_features.count = 0;
        extractor->all_features.capacity = 0;
    } else {
        features->count = 0;
        features->capacity = 0;
        features->vectors = NULL;
    }
    return EXTRACTOR_SUCCESS;
}

// 完成特征提取（提取特征向量和 Rawbow）
int multi_extractor_finalize_with_rawbow(multi_thread_extractor *extractor, feature_vector_list *features, 
                                       rawbow_list *rawbows) {
    if (!extractor || !features || !rawbows) {
        return EXTRACTOR_ERROR;
    }
    
    // 停止所有线程
    for (int i = 0; i < extractor->thread_count; i++) {
        worker_thread *worker = &extractor->workers[i];
        worker->running = false;
        pthread_cond_signal(&worker->queue_cond);
        pthread_join(worker->thread, NULL);
        
        // 合并线程已处理的特征和 Rawbow
        pthread_mutex_lock(&worker->queue_mutex);
        pthread_mutex_lock(&extractor->result_mutex);
        if (extractor->all_features.vectors && worker->features.vectors) {
            for (uint32_t j = 0; j < worker->features.count; j++) {
                if (extractor->all_features.count >= extractor->all_features.capacity) {
                    // 扩容
                    size_t new_capacity = extractor->all_features.capacity * 2;
                    feature_vector *new_vectors = (feature_vector *)realloc(
                        extractor->all_features.vectors, new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
                        extractor->all_features.vectors = new_vectors;
                        extractor->all_features.capacity = new_capacity;
                    } else {
                        // 扩容失败，跳出循环
                        break;
                    }
                }
                extractor->all_features.vectors[extractor->all_features.count++] = worker->features.vectors[j];
            }
        }
        
        if (extractor->all_rawbows.rawbows && worker->rawbows.rawbows) {
            for (uint32_t j = 0; j < worker->rawbows.count; j++) {
                if (extractor->all_rawbows.count >= extractor->all_rawbows.capacity) {
                    // 扩容
                    size_t new_capacity = extractor->all_rawbows.capacity * 2;
                    rawbow *new_rawbows = (rawbow *)realloc(
                        extractor->all_rawbows.rawbows, new_capacity * sizeof(rawbow));
                    if (new_rawbows) {
                        extractor->all_rawbows.rawbows = new_rawbows;
                        extractor->all_rawbows.capacity = new_capacity;
                    } else {
                        // 扩容失败，跳出循环
                        break;
                    }
                }
                extractor->all_rawbows.rawbows[extractor->all_rawbows.count++] = worker->rawbows.rawbows[j];
            }
        }
        pthread_mutex_unlock(&extractor->result_mutex);
        pthread_mutex_unlock(&worker->queue_mutex);
        
        // 调用extractor_finalize_with_rawbow获取剩余特征和 Rawbow
        feature_vector_list remaining_features;
        rawbow_list remaining_rawbows;
        
        remaining_features.count = 0;
        remaining_features.capacity = 100000;
        remaining_features.vectors = (feature_vector *)malloc(remaining_features.capacity * sizeof(feature_vector));
        
        remaining_rawbows.count = 0;
        remaining_rawbows.capacity = 100000;
        remaining_rawbows.rawbows = (rawbow *)malloc(remaining_rawbows.capacity * sizeof(rawbow));
        
        if (remaining_features.vectors && remaining_rawbows.rawbows) {
            int ret = extractor_finalize_with_rawbow(worker->extractor, &remaining_features, &remaining_rawbows);
            if (ret == EXTRACTOR_SUCCESS) {
                // 合并剩余特征
                pthread_mutex_lock(&extractor->result_mutex);
                if (extractor->all_features.vectors) {
                    for (uint32_t j = 0; j < remaining_features.count; j++) {
                        if (extractor->all_features.count >= extractor->all_features.capacity) {
                            // 扩容
                            size_t new_capacity = extractor->all_features.capacity * 2;
                            feature_vector *new_vectors = (feature_vector *)realloc(
                                extractor->all_features.vectors, new_capacity * sizeof(feature_vector));
                            if (new_vectors) {
                                extractor->all_features.vectors = new_vectors;
                                extractor->all_features.capacity = new_capacity;
                            } else {
                                // 扩容失败，跳出循环
                                break;
                            }
                        }
                        extractor->all_features.vectors[extractor->all_features.count++] = remaining_features.vectors[j];
                    }
                }
                
                // 合并剩余 Rawbow
                if (extractor->all_rawbows.rawbows) {
                    for (uint32_t j = 0; j < remaining_rawbows.count; j++) {
                        if (extractor->all_rawbows.count >= extractor->all_rawbows.capacity) {
                            // 扩容
                            size_t new_capacity = extractor->all_rawbows.capacity * 2;
                            rawbow *new_rawbows = (rawbow *)realloc(
                                extractor->all_rawbows.rawbows, new_capacity * sizeof(rawbow));
                            if (new_rawbows) {
                                extractor->all_rawbows.rawbows = new_rawbows;
                                extractor->all_rawbows.capacity = new_capacity;
                            } else {
                                // 扩容失败，跳出循环
                                break;
                            }
                        }
                        extractor->all_rawbows.rawbows[extractor->all_rawbows.count++] = remaining_rawbows.rawbows[j];
                    }
                }
                pthread_mutex_unlock(&extractor->result_mutex);
            }
            free(remaining_features.vectors);
            free(remaining_rawbows.rawbows);
        }
    }
    
    // 复制结果到输出
    if (extractor->all_features.vectors) {
        features->count = extractor->all_features.count;
        features->capacity = extractor->all_features.capacity;
        features->vectors = extractor->all_features.vectors;
        
        // 注意：这里不释放all_features.vectors，由调用者负责释放
        extractor->all_features.vectors = NULL;
        extractor->all_features.count = 0;
        extractor->all_features.capacity = 0;
    } else {
        features->count = 0;
        features->capacity = 0;
        features->vectors = NULL;
    }
    
    if (extractor->all_rawbows.rawbows) {
        rawbows->count = extractor->all_rawbows.count;
        rawbows->capacity = extractor->all_rawbows.capacity;
        rawbows->rawbows = extractor->all_rawbows.rawbows;
        
        // 注意：这里不释放all_rawbows.rawbows，由调用者负责释放
        extractor->all_rawbows.rawbows = NULL;
        extractor->all_rawbows.count = 0;
        extractor->all_rawbows.capacity = 0;
    } else {
        rawbows->count = 0;
        rawbows->capacity = 0;
        rawbows->rawbows = NULL;
    }
    
    return EXTRACTOR_SUCCESS;
}

// 销毁多线程特征提取器
void multi_extractor_destroy(multi_thread_extractor *extractor) {
    if (!extractor) {
        return;
    }
    
    // 停止所有线程
    for (int i = 0; i < extractor->thread_count; i++) {
        worker_thread *worker = &extractor->workers[i];
        if (worker->running) {
            worker->running = false;
            pthread_cond_signal(&worker->queue_cond);
            pthread_join(worker->thread, NULL);
        }
        
        // 清理资源
        if (worker->extractor) {
            // 如果启用了共享域名表，临时将 domain_name_table 设为 NULL，避免被 extractor_destroy 销毁
            if (extractor->enable_shared_domain_table) {
                worker->extractor->domain_name_table = NULL;
            }
            extractor_destroy(worker->extractor);
        }
        if (worker->features.vectors) {
            free(worker->features.vectors);
        }
        if (worker->rawbows.rawbows) {
            free(worker->rawbows.rawbows);
        }
        pthread_mutex_destroy(&worker->queue_mutex);
        pthread_cond_destroy(&worker->queue_cond);
    }
    
    // 销毁共享域名表（如果启用）
    if (extractor->enable_shared_domain_table && extractor->shared_domain_table) {
        domain_name_table_destroy(extractor->shared_domain_table);
    }
    
    // 清理结果内存
    if (extractor->all_features.vectors) {
        free(extractor->all_features.vectors);
    }
    if (extractor->all_rawbows.rawbows) {
        free(extractor->all_rawbows.rawbows);
    }
    
    pthread_mutex_destroy(&extractor->result_mutex);
    free(extractor->workers);
    free(extractor);
}