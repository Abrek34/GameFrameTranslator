#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "log.hpp"

namespace gft {
namespace {

std::string stripInlineComment(const std::string& line) {
    bool single = false;
    bool dbl = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !dbl) single = !single;
        if (c == '"' && !single) dbl = !dbl;
        if (c == '#' && !single && !dbl) return line.substr(0, i);
    }
    return line;
}

std::string unquote(std::string v) {
    v = trim(std::move(v));
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                          (v.front() == '\'' && v.back() == '\''))) {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

bool toBool(const std::string& v) {
    std::string s = v;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::optional<Rect> parseRegion(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) {
        if (c == '[' || c == ']' || c == ',') c = ' ';
    }
    std::istringstream in(s);
    Rect r;
    if (!(in >> r.x >> r.y >> r.w >> r.h)) return std::nullopt;
    return r;
}

template <typename T>
void setNumber(const std::string& section, const std::string& key, const std::string& value,
               T& target, const char* expected) {
    try {
        if constexpr (std::is_same_v<T, int>) {
            target = std::stoi(value);
        } else if constexpr (std::is_same_v<T, double>) {
            target = std::stod(value);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            target = std::stoull(value, nullptr, 0);
        }
        (void)expected;
    } catch (const std::exception&) {
        GFT_WARN("config", "Geçersiz sayı: %s.%s=%s", section.c_str(), key.c_str(),
                 value.c_str());
    }
}

}  // namespace

std::filesystem::path expandUserPath(const std::string& path) {
    std::string p = path;
    if (p.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home) p = std::string(home) + p.substr(1);
    }
    return std::filesystem::absolute(std::filesystem::path(p));
}

Config Config::load(const std::string& path) {
    Config cfg;
    if (path.empty()) return cfg;
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Ayar dosyası bulunamadı: " + path);

    std::string section;
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        std::string line = trim(stripInlineComment(raw));
        if (line.empty()) continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            GFT_WARN("config", "Satır %d anlaşılmadı: %s", lineNo, line.c_str());
            continue;
        }
        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (value.empty() &&
            (key == "capture" || key == "analyzer" || key == "ocr" || key == "translate" ||
             key == "overlay" || key == "cache" || key == "runtime")) {
            section = key;
            continue;
        }
        value = unquote(value);
        const std::string full = section.empty() ? key : section + "." + key;

        if (full == "capture.backend") cfg.capture.backend = value;
        else if (full == "capture.monitor") setNumber(section, key, value, cfg.capture.monitor, "int");
        else if (full == "capture.region") {
            if (auto r = parseRegion(value)) cfg.capture.region = *r;
        } else if (full == "capture.window_title") cfg.capture.windowTitle = value;
        else if (full == "capture.window_class") cfg.capture.windowClass = value;
        else if (full == "capture.window_id") {
            if (value.empty()) cfg.capture.windowId = 0;
            else setNumber(section, key, value, cfg.capture.windowId, "uint64");
        } else if (full == "capture.window_pid") setNumber(section, key, value, cfg.capture.windowPid, "int");
        else if (full == "capture.fps") setNumber(section, key, value, cfg.capture.fps, "int");
        else if (full == "capture.display") cfg.capture.display = value;
        else if (full == "capture.allow_root_with_overlay") cfg.capture.allowRootWithOverlay = toBool(value);

        else if (full == "analyzer.analysis_width") setNumber(section, key, value, cfg.analyzer.analysisWidth, "int");
        else if (full == "analyzer.grid") setNumber(section, key, value, cfg.analyzer.grid, "int");
        else if (full == "analyzer.pixel_threshold") setNumber(section, key, value, cfg.analyzer.pixelThreshold, "int");
        else if (full == "analyzer.change_trigger") setNumber(section, key, value, cfg.analyzer.changeTrigger, "double");
        else if (full == "analyzer.region_change") setNumber(section, key, value, cfg.analyzer.regionChange, "double");
        else if (full == "analyzer.discovery_ms") setNumber(section, key, value, cfg.analyzer.discoveryMs, "int");
        else if (full == "analyzer.heartbeat_ms") setNumber(section, key, value, cfg.analyzer.heartbeatMs, "int");
        else if (full == "analyzer.expire_ms") setNumber(section, key, value, cfg.analyzer.expireMs, "int");
        else if (full == "analyzer.region_padding") setNumber(section, key, value, cfg.analyzer.regionPadding, "int");
        else if (full == "analyzer.max_regions") setNumber(section, key, value, cfg.analyzer.maxRegions, "int");
        else if (full == "analyzer.max_region_ocr_per_cycle") setNumber(section, key, value, cfg.analyzer.maxRegionOcrPerCycle, "int");
        else if (full == "analyzer.max_analysis_fps") setNumber(section, key, value, cfg.analyzer.maxAnalysisFps, "double");

        else if (full == "ocr.backend") cfg.ocr.backend = value;
        else if (full == "ocr.lang") cfg.ocr.lang = value;
        else if (full == "ocr.psm") setNumber(section, key, value, cfg.ocr.psm, "int");
        else if (full == "ocr.region_psm") setNumber(section, key, value, cfg.ocr.regionPsm, "int");
        else if (full == "ocr.min_confidence") setNumber(section, key, value, cfg.ocr.minConfidence, "double");
        else if (full == "ocr.max_calls_per_sec") setNumber(section, key, value, cfg.ocr.maxCallsPerSec, "double");
        else if (full == "ocr.burst") setNumber(section, key, value, cfg.ocr.burst, "int");
        else if (full == "ocr.upscale") setNumber(section, key, value, cfg.ocr.upscale, "double");
        else if (full == "ocr.region_upscale") setNumber(section, key, value, cfg.ocr.regionUpscale, "double");
        else if (full == "ocr.auto_upscale_below_px") setNumber(section, key, value, cfg.ocr.autoUpscaleBelowPx, "int");
        else if (full == "ocr.min_text_height") setNumber(section, key, value, cfg.ocr.minTextHeight, "int");
        else if (full == "ocr.timeout_s") setNumber(section, key, value, cfg.ocr.timeoutS, "double");
        else if (full == "ocr.tesseract_cmd") cfg.ocr.tesseractCmd = value;
        else if (full == "ocr.omp_thread_limit") setNumber(section, key, value, cfg.ocr.ompThreadLimit, "int");

        else if (full == "translate.provider") cfg.translate.provider = value;
        else if (full == "translate.source_lang") cfg.translate.sourceLang = value;
        else if (full == "translate.target_lang") cfg.translate.targetLang = value;
        else if (full == "translate.timeout_s") setNumber(section, key, value, cfg.translate.timeoutS, "double");
        else if (full == "translate.max_chars") setNumber(section, key, value, cfg.translate.maxChars, "int");
        else if (full == "translate.fallback") cfg.translate.fallback = value;
        else if (full == "translate.google_endpoint") cfg.translate.googleEndpoint = value;
        else if (full == "translate.libre_endpoint") cfg.translate.libreEndpoint = value;
        else if (full == "translate.dict_path") cfg.translate.dictPath = value;
        else if (full == "translate.retry_cooldown_ms") setNumber(section, key, value, cfg.translate.retryCooldownMs, "int");
        else if (full == "translate.max_translations_per_cycle") setNumber(section, key, value, cfg.translate.maxTranslationsPerCycle, "int");

        else if (full == "overlay.backend") cfg.overlay.backend = value;
        else if (full == "overlay.monitor") setNumber(section, key, value, cfg.overlay.monitor, "int");
        else if (full == "overlay.gdk_backend") cfg.overlay.gdkBackend = value;
        else if (full == "overlay.font_family") cfg.overlay.fontFamily = value;
        else if (full == "overlay.font_size") setNumber(section, key, value, cfg.overlay.fontSize, "int");
        else if (full == "overlay.min_font_size") setNumber(section, key, value, cfg.overlay.minFontSize, "int");
        else if (full == "overlay.bg_alpha") setNumber(section, key, value, cfg.overlay.bgAlpha, "double");
        else if (full == "overlay.bg_color") cfg.overlay.bgColor = value;
        else if (full == "overlay.text_color") cfg.overlay.textColor = value;
        else if (full == "overlay.show_original") cfg.overlay.showOriginal = toBool(value);
        else if (full == "overlay.padding") setNumber(section, key, value, cfg.overlay.padding, "int");
        else if (full == "overlay.ttl_ms") setNumber(section, key, value, cfg.overlay.ttlMs, "int");
        else if (full == "overlay.click_through") cfg.overlay.clickThrough = toBool(value);
        else if (full == "overlay.debug") cfg.overlay.debug = toBool(value);
        else if (full == "overlay.max_width_scale") setNumber(section, key, value, cfg.overlay.maxWidthScale, "double");

        else if (full == "cache.path") cfg.cache.path = value;
        else if (full == "cache.memory_size") setNumber(section, key, value, cfg.cache.memorySize, "int");

        else if (full == "runtime.log_dir") cfg.runtime.logDir = value;
        else if (full == "runtime.metrics_jsonl") cfg.runtime.metricsJsonl = toBool(value);
        else if (full == "runtime.debug") cfg.runtime.debug = toBool(value);
        else if (full == "runtime.stats_interval_s") setNumber(section, key, value, cfg.runtime.statsIntervalS, "double");
        else {
            GFT_WARN("config", "Bilinmeyen ayar: %s (yok sayıldı)", full.c_str());
        }
    }
    return cfg;
}

void Config::resolveAuto() {
    if (capture.backend == "auto") {
        capture.backend = (!capture.windowTitle.empty() || !capture.windowClass.empty() ||
                           capture.windowId != 0)
                              ? "xcomposite"
                              : "mss";
    }
    if (ocr.backend == "auto") ocr.backend = "tesseract";
}

std::filesystem::path Config::cachePath() const { return expandUserPath(cache.path); }
std::filesystem::path Config::logDir() const { return expandUserPath(runtime.logDir); }

std::string Config::dump() const {
    std::ostringstream o;
    o << "capture:\n"
      << "  backend: " << capture.backend << "\n"
      << "  monitor: " << capture.monitor << "\n"
      << "  window_title: \"" << capture.windowTitle << "\"\n"
      << "  window_class: \"" << capture.windowClass << "\"\n"
      << "  window_id: " << capture.windowId << "\n"
      << "  fps: " << capture.fps << "\n"
      << "  allow_root_with_overlay: " << (capture.allowRootWithOverlay ? "true" : "false") << "\n";
    if (capture.region) {
        o << "  region: [" << capture.region->x << ", " << capture.region->y << ", "
          << capture.region->w << ", " << capture.region->h << "]\n";
    }
    o << "analyzer:\n"
      << "  analysis_width: " << analyzer.analysisWidth << "\n"
      << "  grid: " << analyzer.grid << "\n"
      << "  change_trigger: " << analyzer.changeTrigger << "\n"
      << "  region_change: " << analyzer.regionChange << "\n"
      << "ocr:\n"
      << "  backend: " << ocr.backend << "\n"
      << "  lang: " << ocr.lang << "\n"
      << "  psm: " << ocr.psm << "\n"
      << "translate:\n"
      << "  provider: " << translate.provider << "\n"
      << "  source_lang: " << translate.sourceLang << "\n"
      << "  target_lang: " << translate.targetLang << "\n"
      << "overlay:\n"
      << "  backend: " << overlay.backend << "\n"
      << "  font_family: \"" << overlay.fontFamily << "\"\n"
      << "  font_size: " << overlay.fontSize << "\n"
      << "  ttl_ms: " << overlay.ttlMs << "\n"
      << "cache:\n"
      << "  path: \"" << cache.path << "\"\n"
      << "runtime:\n"
      << "  log_dir: \"" << runtime.logDir << "\"\n"
      << "  debug: " << (runtime.debug ? "true" : "false") << "\n";
    return o.str();
}

}  // namespace gft
