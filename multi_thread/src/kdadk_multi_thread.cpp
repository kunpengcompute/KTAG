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
#define MAX_POOL_SIZE 10000
#define POOL_ITEM_COUNT 1024  // 每个线程的预分配池大小

// 内存池模板类
template <typename T>
class MemoryPool {
private:
    std::vector<T* > free_blocks;
    const size_t max_size;
    const size_t block_size;

public:
    MemoryPool(size_t max_pool_size = MAX_POOL_SIZE, size_t size = sizeof(T)) : 
        max_size(max_pool_size), block_size(size) {}
    
    ~MemoryPool() {
        for (auto block : free_blocks) {
            free(block);
        }
    }
    
    T* allocate() {
        if (!free_blocks.empty()) {
            T* block = free_blocks.back();
            free_blocks.pop_back();
            return block;
        }
        return (T*)malloc(block_size);
    }
    
    void deallocate(T* block) {
        if (block) {
            if (free_blocks.size() < max_size) {
                free_blocks.push_back(block);
            } else {
                free(block);
            }
        }
    }
};

// 数据包内存池管理类
class PacketMemoryPool {
private:
    static const int BUCKET_COUNT = 6;
    static const size_t BUCKET_SIZES[BUCKET_COUNT];
    
    MemoryPool<u_char>* buckets[BUCKET_COUNT];
    
public:
    PacketMemoryPool() {
        // 初始化各个桶的内存池
        for (int i = 0; i < BUCKET_COUNT; i++) {
            buckets[i] = new MemoryPool<u_char>(MAX_POOL_SIZE, BUCKET_SIZES[i]);
        }
    }
    
    ~PacketMemoryPool() {
        for (int i = 0; i < BUCKET_COUNT; i++) {
            delete buckets[i];
        }
    }
    
    u_char* allocate(int size) {
        // 找到合适的桶
        int bucket_index = get_bucket_index(size);
        if (bucket_index < BUCKET_COUNT) {
            // 从对应桶的内存池分配
            u_char* buffer = buckets[bucket_index]->allocate();
            return buffer;
        } else {
            // 大数据包直接 malloc
            return (u_char*)malloc(size);
        }
    }
    
    void deallocate(u_char* buffer, int size) {
        if (buffer) {
            int bucket_index = get_bucket_index(size);
            if (bucket_index < BUCKET_COUNT) {
                // 释放到对应桶的内存池
                buckets[bucket_index]->deallocate(buffer);
            } else {
                // 大数据包直接 free
                free(buffer);
            }
        }
    }
    
private:
    int get_bucket_index(int size) {
        if (size <= 256) return 0;
        if (size <= 512) return 1;
        if (size <= 1024) return 2;
        if (size <= 2048) return 3;
        if (size <= 4096) return 4;
        if (size <= 8192) return 5;
        return BUCKET_COUNT;
    }
};

// 桶大小定义
const size_t PacketMemoryPool::BUCKET_SIZES[PacketMemoryPool::BUCKET_COUNT] = {256, 512, 1024, 2048, 4096, 8192};

// 内存池实例
static PacketMemoryPool packet_memory_pool;
static MemoryPool<feature_vector> feature_pool;
static MemoryPool<rawbow> rawbow_pool;
static MemoryPool<int> has_feature_pool;

// 无锁循环队列模板类
template <typename T, size_t Capacity>
class LockFreeCircularQueue {
private:
    static const size_t CAPACITY = Capacity;
    T buffer[CAPACITY];
    std::atomic<size_t> head;  // 出队位置
    std::atomic<size_t> tail;  // 入队位置

public:
    LockFreeCircularQueue() : head(0), tail(0) {}

    // 入队操作（多生产者）
    bool enqueue(const T& item) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % CAPACITY;
        
        // 检查队列是否已满
        if (next_tail == head.load(std::memory_order_acquire)) {
            return false;  // 队列满
        }
        
        // 存储元素
        buffer[current_tail] = item;
        
        // 原子更新 tail 指针
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    // 出队操作（单消费者）
    bool dequeue(T& item) {
        size_t current_head = head.load(std::memory_order_relaxed);
        
        // 检查队列是否为空
        if (current_head == tail.load(std::memory_order_acquire)) {
            return false;  // 队列空
        }
        
        // 获取元素
        item = buffer[current_head];
        
        // 原子更新 head 指针
        head.store((current_head + 1) % CAPACITY, std::memory_order_release);
        return true;
    }

    // 批量出队
    size_t dequeue_batch(T* items, size_t max_count) {
        size_t current_head = head.load(std::memory_order_relaxed);
        size_t current_tail = tail.load(std::memory_order_acquire);
        
        if (current_head == current_tail) {
            return 0;  // 队列空
        }
        
        size_t count = 0;
        while (current_head != current_tail && count < max_count) {
            items[count++] = buffer[current_head];
            current_head = (current_head + 1) % CAPACITY;
        }
        
        // 原子更新 head 指针
        head.store(current_head, std::memory_order_release);
        return count;
    }

    // 检查队列是否为空
    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }

    // 检查队列是否已满
    bool full() const {
        size_t current_tail = tail.load(std::memory_order_acquire);
        size_t next_tail = (current_tail + 1) % CAPACITY;
        return next_tail == head.load(std::memory_order_acquire);
    }
};

// 无锁回收队列模板类
template <typename T, size_t Capacity>
class LockFreeRecycleQueue {
private:
    static const size_t CAPACITY = Capacity;
    T* buffer[CAPACITY];
    std::atomic<size_t> head;
    std::atomic<size_t> tail;

public:
    LockFreeRecycleQueue() : head(0), tail(0) {}

    bool push(T* item) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % CAPACITY;
        
        if (next_tail == head.load(std::memory_order_acquire)) {
            return false;  // 队列满
        }
        
        buffer[current_tail] = item;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T*& item) {
        size_t current_head = head.load(std::memory_order_relaxed);
        
        if (current_head == tail.load(std::memory_order_acquire)) {
            return false;  // 队列空
        }
        
        item = buffer[current_head];
        head.store((current_head + 1) % CAPACITY, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }
};

// 线程本地内存池类
class ThreadLocalPool {
private:
    struct PoolItem {
        multi_thread_packet_info info;
        bool in_use;
    };
    
    std::vector<PoolItem> items;
    int next_index;
    LockFreeRecycleQueue<multi_thread_packet_info, POOL_ITEM_COUNT> recycle_queue;

public:
    ThreadLocalPool() : items(POOL_ITEM_COUNT), next_index(0) {
        // 预分配内存块
        for (size_t i = 0; i < POOL_ITEM_COUNT; i++) {
            // 初始化 info 结构
            items[i].info.packet = nullptr;
            items[i].info.features = nullptr;
            items[i].info.raw_payload = nullptr;
            items[i].info.has_feature = nullptr;
            items[i].in_use = false;
        }
    }
    
    ~ThreadLocalPool() {
        // 释放所有内存
        for (size_t i = 0; i < POOL_ITEM_COUNT; i++) {
            // 只有当指针不为nullptr时才释放
            if (items[i].info.packet) {
                free(items[i].info.packet);
                items[i].info.packet = nullptr;
            }
            if (items[i].info.features) {
                free(items[i].info.features);
                items[i].info.features = nullptr;
            }
            if (items[i].info.raw_payload) {
                free(items[i].info.raw_payload);
                items[i].info.raw_payload = nullptr;
            }
            if (items[i].info.has_feature) {
                free(items[i].info.has_feature);
                items[i].info.has_feature = nullptr;
            }
        }
    }
    
    // 分配操作（仅主线程调用）
    multi_thread_packet_info* allocate() {
        // 先尝试从回收队列获取
        multi_thread_packet_info* info = nullptr;
        if (recycle_queue.pop(info)) {
            return info;
        }
        
        // 从预分配池获取
        for (int i = 0; i < POOL_ITEM_COUNT; i++) {
            int idx = (next_index + i) % POOL_ITEM_COUNT;
            if (!items[idx].in_use) {
                items[idx].in_use = true;
                next_index = (idx + 1) % POOL_ITEM_COUNT;
                return &items[idx].info;
            }
        }
        
        // 池满，返回 nullptr
        return nullptr;
    }
    
    // 回收操作（子线程调用）
    bool recycle(multi_thread_packet_info* info) {
        // 释放内部内存
        if (info->packet) {
            free(info->packet);
            info->packet = nullptr;
        }
        if (info->features) {
            free(info->features);
            info->features = nullptr;
        }
        if (info->raw_payload) {
            free(info->raw_payload);
            info->raw_payload = nullptr;
        }
        if (info->has_feature) {
            free(info->has_feature);
            info->has_feature = nullptr;
        }
        
        return recycle_queue.push(info);
    }
    
    // 清理操作（主线程调用）
    void cleanup() {
        multi_thread_packet_info* info = nullptr;
        while (recycle_queue.pop(info)) {
            // 找到对应的 PoolItem 并标记为未使用
            for (size_t i = 0; i < POOL_ITEM_COUNT; i++) {
                if (&items[i].info == info) {
                    items[i].in_use = false;
                    // 确保指针已经被设置为nullptr
                    items[i].info.packet = nullptr;
                    items[i].info.features = nullptr;
                    items[i].info.raw_payload = nullptr;
                    items[i].info.has_feature = nullptr;
                    break;
                }
            }
        }
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
        // 单条出队
        multi_thread_packet_info info;
        bool processed = false;
        
        // 持续处理队列中的数据包，直到队列为空
        while (worker->queue->dequeue(info)) {
            processed = true;
            
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
                    feature_vector *new_vectors = (feature_vector *)realloc(
                        worker->features.vectors, new_capacity * sizeof(feature_vector));
                    if (new_vectors) {
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
            
            // 将内存通过回收队列返回给主线程
            while (!worker->recycle_queue->enqueue(info)) {
                // 如果回收队列满了，短暂yield
                std::this_thread::yield();
            }
        }
        
        if (!processed) {
            // 队列为空，短暂休眠
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    }
    
    // 处理队列中剩余的数据包
    multi_thread_packet_info info;
    while (worker->queue->dequeue(info)) {
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
        
        // 将内存通过回收队列返回给主线程
        while (!worker->recycle_queue->enqueue(info)) {
            // 如果回收队列满了，短暂yield
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
        worker->queue = new LockFreeCircularQueue<multi_thread_packet_info, 1024>();
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
        worker->recycle_queue = new LockFreeCircularQueue<multi_thread_packet_info, 1024>();
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
        
        // 无锁队列不需要互斥锁和条件变量
        
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
        multi_thread_packet_info recycled_info;
        for (int i = 0; i < extractor->thread_count; i++) {
            while (extractor->workers[i].recycle_queue->dequeue(recycled_info)) {
                if (recycled_info.packet) packet_memory_pool.deallocate(recycled_info.packet, recycled_info.packet_len);
                if (recycled_info.features) feature_pool.deallocate(recycled_info.features);
                if (recycled_info.raw_payload) rawbow_pool.deallocate(recycled_info.raw_payload);
                if (recycled_info.has_feature) has_feature_pool.deallocate(recycled_info.has_feature);
            }
        }
    };

    // 先执行一次回收，保证池子里有可用内存
    recycle_memory_func();
    
    // 构造数据包信息
    multi_thread_packet_info info;
    
    // 复制数据包
    info.packet = packet_memory_pool.allocate(packet_len);
    if (!info.packet) {
        return EXTRACTOR_ERROR;
    }
    memcpy(info.packet, packet, packet_len);
    info.packet_len = packet_len;
    info.ts = *ts;
    
    // 分配特征向量内存
    info.features = feature_pool.allocate();
    if (!info.features) {
        packet_memory_pool.deallocate(info.packet, packet_len);
        return EXTRACTOR_ERROR;
    }
    // 初始化特征向量
    memset(info.features, 0, sizeof(feature_vector));
    
    // 分配has_feature内存
    info.has_feature = has_feature_pool.allocate();
    if (!info.has_feature) {
        packet_memory_pool.deallocate(info.packet, packet_len);
        feature_pool.deallocate(info.features);
        return EXTRACTOR_ERROR;
    }
    *info.has_feature = 0;
    
    // Rawbow 指针设为 NULL
    info.raw_payload = NULL;
    
    // 无锁入队
    while (!worker->queue->enqueue(info)) {
        // 必须不断拉取回收队列，帮子线程“清垃圾”，打破死锁环，并让出 CPU
        recycle_memory_func();
        std::this_thread::yield(); // 放弃时间片，等子线程消费
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
    
    // 先从回收队列中回收内存
    multi_thread_packet_info recycled_info;
    while (worker->recycle_queue->dequeue(recycled_info)) {
        // 将回收的内存放回内存池
        if (recycled_info.packet) {
            packet_memory_pool.deallocate(recycled_info.packet, recycled_info.packet_len);
        }
        if (recycled_info.features) {
            feature_pool.deallocate(recycled_info.features);
        }
        if (recycled_info.raw_payload) {
            rawbow_pool.deallocate(recycled_info.raw_payload);
        }
        if (recycled_info.has_feature) {
            has_feature_pool.deallocate(recycled_info.has_feature);
        }
    }
    
    // 构造数据包信息
    multi_thread_packet_info info;
    
    // 复制数据包
    info.packet = packet_memory_pool.allocate(packet_len);
    if (!info.packet) {
        return EXTRACTOR_ERROR;
    }
    memcpy(info.packet, packet, packet_len);
    info.packet_len = packet_len;
    info.ts = *ts;
    
    // 分配特征向量内存
    info.features = feature_pool.allocate();
    if (!info.features) {
        packet_memory_pool.deallocate(info.packet, packet_len);
        return EXTRACTOR_ERROR;
    }
    // 初始化特征向量
    memset(info.features, 0, sizeof(feature_vector));
    
    // 分配 Rawbow 内存
    info.raw_payload = rawbow_pool.allocate();
    if (!info.raw_payload) {
        packet_memory_pool.deallocate(info.packet, packet_len);
        feature_pool.deallocate(info.features);
        return EXTRACTOR_ERROR;
    }
    // 初始化 Rawbow
    memset(info.raw_payload, 0, sizeof(rawbow));
    
    // 分配has_feature内存
    info.has_feature = has_feature_pool.allocate();
    if (!info.has_feature) {
        packet_memory_pool.deallocate(info.packet, packet_len);
        feature_pool.deallocate(info.features);
        rawbow_pool.deallocate(info.raw_payload);
        return EXTRACTOR_ERROR;
    }
    *info.has_feature = 0;
    
    // 无锁入队
    int retry_count = 0;
    while (!worker->queue->enqueue(info)) {
        // 队列满时，短暂休眠后重试
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        retry_count++;
        if (retry_count > 100) {
            // 重试次数过多，释放资源并返回错误
            packet_memory_pool.deallocate(info.packet, packet_len);
            feature_pool.deallocate(info.features);
            rawbow_pool.deallocate(info.raw_payload);
            has_feature_pool.deallocate(info.has_feature);
            return EXTRACTOR_ERROR;
        }
    }
    
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
        pthread_join(worker->thread, NULL);
        
        // 合并线程已处理的特征
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
        pthread_join(worker->thread, NULL);
        
        // 合并线程已处理的特征和 Rawbow
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