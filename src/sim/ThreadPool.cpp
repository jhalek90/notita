#include "ThreadPool.h"

#include <algorithm>

namespace notita {

ThreadPool::ThreadPool(int threads) {
    if (threads <= 0) threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    for (int i = 1; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    startCv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::runIndices() {
    for (;;) {
        const int i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count_) break;
        (*fn_)(i);
    }
}

void ThreadPool::workerLoop() {
    std::uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            startCv_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
        }
        runIndices();
        {
            std::lock_guard lock(mutex_);
            ++workersDone_;
        }
        doneCv_.notify_one();
    }
}

void ThreadPool::parallelFor(int count, const std::function<void(int)>& fn) {
    if (count <= 0) return;
    if (workers_.empty() || count == 1) {
        for (int i = 0; i < count; ++i) fn(i);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        fn_ = &fn;
        count_ = count;
        next_.store(0, std::memory_order_relaxed);
        workersDone_ = 0;
        ++generation_;
    }
    startCv_.notify_all();
    runIndices();
    std::unique_lock lock(mutex_);
    doneCv_.wait(lock, [&] { return workersDone_ == static_cast<int>(workers_.size()); });
    fn_ = nullptr;
}

} // namespace notita
