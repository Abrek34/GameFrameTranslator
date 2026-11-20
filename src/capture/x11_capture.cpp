#include "x11_capture.hpp"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {
namespace {
bool contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}
}  // namespace

X11Capture::X11Capture(const CaptureConfig& cfg) : cfg_(cfg) {
    XInitThreads();
    openDisplay();
}

X11Capture::~X11Capture() {
    if (pixmap_) XFreePixmap(dpy_, pixmap_);
    if (dpy_) XCloseDisplay(dpy_);
}

void X11Capture::openDisplay() {
    dpy_ = XOpenDisplay(cfg_.display.empty() ? nullptr : cfg_.display.c_str());
    if (!dpy_) throw std::runtime_error("X11 display açılamadı");
}

Window X11Capture::findWindow() {
    // window_id
    if (cfg_.windowId != 0) return static_cast<Window>(cfg_.windowId);
    const std::string wantTitle = cfg_.windowTitle;
    const std::string wantClass = cfg_.windowClass;
    // active window fallback
    if (wantTitle.empty() && wantClass.empty()) {
        Atom netActive = XInternAtom(dpy_, "_NET_ACTIVE_WINDOW", True);
        if (netActive != None) {
            Atom type;
            int fmt;
            unsigned long n, bytes;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(dpy_, DefaultRootWindow(dpy_), netActive, 0, 1, False,
                                   XA_WINDOW, &type, &fmt, &n, &bytes, &data) == Success &&
                data && n > 0) {
                Window w = *reinterpret_cast<Window*>(data);
                XFree(data);
                if (w) return w;
            }
            if (data) XFree(data);
        }
    }
    // recursive search
    std::vector<Window> stack;
    unsigned int nchild = 0;
    Window root = DefaultRootWindow(dpy_), parent = 0;
    Window* children = nullptr;
    if (XQueryTree(dpy_, root, &root, &parent, &children, &nchild) && children) {
        for (unsigned int i = 0; i < nchild; ++i) stack.push_back(children[i]);
        XFree(children);
    }
    while (!stack.empty()) {
        Window w = stack.back();
        stack.pop_back();
        char* name = nullptr;
        XFetchName(dpy_, w, &name);
        std::string wname = name ? name : "";
        if (name) XFree(name);
        XClassHint hint{};
        bool hasClass = (XGetClassHint(dpy_, w, &hint) != 0);
        std::string cls = hasClass && hint.res_class ? hint.res_class : "";
        if (hasClass) {
            if (hint.res_name) XFree(hint.res_name);
            if (hint.res_class) XFree(hint.res_class);
        }
        if ((!wantTitle.empty() && contains(wname, wantTitle)) ||
            (!wantClass.empty() && contains(cls, wantClass))) {
            return w;
        }
        Window r2, p2;
        Window* ch = nullptr;
        unsigned int nc = 0;
        if (XQueryTree(dpy_, w, &r2, &p2, &ch, &nc) && ch) {
            for (unsigned int i = 0; i < nc; ++i) stack.push_back(ch[i]);
            XFree(ch);
        }
    }
    return 0;
}

void X11Capture::ensureTarget() {
    if (target_) return;
    target_ = findWindow();
    if (!target_) throw std::runtime_error("Hedef pencere bulunamadı");
    refreshGeometry();
    GFT_INFO("capture", "XComposite hedefi: %dx%d", geomW_, geomH_);
}

void X11Capture::refreshGeometry() {
    XWindowAttributes attr{};
    if (!XGetWindowAttributes(dpy_, target_, &attr)) throw std::runtime_error("pencere öznitelikleri alınamadı");
    if (attr.width < 8 || attr.height < 8) throw std::runtime_error("hedef pencere görünür değil");
    Window child;
    int x, y;
    XTranslateCoordinates(dpy_, target_, DefaultRootWindow(dpy_), 0, 0, &x, &y, &child);
    originX_ = x;
    originY_ = y;
    bool resized = (geomW_ != attr.width || geomH_ != attr.height);
    geomW_ = attr.width;
    geomH_ = attr.height;
    const auto now = std::chrono::steady_clock::now();
    if (resized || now - lastPixmapRefresh_ > std::chrono::milliseconds(100)) {
        if (pixmap_) XFreePixmap(dpy_, pixmap_);
        pixmap_ = XCompositeNameWindowPixmap(dpy_, target_);
        if (!pixmap_) throw std::runtime_error("XComposite pixmap alınamadı");
        lastPixmapRefresh_ = now;
    }
}

Frame X11Capture::captureOnce() {
    ensureTarget();
    refreshGeometry();
    XImage* img = XGetImage(dpy_, pixmap_, 0, 0, geomW_, geomH_, AllPlanes, ZPixmap);
    if (!img) throw std::runtime_error("XGetImage başarısız");
    Frame frame;
    frame.width = geomW_;
    frame.height = geomH_;
    frame.originX = originX_;
    frame.originY = originY_;
    frame.capturedAtNs = nowNs();
    frame.source = "xcomposite";
    frame.rgb.resize(static_cast<size_t>(geomW_) * geomH_ * 3);
    // XImage data is BGRX (little-endian) usually
    for (int y = 0; y < geomH_; ++y) {
        for (int x = 0; x < geomW_; ++x) {
            unsigned long px = XGetPixel(img, x, y);
            const size_t di = (static_cast<size_t>(y) * geomW_ + x) * 3;
            frame.rgb[di + 2] = static_cast<uint8_t>(px & 0xff);
            frame.rgb[di + 1] = static_cast<uint8_t>((px >> 8) & 0xff);
            frame.rgb[di] = static_cast<uint8_t>((px >> 16) & 0xff);
        }
    }
    XDestroyImage(img);
    return frame;
}

Frame X11Capture::grabOnce() { return captureOnce(); }

void X11Capture::start(Callback cb) {
    cb_ = std::move(cb);
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void X11Capture::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void X11Capture::loop() {
    const auto period = std::chrono::milliseconds(1000 / std::max(1, cfg_.fps));
    auto next = std::chrono::steady_clock::now();
    uint64_t seq = 0;
    while (!stop_) {
        try {
            Frame f = captureOnce();
            ++seq;
            f.seq = seq;
            if (cb_) cb_(f);
        } catch (const std::exception& e) {
            GFT_WARN("capture", "Yakalama hatası (xcomposite): %s", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        next += period;
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;
    }
}

// ---------------- RootX11Capture ----------------

RootX11Capture::RootX11Capture(const CaptureConfig& cfg) : cfg_(cfg) {
    XInitThreads();
    openDisplay();
}

RootX11Capture::~RootX11Capture() {
    if (dpy_) XCloseDisplay(dpy_);
}

void RootX11Capture::openDisplay() {
    dpy_ = XOpenDisplay(cfg_.display.empty() ? nullptr : cfg_.display.c_str());
    if (!dpy_) throw std::runtime_error("X11 display açılamadı");
}

Frame RootX11Capture::captureOnce() {
    int screen = DefaultScreen(dpy_);
    Window root = RootWindow(dpy_, screen);
    int w = DisplayWidth(dpy_, screen);
    int h = DisplayHeight(dpy_, screen);
    if (cfg_.region) {
        w = cfg_.region->w;
        h = cfg_.region->h;
    }
    XImage* img = XGetImage(dpy_, root, cfg_.region ? cfg_.region->x : 0,
                            cfg_.region ? cfg_.region->y : 0, w, h, AllPlanes, ZPixmap);
    if (!img) throw std::runtime_error("XGetImage başarısız");
    Frame frame;
    frame.width = w;
    frame.height = h;
    frame.originX = cfg_.region ? cfg_.region->x : 0;
    frame.originY = cfg_.region ? cfg_.region->y : 0;
    frame.capturedAtNs = nowNs();
    frame.source = "mss";
    frame.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned long px = XGetPixel(img, x, y);
            const size_t di = (static_cast<size_t>(y) * w + x) * 3;
            frame.rgb[di + 2] = static_cast<uint8_t>(px & 0xff);
            frame.rgb[di + 1] = static_cast<uint8_t>((px >> 8) & 0xff);
            frame.rgb[di] = static_cast<uint8_t>((px >> 16) & 0xff);
        }
    XDestroyImage(img);
    return frame;
}

Frame RootX11Capture::grabOnce() { return captureOnce(); }

void RootX11Capture::start(Callback cb) {
    cb_ = std::move(cb);
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void RootX11Capture::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void RootX11Capture::loop() {
    const auto period = std::chrono::milliseconds(1000 / std::max(1, cfg_.fps));
    auto next = std::chrono::steady_clock::now();
    uint64_t seq = 0;
    while (!stop_) {
        try {
            Frame f = captureOnce();
            ++seq;
            f.seq = seq;
            if (cb_) cb_(f);
        } catch (const std::exception& e) {
            GFT_WARN("capture", "Yakalama hatası (root): %s", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        next += period;
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;
    }
}

}  // namespace gft
