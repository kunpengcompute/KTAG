/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 */

#include "kdadk_multi_thread.h"
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

// 内存池配置
#define MAX_POOL_SIZE 20000
#define MAX_PACKET_SIZE 65536 // 最大数据包大小

// 定义包缓冲区结构体
typedef struct {
    u_char data[MAX_PACKET_SIZE];
} packet_buffer_t;

// 快速内存池模板类 - 线程本地
template <typename T, size_t MaxSize = MAX_POOL_SIZE>
class FastMemoryPool {
private:
    union Node {
        T data;
        Node* next;
    };

    Node* pool_storage; // 预分配的大块内存
    Node* free_list;    
    size_t capacity;

public:
    FastMemoryPool() : capacity(MaxSize) {
        pool_storage = (Node*)malloc(sizeof(Node) * MaxSize);
        if (!pool_storage) {
            free_list = nullptr;
            return;
        }

        // 初始化链表，将所有节点串联起来
        for (size_t i = 0; i < MaxSize - 1; ++i) {
            pool_storage[i].next = &pool_storage[i+1];
        }
        pool_storage[MaxSize - 1].next = nullptr;
        free_list = pool_storage;
    }

    ~FastMemoryPool() {
        if (pool_storage) free(pool_storage);
    }

    inline T* allocate() {
        if (free_list != nullptr) {
            Node* node = free_list;
            free_list = node->next;
            return &(node->data);
        }
        // 如果池子干了，走慢速路径
        return (T*)malloc(sizeof(T));
    }

    inline void deallocate(T* ptr) {
        if (__builtin_expect(ptr >= (T*)pool_storage && ptr < (T*)(pool_storage + capacity), 1)) {
            Node* node = (Node*)ptr;
            node->next = free_list;
            free_list = node;
        } else {
            // 非池内内存直接释放
            free(ptr);
        }
    }
};

typedef struct {
    packet_buffer_t packet;      // 数据包 Buffer
    feature_vector features;     // 特征
    int has_feature;             // 标志位
    rawbow raw_payload;          // Rawbow
} packet_work_item_t;

static FastMemoryPool<packet_work_item_t, 20000uL> main_work_pond;

// 无锁循环队列模板类
template <typename T, size_t Capacity>
class LockFreeCircularQueue {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of 2");

private:
    static const size_t MASK = Capacity - 1;
    T buffer[Capacity];

    // ================= 生产者独占区域 =================
    // 使用 alignas(64) 将变量强制对其到 64 字节（典型的 CPU Cache Line 大小），彻底消除伪共享
    alignas(64) std::atomic<size_t> tail{0};  
    size_t cached_head_for_producer{0}; // 生产者本地缓存的 head（非原子变量，极快）

    // ================= 消费者独占区域 =================
    alignas(64) std::atomic<size_t> head{0};  
    size_t cached_tail_for_consumer{0}; // 消费者本地缓存的 tail（非原子变量，极快）

public:
    LockFreeCircularQueue() = default;

    // 入队操作
    bool enqueue(const T& item) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & MASK; // 使用 & MASK 替代 % CAPACITY

        // 核心优化：只在本地缓存显示队列已满时，才去读取原子的 head 进行确认
        if (next_tail == cached_head_for_producer) {
            cached_head_for_producer = head.load(std::memory_order_acquire);
            if (next_tail == cached_head_for_producer) {
                return false;
            }
        }
        
        buffer[current_tail] = item;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    // 出队操作
    bool dequeue(T& item) {
        size_t current_head = head.load(std::memory_order_relaxed);
        
        // 核心优化：只在本地缓存显示队列为空时，才去读取原子的 tail 进行确认
        if (current_head == cached_tail_for_consumer) {
            cached_tail_for_consumer = tail.load(std::memory_order_acquire);
            if (current_head == cached_tail_for_consumer) {
                return false;
            }
        }
        
        item = buffer[current_head];
        head.store((current_head + 1) & MASK, std::memory_order_release);
        return true;
    }

    // 批量出队
    size_t dequeue_batch(T* items, size_t max_count) {
        size_t current_head = head.load(std::memory_order_relaxed);
        
        if (current_head == cached_tail_for_consumer) {
            cached_tail_for_consumer = tail.load(std::memory_order_acquire);
            if (current_head == cached_tail_for_consumer) {
                return 0;
            }
        }
        
        size_t count = 0;
        while (current_head != cached_tail_for_consumer && count < max_count) {
            items[count++] = buffer[current_head];
            current_head = (current_head + 1) & MASK;
        }
        
        head.store(current_head, std::memory_order_release);
        return count;
    }

    // 检查队列是否为空
    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }
};

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
    
    int thread_id = 0;
    if (thread_count > 0) {
        thread_id = hash % thread_count;
    }
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
        packet_batch batch;
        bool processed = false;
        
        // 批量出队：一次拿到一批包
        while (worker->queue->dequeue(batch)) {
            processed = true;
            
            // 在子线程本地连续处理这批包
            for (size_t i = 0; i < batch.count; ++i) {
                multi_thread_packet_info& info = batch.pkts[i];
                
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
                    if (worker->features.count >= worker->features.capacity) {
                    // 扩容
                    size_t new_capacity = worker->features.capacity * 2;
                    feature_vector *new_vectors = (feature_vector *)malloc(
                        new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
                        memcpy(new_vectors, worker->features.vectors, 
                               worker->features.count * sizeof(feature_vector));
                        free(worker->features.vectors);
                        worker->features.vectors = new_vectors;
                        worker->features.capacity = new_capacity;
                    } else {
                        // 扩容失败，跳过此数据包
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
                        rawbow *new_rawbows = (rawbow *)malloc(
                            new_capacity * sizeof(rawbow));
                        if (new_rawbows) {
                            memcpy(new_rawbows, worker->rawbows.rawbows, 
                                   worker->rawbows.count * sizeof(rawbow));
                            free(worker->rawbows.rawbows);
                            worker->rawbows.rawbows = new_rawbows;
                            worker->rawbows.capacity = new_capacity;
                        }
                    }
                    if (worker->rawbows.rawbows) {
                        worker->rawbows.rawbows[worker->rawbows.count++] = *info.raw_payload;
                    }
                }
                }
            }
            
            // 处理完整个 batch 后，把整个 batch 一次性扔回回收队列
            while (!worker->recycle_queue->enqueue(batch)) {
                std::this_thread::yield();
            }
        }
        
        if (!processed) {
            // 队列为空，短暂休眠
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    }
    
    // 处理队列中剩余的数据包
    packet_batch batch;
    while (worker->queue->dequeue(batch)) {
        // 在子线程本地连续处理这批包
        for (size_t i = 0; i < batch.count; ++i) {
            multi_thread_packet_info& info = batch.pkts[i];
            
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
                // 保存特征向量
                if (worker->features.count >= worker->features.capacity) {
                    // 扩容
                    size_t new_capacity = worker->features.capacity * 2;
                    feature_vector *new_vectors = (feature_vector *)malloc(
                        new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
                        memcpy(new_vectors, worker->features.vectors, 
                               worker->features.count * sizeof(feature_vector));
                        free(worker->features.vectors);
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
                        rawbow *new_rawbows = (rawbow *)malloc(
                            new_capacity * sizeof(rawbow));
                        if (new_rawbows) {
                            memcpy(new_rawbows, worker->rawbows.rawbows, 
                                   worker->rawbows.count * sizeof(rawbow));
                            free(worker->rawbows.rawbows);
                            worker->rawbows.rawbows = new_rawbows;
                            worker->rawbows.capacity = new_capacity;
                        }
                    }
                    if (worker->rawbows.rawbows) {
                        worker->rawbows.rawbows[worker->rawbows.count++] = *info.raw_payload;
                    }
                }
            }
        }
        
        // 处理完整个 batch 后，把整个 batch 一次性扔回回收队列
        while (!worker->recycle_queue->enqueue(batch)) {
            std::this_thread::yield();
        }
    }
    
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
        
        // 创建无锁循环队列
        worker->queue = new LockFreeCircularQueue<packet_batch, 256>();
        if (!worker->queue) {
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
                if (prev_worker->queue) {
                    delete prev_worker->queue;
                }
                if (prev_worker->recycle_queue) {
                    delete prev_worker->recycle_queue;
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
        
        // 创建回收队列
        worker->recycle_queue = new LockFreeCircularQueue<packet_batch, 256>();
        if (!worker->recycle_queue) {
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
                if (prev_worker->queue) {
                    delete prev_worker->queue;
                }
                if (prev_worker->recycle_queue) {
                    delete prev_worker->recycle_queue;
                }
            }
            // 清理当前线程
            if (worker->queue) {
                delete worker->queue;
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
                if (prev_worker->queue) {
                    delete prev_worker->queue;
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
                if (prev_worker->queue) {
                    delete prev_worker->queue;
                }
            }
            // 清理当前线程
            extractor_destroy(worker->extractor);
            if (worker->queue) {
                delete worker->queue;
            }
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
                if (prev_worker->queue) {
                    delete prev_worker->queue;
                }
            }
            free(extractor->workers);
            free(extractor->all_features.vectors);
            free(extractor->all_rawbows.rawbows);
            pthread_mutex_destroy(&extractor->result_mutex);
            free(extractor);
            return NULL;
        }
        // 设置运行状态
        worker->running = true;
        
        // 创建线程
        if (pthread_create(&worker->thread, NULL, worker_thread_func, worker) != 0) {
            worker->running = false;
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
            // 无锁队列不需要销毁互斥锁和条件变量
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
    
    // 将回收逻辑封装成一个 Lambda，随时可以调用 
    auto recycle_memory_func = [&]() {
        packet_batch recycled_batch;
        for (int i = 0; i < extractor->thread_count; i++) {
            // 一次拿回一批垃圾
            while (extractor->workers[i].recycle_queue->dequeue(recycled_batch)) {
                // 本地循环清理
                for (size_t j = 0; j < recycled_batch.count; ++j) {
                    multi_thread_packet_info& recycled_info = recycled_batch.pkts[j];
                    if (recycled_info.packet) {
                        if (recycled_info.packet_len <= MAX_PACKET_SIZE) {
                            main_work_pond.deallocate((packet_work_item_t*)recycled_info.packet);
                        } else {
                            free(recycled_info.packet);
                        }
                    }
                }
            }
        }
    };

    // 构造数据包信息
    multi_thread_packet_info info;
    packet_work_item_t* item = main_work_pond.allocate();
    if (!item) return EXTRACTOR_ERROR;
    
    // 1. 拷数据包
    if (packet_len <= MAX_PACKET_SIZE) {
        memcpy(item->packet.data, packet, packet_len);
        info.packet = item->packet.data;
    } else {
        info.packet = (u_char*)malloc(packet_len);
        if (!info.packet) {
            main_work_pond.deallocate(item);
            return EXTRACTOR_ERROR;
        }
        memcpy(info.packet, packet, packet_len);
    }
    
    info.packet_len = packet_len;
    info.ts = *ts;
    
    info.features = &item->features;      // 取大结构体内部成员的地址
    info.has_feature = &item->has_feature;
    info.raw_payload = &item->raw_payload;

    // 初始化内部成员
    item->has_feature = 0;

    // 替换为：批量暂存与提交逻辑
    worker->staging_batch.pkts[worker->staging_batch.count++] = info;

    // 如果攒够了一批（64个），一次性提交给子线程
    if (worker->staging_batch.count == BATCH_SIZE) {
        recycle_memory_func();
        
        int retry_count = 0;
        while (!worker->queue->enqueue(worker->staging_batch)) {
            // 队列满时，持续回收直到有空间
            recycle_memory_func();
            std::this_thread::yield();
            retry_count++;
            if (retry_count > 100) {
                // 重试次数过多，释放资源并返回错误
                for (size_t i = 0; i < worker->staging_batch.count; i++) {
                    multi_thread_packet_info& batch_info = worker->staging_batch.pkts[i];
                    if (batch_info.packet) {
                        if (batch_info.packet_len <= MAX_PACKET_SIZE) {
                            main_work_pond.deallocate((packet_work_item_t*)batch_info.packet);
                        } else {
                            free(batch_info.packet);
                        }
                    }
                }
                worker->staging_batch.count = 0;
                return EXTRACTOR_ERROR;
            }
        }
        // 提交成功，重置计数器，开始攒下一批
        worker->staging_batch.count = 0;
    }
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
    
    // 将回收逻辑封装成一个 Lambda，随时可以调用 
    auto recycle_memory_func = [&]() {
        packet_batch recycled_batch;
        for (int i = 0; i < extractor->thread_count; i++) {
            // 一次拿回一批垃圾
            while (extractor->workers[i].recycle_queue->dequeue(recycled_batch)) {
                // 本地循环清理
                for (size_t j = 0; j < recycled_batch.count; ++j) {
                    multi_thread_packet_info& recycled_info = recycled_batch.pkts[j];
                    if (recycled_info.packet) {
                        if (recycled_info.packet_len <= MAX_PACKET_SIZE) {
                            // 因为 packet 字段在 packet_work_item_t 的最开头
                            // info.packet 刚好就是整个大结构体的起始地址
                            main_work_pond.deallocate((packet_work_item_t*)recycled_info.packet);
                        } else {
                            free(recycled_info.packet);
                        }
                    }
                }
            }
        }
    };
    // 构造数据包信息
    multi_thread_packet_info info;
    
    packet_work_item_t* item = main_work_pond.allocate();
    if (!item) return EXTRACTOR_ERROR;
    
    // 1. 拷数据包
    if (packet_len <= MAX_PACKET_SIZE) {
        memcpy(item->packet.data, packet, packet_len);
        info.packet = item->packet.data;
    } else {
        info.packet = (u_char*)malloc(packet_len);
        if (!info.packet) {
            main_work_pond.deallocate(item);
            return EXTRACTOR_ERROR;
        }
        memcpy(info.packet, packet, packet_len);
    }
    
    info.packet_len = packet_len;
    info.ts = *ts;

    info.features = &item->features;      // 取大结构体内部成员的地址
    info.has_feature = &item->has_feature;
    info.raw_payload = &item->raw_payload;
    
    // 初始化内部成员
    item->has_feature = 0;
    
    worker->staging_batch.pkts[worker->staging_batch.count++] = info;

    // 如果攒够了一批（64个），一次性提交给子线程
    if (worker->staging_batch.count == BATCH_SIZE) {
        recycle_memory_func();
        
        int retry_count = 0;
        while (!worker->queue->enqueue(worker->staging_batch)) {
            // 队列满时，持续回收直到有空间
            recycle_memory_func();
            std::this_thread::yield();
            retry_count++;
            if (retry_count > 100) {
                // 重试次数过多，释放资源并返回错误
                for (size_t i = 0; i < worker->staging_batch.count; i++) {
                    multi_thread_packet_info& batch_info = worker->staging_batch.pkts[i];
                    if (batch_info.packet) {
                        if (batch_info.packet_len <= MAX_PACKET_SIZE) {
                            // 因为 packet 字段在 packet_work_item_t 的最开头
                            // info.packet 刚好就是整个大结构体的起始地址
                            main_work_pond.deallocate((packet_work_item_t*)batch_info.packet);
                        } else {
                            free(batch_info.packet);
                        }
                    }
                }
                worker->staging_batch.count = 0;
                return EXTRACTOR_ERROR;
            }
        }
        // 提交成功，重置计数器，开始攒下一批
        worker->staging_batch.count = 0;
    }
    return EXTRACTOR_SUCCESS;
}

// 完成特征提取
int multi_extractor_finalize(multi_thread_extractor *extractor, feature_vector_list *features) {
    if (!extractor || !features) {
        return EXTRACTOR_ERROR;
    }
    
    // 强制推送所有未满的 batch
    for (int i = 0; i < extractor->thread_count; i++) {
        worker_thread *worker = &extractor->workers[i];
        if (worker->staging_batch.count > 0) {
            while (!worker->queue->enqueue(worker->staging_batch)) {
                std::this_thread::yield(); // 等待子线程消化
            }
            worker->staging_batch.count = 0;
        }
    }
    
    // 停止所有线程
    for (int i = 0; i < extractor->thread_count; i++) {
        worker_thread *worker = &extractor->workers[i];
        worker->running = false;
        pthread_join(worker->thread, NULL);
        
        // 合并线程已处理的特征
        pthread_mutex_lock(&extractor->result_mutex);
        if (extractor->all_features.vectors && worker->features.vectors) {
            for (uint32_t j = 0; j < worker->features.count; j++) {
                if (extractor->all_features.count >= extractor->all_features.capacity) {
                    // 扩容
                    size_t new_capacity = extractor->all_features.capacity * 2;
                    feature_vector *new_vectors = (feature_vector *)malloc(
                        new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
                        memcpy(new_vectors, extractor->all_features.vectors, 
                               extractor->all_features.count * sizeof(feature_vector));
                        free(extractor->all_features.vectors);
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
                            feature_vector *new_vectors = (feature_vector *)malloc(
                                new_capacity * sizeof(feature_vector));
                            if (new_vectors) {
                                memcpy(new_vectors, extractor->all_features.vectors, 
                                       extractor->all_features.count * sizeof(feature_vector));
                                free(extractor->all_features.vectors);
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
    
    // 强制推送所有未满的 batch
    for (int i = 0; i < extractor->thread_count; i++) {
        worker_thread *worker = &extractor->workers[i];
        if (worker->staging_batch.count > 0) {
            while (!worker->queue->enqueue(worker->staging_batch)) {
                std::this_thread::yield(); // 等待子线程消化
            }
            worker->staging_batch.count = 0;
        }
    }
    
    // 停止所有线程
    for (int i = 0; i < extractor->thread_count; i++) {
        worker_thread *worker = &extractor->workers[i];
        worker->running = false;
        pthread_join(worker->thread, NULL);
        
        // 合并线程已处理的特征和 Rawbow
        pthread_mutex_lock(&extractor->result_mutex);
        if (extractor->all_features.vectors && worker->features.vectors) {
            for (uint32_t j = 0; j < worker->features.count; j++) {
                if (extractor->all_features.count >= extractor->all_features.capacity) {
                    // 扩容
                    size_t new_capacity = extractor->all_features.capacity * 2;
                    feature_vector *new_vectors = (feature_vector *)malloc(
                        new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
                        memcpy(new_vectors, extractor->all_features.vectors, 
                               extractor->all_features.count * sizeof(feature_vector));
                        free(extractor->all_features.vectors);
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
                    rawbow *new_rawbows = (rawbow *)malloc(
                        new_capacity * sizeof(rawbow));
                    if (new_rawbows) {
                        memcpy(new_rawbows, extractor->all_rawbows.rawbows, 
                               extractor->all_rawbows.count * sizeof(rawbow));
                        free(extractor->all_rawbows.rawbows);
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
                            feature_vector *new_vectors = (feature_vector *)malloc(
                                new_capacity * sizeof(feature_vector));
                            if (new_vectors) {
                                memcpy(new_vectors, extractor->all_features.vectors, 
                                       extractor->all_features.count * sizeof(feature_vector));
                                free(extractor->all_features.vectors);
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
                            rawbow *new_rawbows = (rawbow *)malloc(
                                new_capacity * sizeof(rawbow));
                            if (new_rawbows) {
                                memcpy(new_rawbows, extractor->all_rawbows.rawbows, 
                                       extractor->all_rawbows.count * sizeof(rawbow));
                                free(extractor->all_rawbows.rawbows);
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
        // 销毁无锁队列
        if (worker->queue) {
            delete worker->queue;
        }
        // 销毁回收队列
        if (worker->recycle_queue) {
            delete worker->recycle_queue;
        }
        // 无锁队列不需要销毁互斥锁和条件变量
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