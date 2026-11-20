#pragma once
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "common/types.hpp"
#include "frame_source.hpp"

namespace gft {

class X11Capture : public FrameSource {
public:
    explicit X11Capture(const CaptureConfig& cfg);
    ~X11Capture() override;

    std::string name() const override { return "xcomposite"; }
    void start(Callback cb) override;
    void stop() override;
    Frame grabOnce() override;

private:
    CaptureConfig cfg_;
    Display* dpy_{nullptr};
    Window target_{0};
    Pixmap pixmap_{0};
    int geomW_{0}, geomH_{0};
    int originX_{0}, originY_{0};
    std::chrono::steady_clock::time_point lastPixmapRefresh_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    Callback cb_;

    void openDisplay();
    Window findWindow();
    void ensureTarget();
    void refreshGeometry();
    Frame captureOnce();
    void loop();
};

class RootX11Capture : public FrameSource {
public:
    explicit RootX11Capture(const CaptureConfig& cfg);
    ~RootX11Capture();
    std::string name() const override { return "mss"; }
    void start(Callback cb) override;
    void stop() override;
    Frame grabOnce() override;

private:
    CaptureConfig cfg_;
    Display* dpy_{nullptr};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    Callback cb_;
    void openDisplay();
    Frame captureOnce();
    void loop();
};

}  // namespace gft
