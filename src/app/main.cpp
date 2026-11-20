#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <X11/Xlib.h>

#include <cairo.h>

#include "analyzer/analyzer.hpp"
#include "cache/sqlite_cache.hpp"
#include "capture/frame_source.hpp"
#include "common/config.hpp"
#include "common/log.hpp"
#include "common/metrics.hpp"
#include "common/model_slot.hpp"
#include "common/thread_pool.hpp"
#include "common/types.hpp"
#include "ocr/ocr_engine.hpp"
#include "overlay/overlay.hpp"
#include "pipeline/pipeline.hpp"
#include "translate/translator.hpp"

namespace {
std::atomic<bool> g_stop{false};
void signalHandler(int) { g_stop.store(true); }
}  // namespace

namespace gft {
// forward helpers
}

static void printUsage() {
    std::cout << "GameFrame Translator (C++ çekirdek)\\n"
              << "Kullanım: gft [--config dosya] [--once] [--no-overlay] [--overlay auto|gtk|tk|none]\\n"
              << "  [--source auto|mss|xcomposite|synthetic] [--window-title BAŞLIK] [--window-class SINIF]\\n"
              << "  [--window-id ID] [--region x,y,w,h] [--monitor N] [--fps N] [--lang DİL]\\n"
              << "  [--provider auto|dictionary|google|none] [--target DİL] [--cache YOL]\\n"
              << "  [--duration SANİYE] [--save-frame PNG] [--dump-config] [--debug] [--version]\\n";
}

static bool parseRegion(const std::string& s, gft::Rect& out) {
    std::string t = s;
    for (char& c : t) if (c == '[' || c == ']' || c == ',') c = ' ';
    std::istringstream in(t);
    return static_cast<bool>(in >> out.x >> out.y >> out.w >> out.h);
}

static void savePng(const gft::Frame& frame, const std::string& path) {
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, frame.width, frame.height);
    unsigned char* data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < frame.height; ++y) {
        unsigned char* row = data + y * stride;
        for (int x = 0; x < frame.width; ++x) {
            const size_t si = (static_cast<size_t>(y) * frame.width + x) * 3;
            row[x * 4 + 0] = frame.rgb[si + 2];
            row[x * 4 + 1] = frame.rgb[si + 1];
            row[x * 4 + 2] = frame.rgb[si];
            row[x * 4 + 3] = 255;
        }
    }
    cairo_surface_mark_dirty(surf);
    cairo_surface_write_to_png(surf, path.c_str());
    cairo_surface_destroy(surf);
}

static int runOnce(gft::Config cfg, const std::string& saveFrame) {
    auto source = gft::createFrameSource(cfg);
    auto ocr = gft::createOcr(cfg);
    gft::TranslationCache cache(cfg.cachePath(), cfg.cache.memorySize);
    auto translator = gft::createTranslator(cfg, cache);
    try {
        gft::Frame frame = source->grabOnce();
        if (!saveFrame.empty()) {
            savePng(frame, saveFrame);
            std::cout << "Kare kaydedildi: " << saveFrame << "\n";
        }
        std::cout << "Kare: " << frame.width << "x" << frame.height
                  << " origin=" << frame.originX << "," << frame.originY
                  << " source=" << frame.source << "\n";
        const auto t0 = std::chrono::steady_clock::now();
        auto lines = ocr->recognize(frame, std::nullopt, cfg.ocr.psm, cfg.ocr.upscale);
        const double ocrMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        std::cout << "OCR: " << lines.size() << " satır, " << ocrMs << " ms, dil=" << cfg.ocr.lang
                  << "\n";
        for (size_t i = 0; i < lines.size(); ++i) {
            const auto tr = translator->translate(lines[i].text);
            std::cout << "  [" << i << "] conf=" << lines[i].confidence
                      << " rect=" << lines[i].rect.x << "," << lines[i].rect.y << ","
                      << lines[i].rect.w << "," << lines[i].rect.h << " "
                      << lines[i].text << " -> " << tr.text << " (" << tr.provider << ")\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Hata: " << e.what() << "\n";
        cache.close();
        return 1;
    }
    cache.close();
    return 0;
}

int main(int argc, char** argv) {
    std::string configPath;
    bool once = false;
    bool noOverlay = false;
    std::string overlayBackend;
    std::string sourceBackend;
    std::string windowTitle;
    std::string windowClass;
    std::string windowId;
    std::string regionStr;
    int monitor = 0;
    int fps = 0;
    std::string lang;
    std::string provider;
    std::string target;
    std::string cachePath;
    double duration = 0;
    std::string saveFrame;
    bool listMonitors = false;
    bool dumpConfig = false;
    bool debug = false;
    bool version = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Eksik değer: " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--config" || a == "-c") configPath = next();
        else if (a == "--once") once = true;
        else if (a == "--no-overlay") noOverlay = true;
        else if (a == "--overlay") overlayBackend = next();
        else if (a == "--source") sourceBackend = next();
        else if (a == "--window-title") windowTitle = next();
        else if (a == "--window-class") windowClass = next();
        else if (a == "--window-id") windowId = next();
        else if (a == "--region") regionStr = next();
        else if (a == "--monitor") monitor = std::stoi(next());
        else if (a == "--fps") fps = std::stoi(next());
        else if (a == "--lang") lang = next();
        else if (a == "--provider") provider = next();
        else if (a == "--target") target = next();
        else if (a == "--cache") cachePath = next();
        else if (a == "--duration") duration = std::stod(next());
        else if (a == "--save-frame") saveFrame = next();
        else if (a == "--list-monitors") listMonitors = true;
        else if (a == "--dump-config") dumpConfig = true;
        else if (a == "--debug") debug = true;
        else if (a == "--version") version = true;
        else if (a == "--help" || a == "-h") {
            printUsage();
            return 0;
        } else {
            std::cerr << "Bilinmeyen seçenek: " << a << "\n";
            return 2;
        }
    }

    if (version) {
        std::cout << "GameFrame Translator (C++ çekirdek) 0.1.0\n";
        return 0;
    }

    gft::Config cfg;
    try {
        cfg = gft::Config::load(configPath);
    } catch (const std::exception& e) {
        std::cerr << "Ayar hatası: " << e.what() << "\n";
        return 2;
    }
    // overrides
    if (!sourceBackend.empty()) cfg.capture.backend = sourceBackend;
    if (!windowTitle.empty()) cfg.capture.windowTitle = windowTitle;
    if (!windowClass.empty()) cfg.capture.windowClass = windowClass;
    if (!windowId.empty()) cfg.capture.windowId = std::stoull(windowId, nullptr, 0);
    if (!regionStr.empty()) {
        gft::Rect r;
        if (parseRegion(regionStr, r)) cfg.capture.region = r;
    }
    if (monitor > 0) {
        cfg.capture.monitor = monitor;
        cfg.overlay.monitor = monitor;
    }
    if (fps > 0) cfg.capture.fps = fps;
    if (!lang.empty()) cfg.ocr.lang = lang;
    if (!provider.empty()) cfg.translate.provider = provider;
    if (!target.empty()) cfg.translate.targetLang = target;
    if (!cachePath.empty()) cfg.cache.path = cachePath;
    if (!overlayBackend.empty()) cfg.overlay.backend = overlayBackend;
    if (noOverlay) cfg.overlay.backend = "none";
    if (debug) {
        cfg.runtime.debug = true;
        cfg.overlay.debug = true;
    }
    cfg.resolveAuto();
    if (cfg.runtime.debug) gft::setLogLevel(gft::LogLevel::Debug);
    else gft::setLogLevel(gft::LogLevel::Info);

    if (listMonitors) {
        Display* dpy = XOpenDisplay(nullptr);
        if (!dpy) {
            std::cerr << "Display açılamadı\n";
            return 1;
        }
        std::cout << "0: tümü: " << DisplayWidth(dpy, DefaultScreen(dpy)) << "x"
                  << DisplayHeight(dpy, DefaultScreen(dpy)) << "\n";
        XCloseDisplay(dpy);
        return 0;
    }
    if (dumpConfig) {
        std::cout << cfg.dump();
        return 0;
    }

    if (once) return runOnce(cfg, saveFrame);

    // root capture + overlay feedback guard
    if (cfg.capture.backend == "mss" && cfg.overlay.backend != "none" &&
        !cfg.capture.allowRootWithOverlay) {
        std::cerr << "Hata: root ekran yakalama + overlay birlikte overlay'in kendi çıktısını yakalar.\n"
                  << "  Çözüm: --window-title \"Oyun Başlığı\" veya --no-overlay.\n";
        return 2;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    gft::Metrics metrics(cfg.logDir(), cfg.runtime.metricsJsonl, cfg.runtime.statsIntervalS);
    gft::LatestModelSlot models;
    gft::ThreadPool pool(2, "translate");
    auto source = gft::createFrameSource(cfg);
    auto ocr = gft::createOcr(cfg);
    gft::TranslationCache cache(cfg.cachePath(), cfg.cache.memorySize);
    auto translator = gft::createTranslator(cfg, cache);
    gft::Pipeline pipeline(cfg, std::move(source), std::move(ocr), std::move(translator), metrics,
                           models, pool);
    auto overlay = gft::createOverlay(cfg.overlay, models, g_stop);

    metrics.event("app_start", {{"version", gft::jsonString("0.1.0")},
                                {"capture", gft::jsonString(cfg.capture.backend)},
                                {"overlay", gft::jsonString(cfg.overlay.backend)},
                                {"ocr", gft::jsonString(cfg.ocr.backend)},
                                {"lang", gft::jsonString(cfg.ocr.lang)},
                                {"translator", gft::jsonString(cfg.translate.provider)},
                                {"target", gft::jsonString(cfg.translate.targetLang)}});

    try {
        overlay->init();
    } catch (const std::exception& e) {
        std::cerr << "Overlay başlatılamadı: " << e.what() << "\n";
        metrics.close();
        cache.close();
        return 3;
    }

    pipeline.start();
    GFT_INFO("app", "Çalışıyor. Durdurmak için Ctrl+C. Overlay=%s, kaynak=%s, %s->%s",
             cfg.overlay.backend.c_str(), cfg.capture.backend.c_str(),
             cfg.translate.sourceLang.c_str(), cfg.translate.targetLang.c_str());

    if (duration > 0) {
        std::thread([duration]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(duration));
            g_stop.store(true);
        }).detach();
    }

    if (cfg.overlay.backend == "none") {
        while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
        overlay->run();
    }

    g_stop.store(true);
    pipeline.stop();
    overlay->shutdown();
    auto summaryPath = metrics.writeSummary();
    std::cout << metrics.summaryJson() << "\n";
    if (!summaryPath.empty()) std::cout << "Özet: " << summaryPath << "\n";
    metrics.close();
    cache.close();
    GFT_INFO("app", "Kapatıldı");
    return 0;
}
