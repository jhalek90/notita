#pragma once
// Minimal persistent thread pool with a blocking parallel-for. The calling
// thread takes part in the work, so ThreadPool(1) runs everything inline.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace notita {

class ThreadPool {
public:
    explicit ThreadPool(int threads = 0); // 0 = hardware concurrency
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int threadCount() const { return static_cast<int>(workers_.size()) + 1; }

    // Calls fn(i) for i in [0, count). Returns once all calls are done.
    void parallelFor(int count, const std::function<void(int)>& fn);

private:
    void workerLoop();
    void runIndices();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable startCv_, doneCv_;
    std::uint64_t generation_ = 0;
    bool stop_ = false;
    int workersDone_ = 0;

    const std::function<void(int)>* fn_ = nullptr;
    int count_ = 0;
    std::atomic<int> next_{0};
};

} // namespace notita
