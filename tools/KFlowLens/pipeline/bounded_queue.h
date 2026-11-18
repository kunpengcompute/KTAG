/*
Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
*/
#ifndef KTAG_DEMO_BOUNDED_QUEUE_H
#define KTAG_DEMO_BOUNDED_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

enum class ktag_demo_backpressure_policy {
    block,
    drop_newest,
    drop_oldest,
    fail
};

enum class ktag_demo_queue_push_result {
    pushed,
    pushed_after_dropping_oldest,
    dropped_newest,
    failed,
    closed
};

struct ktag_demo_queue_statistics {
    uint64_t pushed = 0U;
    uint64_t popped = 0U;
    uint64_t block_count = 0U;
    uint64_t blocked_nanoseconds = 0U;
    uint64_t dropped_newest = 0U;
    uint64_t dropped_oldest = 0U;
    uint64_t failures = 0U;
    size_t current_depth = 0U;
    size_t peak_depth = 0U;
    bool closed = false;
};

template <typename T>
class ktag_demo_bounded_queue {
public:
    explicit ktag_demo_bounded_queue(size_t capacity)
        : capacity_(capacity)
    {
    }

    ktag_demo_bounded_queue(const ktag_demo_bounded_queue &) = delete;
    ktag_demo_bounded_queue &operator=(
        const ktag_demo_bounded_queue &) = delete;

    ktag_demo_queue_push_result push(
        T value,
        ktag_demo_backpressure_policy policy)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || capacity_ == 0U) {
            return ktag_demo_queue_push_result::closed;
        }
        if (queue_.size() >= capacity_) {
            if (policy == ktag_demo_backpressure_policy::drop_newest) {
                ++statistics_.dropped_newest;
                return ktag_demo_queue_push_result::dropped_newest;
            }
            if (policy == ktag_demo_backpressure_policy::drop_oldest) {
                queue_.pop_front();
                ++statistics_.dropped_oldest;
                queue_.push_back(std::move(value));
                record_push_locked();
                not_empty_.notify_one();
                return ktag_demo_queue_push_result::pushed_after_dropping_oldest;
            }
            if (policy == ktag_demo_backpressure_policy::fail) {
                ++statistics_.failures;
                return ktag_demo_queue_push_result::failed;
            }

            ++statistics_.block_count;
            const auto wait_start = std::chrono::steady_clock::now();
            not_full_.wait(lock, [this]() {
                return closed_ || queue_.size() < capacity_;
            });
            const auto wait_end = std::chrono::steady_clock::now();
            statistics_.blocked_nanoseconds +=
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        wait_end - wait_start).count());
            if (closed_) {
                return ktag_demo_queue_push_result::closed;
            }
        }

        queue_.push_back(std::move(value));
        record_push_locked();
        not_empty_.notify_one();
        return ktag_demo_queue_push_result::pushed;
    }

    // Used only by the parallel UB batch producer. Ordinary packet queues
    // retain the original push() path, with no callback or cancellation checks.
    template <typename Cancelled>
    ktag_demo_queue_push_result push_interruptible(T value, Cancelled cancelled)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || capacity_ == 0U || cancelled()) {
            return ktag_demo_queue_push_result::closed;
        }
        if (queue_.size() >= capacity_) {
            ++statistics_.block_count;
            const auto wait_start = std::chrono::steady_clock::now();
            // Signal handlers cannot notify this condition variable. Normal
            // consumers still wake this producer immediately when room opens.
            while (!closed_ && queue_.size() >= capacity_ && !cancelled()) {
                not_full_.wait_for(lock, std::chrono::milliseconds(10));
            }
            statistics_.blocked_nanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start).count());
            if (closed_ || cancelled()) {
                return ktag_demo_queue_push_result::closed;
            }
        }
        queue_.push_back(std::move(value));
        record_push_locked();
        not_empty_.notify_one();
        return ktag_demo_queue_push_result::pushed;
    }

    bool pop(T *value)
    {
        if (value == nullptr) {
            return false;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });
        if (queue_.empty()) {
            return false;
        }
        *value = std::move(queue_.front());
        queue_.pop_front();
        ++statistics_.popped;
        not_full_.notify_one();
        return true;
    }

    bool try_pop(T *value)
    {
        if (value == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        *value = std::move(queue_.front());
        queue_.pop_front();
        ++statistics_.popped;
        not_full_.notify_one();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    ktag_demo_queue_statistics statistics() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = statistics_;
        result.current_depth = queue_.size();
        result.closed = closed_;
        return result;
    }

private:
    void record_push_locked()
    {
        ++statistics_.pushed;
        if (queue_.size() > statistics_.peak_depth) {
            statistics_.peak_depth = queue_.size();
        }
    }

    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> queue_;
    ktag_demo_queue_statistics statistics_;
    bool closed_ = false;
};

#endif /* KTAG_DEMO_BOUNDED_QUEUE_H */
