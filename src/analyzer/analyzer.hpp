#pragma once
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "common/config.hpp"
#include "common/types.hpp"

namespace gft {

class FrameDiffer {
public:
    explicit FrameDiffer(const AnalyzerConfig& cfg);
    DiffResult compute(const Frame& frame);
    RegionSignature signature(const Frame& frame, const Rect& rect) const;

private:
    AnalyzerConfig cfg_;
    std::vector<uint8_t> prev_;
    int prevW_{0};
    int prevH_{0};
    double scaleX_{1.0};
    double scaleY_{1.0};

    std::vector<uint8_t> toGray(const Frame& frame) const;
    static std::vector<uint8_t> resizeGray(const std::vector<uint8_t>& gray, int w, int h,
                                           int targetW);
    std::vector<Cell> changedCells(const std::vector<uint8_t>& mask, int w, int h,
                                   const Frame& frame) const;
};

class RegionTracker {
public:
    RegionTracker(const AnalyzerConfig& analyzer, const TranslateConfig& translate);

    std::vector<TextRegion> observe(const Frame& frame, const FrameDiffer& differ,
                                   const DiffResult& diff, int64_t nowNs);
    void ingestFull(const Frame& frame, const std::vector<OcrLine>& lines, int64_t nowNs);
    void ingestRegion(const Frame& frame, int regionId, const std::vector<OcrLine>& lines,
                      int64_t nowNs);
    std::vector<TextRegion> pendingTranslations(int64_t nowNs, int limit) const;
    void markTranslateAttempt(int regionId, int64_t nowNs);
    bool setTranslation(int regionId, const std::string& translation,
                        const std::string& provider, int64_t nowNs,
                        std::optional<int> expectedRevision = std::nullopt);
    std::vector<OverlayItem> overlayItems(int64_t nowNs, int ttlMs) const;
    void expire(int64_t nowNs);
    void expireLocked(int64_t nowNs);
    std::tuple<int, int, int> debugCounts() const;
    size_t size() const;
    std::optional<TextRegion> get(int regionId) const;

private:
    AnalyzerConfig cfg_;
    TranslateConfig trCfg_;
    mutable std::mutex mutex_;
    std::unordered_map<int, TextRegion> regions_;
    int nextId_{1};

    static RegionSignature signatureForFrameRect(const Frame& frame, const Rect& rect);
    int matchRegion(const TextRegion& region, const OcrLine& line, const std::vector<int>& used) const;
};

}  // namespace gft
