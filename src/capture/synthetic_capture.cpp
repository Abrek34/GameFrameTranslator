#include "synthetic_capture.hpp"

#include <cairo.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {
namespace {
const char* LINES[][2] = {
    {"Continue", "Press E to interact"},
    {"Settings", "Audio, Video, Controls"},
    {"Inventory", "Health Potion x3"},
    {"Quest updated", "Find the missing blacksmith"},
    {"Game paused", "Resume   Save   Quit"},
    {"Level up!", "You reached level 12"},
};
const int N = 6;
}  // namespace

SyntheticCapture::SyntheticCapture(const CaptureConfig& cfg) : cfg_(cfg) {}

Frame SyntheticCapture::render() {
    const int w = 1280, h = 720;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(surf);
    // background
    cairo_set_source_rgb(cr, 0.07, 0.08, 0.11);
    cairo_paint(cr);
    for (int y = 0; y < h; ++y) {
        double t = y / static_cast<double>(h);
        cairo_set_source_rgb(cr, 0.07 + 0.08 * t, 0.08 + 0.05 * t, 0.11 + 0.04 * t);
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, w, y);
        cairo_stroke(cr);
    }
    cairo_set_source_rgb(cr, 0.04, 0.05, 0.07);
    cairo_rectangle(cr, 0, 0, w, 46);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.55, 0.58, 0.65);
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, 16, 28);
    cairo_show_text(cr, "GFT SYNTHETIC SCENE  |  presented-frame test fixture");
    cairo_move_to(cr, 16, 62);
    cairo_show_text(cr, "Health 100     Mana 50     Gold 1234");

    const int idx = static_cast<int>((nowNs() / 1e9) / 2.5) % N;
    // Actually use elapsed from process start? simpler time-based modulo.
    const int i = (static_cast<int>(std::time(nullptr) / 2)) % N;
    (void)idx;
    const char* title = LINES[i][0];
    const char* body = LINES[i][1];

    const int bw = static_cast<int>(w * 0.80);
    const int bh = static_cast<int>(h * 0.27);
    const int x0 = (w - bw) / 2;
    const int y0 = static_cast<int>(h * 0.64);
    cairo_set_source_rgb(cr, 0.03, 0.04, 0.06);
    cairo_rectangle(cr, x0, y0, bw, bh);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.29, 0.35, 0.43);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, x0, y0, bw, bh);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_font_size(cr, 28);
    cairo_move_to(cr, x0 + 28, y0 + 52);
    cairo_show_text(cr, title);
    cairo_set_source_rgb(cr, 0.86, 0.89, 0.93);
    cairo_move_to(cr, x0 + 28, y0 + 112);
    cairo_show_text(cr, body);
    cairo_set_source_rgb(cr, 0.6, 0.63, 0.7);
    cairo_set_font_size(cr, 16);
    cairo_move_to(cr, x0 + 28, y0 + bh - 28);
    cairo_show_text(cr, "Press Space to continue");

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    unsigned char* data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    Frame frame;
    frame.width = w;
    frame.height = h;
    frame.originX = 0;
    frame.originY = 0;
    frame.capturedAtNs = nowNs();
    frame.source = "synthetic";
    frame.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = data + y * stride;
        for (int x = 0; x < w; ++x) {
            const size_t di = (static_cast<size_t>(y) * w + x) * 3;
            frame.rgb[di] = row[x * 4 + 2];
            frame.rgb[di + 1] = row[x * 4 + 1];
            frame.rgb[di + 2] = row[x * 4];
        }
    }
    cairo_surface_destroy(surf);
    return frame;
}

Frame SyntheticCapture::grabOnce() { return render(); }

void SyntheticCapture::start(Callback cb) {
    cb_ = std::move(cb);
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void SyntheticCapture::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void SyntheticCapture::loop() {
    const auto period = std::chrono::milliseconds(1000 / std::max(1, cfg_.fps));
    auto next = std::chrono::steady_clock::now();
    uint64_t seq = 0;
    while (!stop_) {
        Frame f = render();
        ++seq;
        f.seq = seq;
        if (cb_) cb_(f);
        next += period;
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;
    }
}

}  // namespace gft
