#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "common/config.hpp"
#include <thread>

#include "common/model_slot.hpp"
#include "common/types.hpp"

namespace gft {

class Overlay {
public:
    virtual ~Overlay() = default;
    virtual std::string name() const = 0;
    virtual void init() = 0;
    virtual void run() = 0;  // bloke eder
    virtual void shutdown() = 0;
};

std::unique_ptr<Overlay> createOverlay(const OverlayConfig& cfg, LatestModelSlot& models,
                                       std::atomic<bool>& stop);

class NullOverlay : public Overlay {
public:
    NullOverlay(LatestModelSlot& models, std::atomic<bool>& stop) : models_(models), stop_(stop) {}
    std::string name() const override { return "none"; }
    void init() override {}
    void run() override { while (!stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    void shutdown() override {}

private:
    LatestModelSlot& models_;
    std::atomic<bool>& stop_;
};

}  // namespace gft
