#pragma once
// Windows DXGI Desktop Duplication backend (Faz 2).
// Bu başlık şu an iskelettir; implementasyon Windows'ta eklenecek.
#ifdef _WIN32
#include "common/config.hpp"
#include "common/types.hpp"
#include "frame_source.hpp"
namespace gft {
class DxgiCapture : public FrameSource {
public:
    explicit DxgiCapture(const CaptureConfig& cfg) : cfg_(cfg) {}
    std::string name() const override { return "dxgi"; }
    void start(Callback cb) override { (void)cb; }
    void stop() override {}
    Frame grabOnce() override { return Frame{}; }
private:
    CaptureConfig cfg_;
};
}  // namespace gft
#endif
