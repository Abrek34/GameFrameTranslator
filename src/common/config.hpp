#pragma once
// YAML benzeri minimal yapılandırma. Harici YAML bağımlılığı yok; desteklenen
// biçim prototype/config.example.yaml ile aynı düz bölüm/anahtar yapısıdır.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "types.hpp"

namespace gft {

struct CaptureConfig {
    std::string backend{"auto"};  // auto | xcomposite | mss | synthetic
    int monitor{1};
    std::optional<Rect> region;
    std::string windowTitle;
    std::string windowClass;
    uint64_t windowId{0};
    int windowPid{0};
    int fps{30};
    std::string display;
    bool allowRootWithOverlay{false};
};

struct AnalyzerConfig {
    int analysisWidth{640};
    int grid{16};
    int pixelThreshold{16};
    double changeTrigger{0.0025};
    double regionChange{0.015};
    int discoveryMs{700};
    int heartbeatMs{3000};
    int expireMs{6000};
    int regionPadding{6};
    int maxRegions{32};
    int maxRegionOcrPerCycle{2};
    double maxAnalysisFps{15.0};
};

struct OcrConfig {
    std::string backend{"auto"};
    std::string lang{"eng"};
    int psm{11};
    int regionPsm{7};
    double minConfidence{45.0};
    double maxCallsPerSec{4.0};
    int burst{2};
    double upscale{1.0};
    double regionUpscale{1.5};
    int autoUpscaleBelowPx{26};
    int minTextHeight{8};
    double timeoutS{5.0};
    std::string tesseractCmd{"tesseract"};
    int ompThreadLimit{2};
};

struct TranslateConfig {
    std::string provider{"auto"};
    std::string sourceLang{"en"};
    std::string targetLang{"tr"};
    double timeoutS{4.0};
    int maxChars{500};
    std::string fallback{"original"};
    std::string googleEndpoint{"https://translate.googleapis.com/translate_a/single"};
    std::string libreEndpoint;
    std::string dictPath;
    int retryCooldownMs{2000};
    int maxTranslationsPerCycle{4};
};

struct OverlayConfig {
    std::string backend{"auto"};
    int monitor{1};
    std::string gdkBackend{"x11"};
    std::string fontFamily{"Noto Sans"};
    int fontSize{20};
    int minFontSize{10};
    double bgAlpha{0.80};
    std::string bgColor{"#0b0d12"};
    std::string textColor{"#ffffff"};
    bool showOriginal{false};
    int padding{6};
    int ttlMs{2500};
    bool clickThrough{true};
    bool debug{false};
    double maxWidthScale{1.25};
};

struct CacheConfig {
    std::string path{"~/.cache/gameframe/translations.sqlite3"};
    int memorySize{512};
};

struct RuntimeConfig {
    std::string logDir{"~/.local/state/gameframe"};
    bool metricsJsonl{true};
    bool debug{false};
    double statsIntervalS{1.0};
};

struct Config {
    CaptureConfig capture;
    AnalyzerConfig analyzer;
    OcrConfig ocr;
    TranslateConfig translate;
    OverlayConfig overlay;
    CacheConfig cache;
    RuntimeConfig runtime;

    static Config load(const std::string& path);
    void resolveAuto();
    [[nodiscard]] std::filesystem::path cachePath() const;
    [[nodiscard]] std::filesystem::path logDir() const;
    [[nodiscard]] std::string dump() const;
};

std::filesystem::path expandUserPath(const std::string& path);

}  // namespace gft
