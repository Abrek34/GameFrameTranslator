#pragma once
// "En son kare" semantiği: üretici asla beklemez, tüketici yalnızca en yeni
// değeri alır. Kuyruk büyümez; yetişilemeyen kareler bilinçli olarak düşer.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace gft {

template <typename T>
class LatestSlot {
public:
    void publish(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        if (value_) ++overwritten_;
        value_ = std::move(value);
        ++version_;
        cv_.notify_all();
    }

    // lastVersion güncellenir; yeni değer yoksa false döner.
    bool takeNew(uint64_t& lastVersion, T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (version_ <= lastVersion && !closed_) {
            cv_.wait_for(lock, timeout, [&] { return version_ > lastVersion || closed_; });
        }
        if (version_ <= lastVersion || !value_) return false;
        out = std::move(*value_);
        value_.reset();
        lastVersion = version_;
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] uint64_t overwritten() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return overwritten_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> value_;
    uint64_t version_{0};
    uint64_t overwritten_{0};
    bool closed_{false};
};

}  // namespace gft
