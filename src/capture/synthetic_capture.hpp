#pragma once
#include <memory>
#include <string>

#include "common/config.hpp"
#include "common/types.hpp"
#include "frame_source.hpp"

namespace gft {

class SyntheticCapture : public FrameSource {
public:
    explicit SyntheticCapture(const CaptureConfig& cfg);
    std::string name() const override { return "synthetic"; }
    void start(Callback cb) override;
    void stop() override;
    Frame grabOnce() override;

private:
    CaptureConfig cfg_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    Callback cb_;
    void loop();
    Frame render();
};

}  // namespace gft
