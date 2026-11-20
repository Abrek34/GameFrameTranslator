#include "frame_source.hpp"

#include "common/config.hpp"
#include "common/log.hpp"
#include "x11_capture.hpp"
#include "synthetic_capture.hpp"

namespace gft {

std::unique_ptr<FrameSource> createFrameSource(const Config& cfg) {
    if (cfg.capture.backend == "xcomposite") return std::make_unique<X11Capture>(cfg.capture);
    if (cfg.capture.backend == "synthetic") return std::make_unique<SyntheticCapture>(cfg.capture);
    // mss/root
    return std::make_unique<RootX11Capture>(cfg.capture);
}

}  // namespace gft
