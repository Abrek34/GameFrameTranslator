#pragma once
// Wayland portal / PipeWire backend (Faz 2, Linux native Wayland).
// Şu an iskelet; XWayland oyunlar için XComposite kullanılır.
#include "common/config.hpp"
#include "common/types.hpp"
#include "frame_source.hpp"
namespace gft {
class WaylandPortalCapture : public FrameSource {
public:
    explicit WaylandPortalCapture(const CaptureConfig& cfg) : cfg_(cfg) {}
    std::string name() const override { return "wayland_portal"; }
    void start(Callback cb) override { (void)cb; }
    void stop() override {}
    Frame grabOnce() override { return Frame{}; }
private:
    CaptureConfig cfg_;
};
}  // namespace gft
