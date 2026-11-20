#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "analyzer/analyzer.hpp"
#include "capture/frame_source.hpp"
#include "common/latest_slot.hpp"
#include "common/metrics.hpp"
#include "common/model_slot.hpp"
#include "common/thread_pool.hpp"
#include "common/types.hpp"
#include "ocr/ocr_engine.hpp"
#include "translate/translator.hpp"

namespace gft {

class RateLimiter {
public:
    RateLimiter(double rate, int burst);
    bool allow();

private:
    double rate_;
    int capacity_;
    double tokens_;
    std::chrono::steady_clock::time_point last_;
};

class Pipeline {
public:
    Pipeline(const Config& cfg, std::unique_ptr<FrameSource> source,
             std::unique_ptr<OcrEngine> ocr, std::unique_ptr<TranslatorChain> translator,
             Metrics& metrics, LatestModelSlot& models, ThreadPool& pool);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void start();
    void stop();

private:
    Config cfg_;
    std::unique_ptr<FrameSource> source_;
    std::unique_ptr<OcrEngine> ocr_;
    std::unique_ptr<TranslatorChain> translator_;
    Metrics& metrics_;
    LatestModelSlot& models_;
    ThreadPool& pool_;

    FrameDiffer differ_;
    RegionTracker tracker_;
    LatestSlot<Frame> frames_;
    RateLimiter limiter_;

    std::atomic<bool> stop_{false};
    std::thread analyzer_;
    uint64_t lastVersion_{0};
    int64_t lastDiscoveryNs_{0};
    uint64_t lastPublishSig_{0};
    int64_t lastPublishNs_{0};
    std::atomic<double> lastLatencyMs_{0.0};
    int64_t lastStatsNs_{0};
    uint64_t overwrittenSeen_{0};

    std::mutex inFlightMutex_;
    std::set<int> inFlight_;

    int ocrErrors_{0};

    void onFrame(const Frame& frame);
    void analyzeLoop();
    void process(const Frame& frame);
    bool discover(const Frame& frame, int64_t now);
    bool ocrFull(const Frame& frame, int64_t now);
    bool ocrDirty(const Frame& frame, const std::vector<TextRegion>& dirty, int64_t now);
    bool translatePending(int64_t now);
    void publishModel(int64_t now, const DiffResult& diff, bool force);
    static uint64_t modelSignature(const OverlayModel& model);
};

}  // namespace gft
