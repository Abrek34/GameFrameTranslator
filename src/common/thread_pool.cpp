#include "thread_pool.hpp"

#include "log.hpp"

namespace gft {

ThreadPool::ThreadPool(size_t threads, std::string name) : name_(std::move(name)) {
    const size_t n = threads == 0 ? 1 : threads;
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() { stop(); }

bool ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::stop() {
    if (stopping_.exchange(true)) return;
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

size_t ThreadPool::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception& e) {
            GFT_ERROR("pool", "%s görevi hata verdi: %s", name_.c_str(), e.what());
        } catch (...) {
            GFT_ERROR("pool", "%s görevi bilinmeyen hata verdi", name_.c_str());
        }
    }
}

}  // namespace gft
