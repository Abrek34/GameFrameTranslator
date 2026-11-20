#pragma once
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "common/types.hpp"

namespace gft {

class FrameSource {
public:
    using Callback = std::function<void(const Frame&)>;

    virtual ~FrameSource() = default;
    virtual std::string name() const = 0;
    virtual void start(Callback cb) = 0;
    virtual void stop() = 0;
    virtual Frame grabOnce() = 0;
};

std::unique_ptr<FrameSource> createFrameSource(const Config& cfg);

// Stublar (Faz 2/Windows):
// - Windows DXGI Desktop Duplication
// - Wayland portal (PipeWire)
// - Vulkan layer shared-memory ring
// Bu arayüzler C++ çekirdeğine ileride eklenecek; şu an yalnızca başlıklar.

}  // namespace gft
