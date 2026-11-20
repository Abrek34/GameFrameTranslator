#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace gft {

// Sabit iş parçacıklı havuz. OCR/çeviri gibi bloklayan işler render/capture
// thread'inden ayrı tutulur.
class ThreadPool {
public:
    explicit ThreadPool(size_t threads, std::string name = "pool");
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool submit(std::function<void()> task);
    void stop();
    [[nodiscard]] size_t pending() const;
    [[nodiscard]] size_t threads() const { return workers_.size(); }

private:
    void workerLoop();

    std::string name_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stopping_{false};
};

}  // namespace gft
