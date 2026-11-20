#pragma once
#include <mutex>
#include <utility>

#include "common/types.hpp"

namespace gft {

class LatestModelSlot {
public:
    void publish(const OverlayModel& m) {
        std::lock_guard<std::mutex> lock(mutex_);
        model_ = m;
        ++version_;
    }
    std::pair<OverlayModel, uint64_t> get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {model_, version_};
    }
    uint64_t version() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return version_;
    }

private:
    mutable std::mutex mutex_;
    OverlayModel model_;
    uint64_t version_{0};
};

}  // namespace gft
