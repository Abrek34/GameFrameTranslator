#include "pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {

RateLimiter::RateLimiter(double rate, int burst)
    : rate_(std::max(0.1, rate)),
      capacity_(std::max(1, burst)),
      tokens_(static_cast<double>(std::max(1, burst))),
      last_(std::chrono::steady_clock::now()) {}

bool RateLimiter::allow() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    tokens_ = std::min(static_cast<double>(capacity_), tokens_ + elapsed * rate_);
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;
    }
    return false;
}

Pipeline::Pipeline(const Config& cfg, std::unique_ptr<FrameSource> source,
                   std::unique_ptr<OcrEngine> ocr, std::unique_ptr<TranslatorChain> translator,
                   Metrics& metrics, LatestModelSlot& models, ThreadPool& pool)
    : cfg_(cfg),
      source_(std::move(source)),
      ocr_(std::move(ocr)),
      translator_(std::move(translator)),
      metrics_(metrics),
      models_(models),
      pool_(pool),
      differ_(cfg_.analyzer),
      tracker_(cfg_.analyzer, cfg_.translate),
      limiter_(cfg_.ocr.maxCallsPerSec, cfg_.ocr.burst) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::start() {
    stop_ = false;
    source_->start(std::bind(&Pipeline::onFrame, this, std::placeholders::_1));
    analyzer_ = std::thread([this] { analyzeLoop(); });
    GFT_INFO("pipeline", "Pipeline başladı (capture=%s, ocr=%s)", source_->name().c_str(),
             ocr_->name().c_str());
}

void Pipeline::stop() {
    if (stop_.exchange(true)) return;
    frames_.close();
    try {
        source_->stop();
    } catch (...) {
    }
    pool_.stop();
    if (analyzer_.joinable()) analyzer_.join();
    GFT_INFO("pipeline", "Pipeline durdu");
}

void Pipeline::onFrame(const Frame& frame) {
    if (stop_.load()) return;
    metrics_.inc("frames_captured");
    frames_.publish(frame);
}

void Pipeline::analyzeLoop() {
    while (!stop_.load()) {
        Frame frame;
        if (!frames_.takeNew(lastVersion_, frame, std::chrono::milliseconds(200))) {
            if (stop_.load()) break;
            continue;
        }
        try {
            process(frame);
        } catch (const std::exception& e) {
            metrics_.inc("errors");
            GFT_ERROR("pipeline", "Analiz hatası: %s", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void Pipeline::process(const Frame& frame) {
    const int64_t now = nowNs();
    const auto t0 = std::chrono::steady_clock::now();
    const DiffResult diff = differ_.compute(frame);
    const auto dirty = tracker_.observe(frame, differ_, diff, now);
    metrics_.inc("frames_analyzed");
    metrics_.timing("diff_ms",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count());

    const uint64_t dropped = frames_.overwritten() - overwrittenSeen_;
    if (dropped > 0) {
        metrics_.inc("frames_dropped", static_cast<int>(dropped));
        overwrittenSeen_ = frames_.overwritten();
    }

    bool didOcr = false;
    const int64_t intervalNs = static_cast<int64_t>(cfg_.analyzer.discoveryMs) * 1000000LL;
    const int64_t heartbeatNs = static_cast<int64_t>(cfg_.analyzer.heartbeatMs) * 1000000LL;
    const bool due = (now - lastDiscoveryNs_) >= intervalNs;

    if (tracker_.size() == 0) {
        didOcr = discover(frame, now);
    } else if (!dirty.empty()) {
        didOcr = ocrDirty(frame, dirty, now);
    } else if (due && diff.changedRatio >= cfg_.analyzer.changeTrigger) {
        didOcr = discover(frame, now);
    } else if ((now - lastDiscoveryNs_) >= heartbeatNs && diff.changedRatio > 0.0005) {
        didOcr = discover(frame, now);
    }

    const bool translated = translatePending(now);
    tracker_.expire(now);
    publishModel(now, diff, didOcr || translated);

    metrics_.timing(
        "frame_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

    if (now - lastStatsNs_ >= 1000000000LL) {
        lastStatsNs_ = now;
        const auto [regs, trans, stale] = tracker_.debugCounts();
        std::ostringstream dbg;
        dbg << "regions=" << regs << " translated=" << trans << " stale=" << stale
            << " dirty=" << dirty.size() << " diff=" << (diff.changedRatio * 100.0) << "%";
        metrics_.event("pipeline",
                       {{"diff_ratio", jsonNumber(diff.changedRatio)},
                        {"regions", std::to_string(regs)},
                        {"translated", std::to_string(trans)},
                        {"stale", std::to_string(stale)},
                        {"dirty", std::to_string(dirty.size())},
                        {"ocr_calls", std::to_string(metrics_.counter("ocr_calls"))},
                        {"translate_calls", std::to_string(metrics_.counter("translate_calls"))}});
    }
}

bool Pipeline::discover(const Frame& frame, int64_t now) {
    if (!limiter_.allow()) return false;
    lastDiscoveryNs_ = now;
    return ocrFull(frame, now);
}

bool Pipeline::ocrFull(const Frame& frame, int64_t now) {
    try {
        const auto lines = ocr_->recognize(frame, std::nullopt, cfg_.ocr.psm, cfg_.ocr.upscale);
        metrics_.inc("ocr_calls");
        metrics_.inc("full_ocr_calls");
        tracker_.ingestFull(frame, lines, now);
        GFT_DEBUG("pipeline", "Keşif OCR: %zu satır", lines.size());
        return true;
    } catch (const OcrError& e) {
        ++ocrErrors_;
        metrics_.inc("ocr_errors");
        if (ocrErrors_ <= 3 || ocrErrors_ % 10 == 0)
            GFT_ERROR("pipeline", "OCR hatası: %s", e.what());
        return false;
    }
}

bool Pipeline::ocrDirty(const Frame& frame, const std::vector<TextRegion>& dirty, int64_t now) {
    bool did = false;
    const int maxN = std::max(1, cfg_.analyzer.maxRegionOcrPerCycle);
    int n = 0;
    for (const auto& region : dirty) {
        if (n++ >= maxN) break;
        if (!limiter_.allow()) break;
        const int padY = std::max(cfg_.analyzer.regionPadding,
                                  static_cast<int>(region.rect.h * 0.28));
        const int padLeft = std::max(6, std::min(14, static_cast<int>(region.rect.w * 0.08)));
        const int padRight = std::max(48, static_cast<int>(region.rect.w * 0.90));
        const Rect roi{region.rect.x - padLeft, region.rect.y - padY,
                       region.rect.w + padLeft + padRight, region.rect.h + 2 * padY};
        try {
            const auto lines = ocr_->recognize(frame, roi, cfg_.ocr.regionPsm, cfg_.ocr.regionUpscale);
            metrics_.inc("ocr_calls");
            metrics_.inc("region_ocr_calls");
            tracker_.ingestRegion(frame, region.id, lines, now);
            did = true;
            GFT_DEBUG("pipeline", "Bölge OCR [%d]: %zu satır", region.id, lines.size());
        } catch (const OcrError& e) {
            ++ocrErrors_;
            metrics_.inc("ocr_errors");
            if (ocrErrors_ <= 3 || ocrErrors_ % 10 == 0)
                GFT_ERROR("pipeline", "Bölge OCR hatası: %s", e.what());
        }
    }
    return did;
}

bool Pipeline::translatePending(int64_t now) {
    const auto pending = tracker_.pendingTranslations(now, cfg_.translate.maxTranslationsPerCycle);
    bool scheduled = false;
    for (const auto& region : pending) {
        if (stop_.load()) break;
        const int rid = region.id;
        const std::string text = region.text;
        const int rev = region.revision;
        tracker_.markTranslateAttempt(rid, now);

        {
            std::lock_guard<std::mutex> lock(inFlightMutex_);
            if (inFlight_.count(rid) || inFlight_.size() >= 4) continue;
            inFlight_.insert(rid);
        }
        scheduled = true;
        pool_.submit([this, rid, text, rev]() {
            const auto t0 = std::chrono::steady_clock::now();
            const auto result = translator_->translate(text);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            metrics_.inc("translate_calls");
            metrics_.timing("translate_ms", ms);
            if (result.cached) metrics_.inc("cache_hits");
            if (!result.text.empty()) {
                const bool ok = tracker_.setTranslation(rid, result.text, result.provider, nowNs(), rev);
                if (ok) {
                    const auto r = tracker_.get(rid);
                    const double lat = r ? r->latencyMs : 0.0;
                    lastLatencyMs_ = lat;
                    metrics_.latency(lat, {{"region_id", std::to_string(rid)},
                                           {"source", jsonString(text)},
                                           {"translated", jsonString(result.text)},
                                           {"provider", jsonString(result.provider)},
                                           {"cached", jsonBool(result.cached)}});
                    metrics_.event("translation",
                                   {{"region_id", std::to_string(rid)},
                                    {"source", jsonString(text)},
                                    {"translated", jsonString(result.text)},
                                    {"provider", jsonString(result.provider)},
                                    {"cached", jsonBool(result.cached)},
                                    {"elapsed_ms", jsonNumber(ms)},
                                    {"latency_ms", jsonNumber(lat)}});
                    GFT_INFO("pipeline", "Çeviri [%d] %s -> %s (%s, %.0f ms)", rid, text.c_str(),
                             result.text.c_str(), result.provider.c_str(), lat);
                }
            }
            {
                std::lock_guard<std::mutex> lock(inFlightMutex_);
                inFlight_.erase(rid);
            }
            publishModel(nowNs(), DiffResult{}, true);
        });
    }
    return scheduled;
}

void Pipeline::publishModel(int64_t now, const DiffResult& diff, bool force) {
    OverlayModel model;
    model.items = tracker_.overlayItems(now, cfg_.overlay.ttlMs);
    const auto [regs, trans, stale] = tracker_.debugCounts();
    std::ostringstream dbg;
    dbg << "regions=" << regs << " translated=" << trans << " stale=" << stale
        << " diff=" << (diff.changedRatio * 100.0) << "%"
        << " ocr=" << metrics_.counter("ocr_calls");
    model.debug = dbg.str();
    model.updatedAtNs = now;
    const uint64_t sig = modelSignature(model);
    if (!force && sig == lastPublishSig_ && (now - lastPublishNs_) < 250000000LL) return;
    lastPublishSig_ = sig;
    lastPublishNs_ = now;
    models_.publish(model);
    if (sig != 0 || force) metrics_.overlay(model, lastLatencyMs_.load());
}

uint64_t Pipeline::modelSignature(const OverlayModel& model) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& item : model.items) {
        for (char c : item.text) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
        for (char c : item.sourceText) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
        h ^= static_cast<uint64_t>(item.regionId);
        h *= 0x100000001b3ULL;
        h ^= item.stale ? 1ULL : 0ULL;
        h *= 0x100000001b3ULL;
    }
    return h;
}

}  // namespace gft
