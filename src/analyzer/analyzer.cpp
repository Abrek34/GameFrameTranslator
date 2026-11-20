#include "analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <numeric>
#include <optional>
#include <sstream>

#include "common/config.hpp"
#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {

RegionSignature signatureFromRgb(const std::vector<uint8_t>& rgb, int w, int h) {
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
    for (int i = 0; i < w * h; ++i) {
        const uint8_t* p = rgb.data() + static_cast<size_t>(i) * 3;
        gray[i] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    }
    RegionSignature sig{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            int sy0 = y * h / 16, sy1 = (y + 1) * h / 16;
            int sx0 = x * w / 16, sx1 = (x + 1) * w / 16;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            long sum = 0;
            int n = 0;
            for (int yy = sy0; yy < sy1 && yy < h; ++yy)
                for (int xx = sx0; xx < sx1 && xx < w; ++xx) {
                    sum += gray[static_cast<size_t>(yy) * w + xx];
                    ++n;
                }
            sig.thumb[static_cast<size_t>(y) * 16 + x] = static_cast<uint8_t>(n ? sum / n : 0);
        }
    }
    std::array<uint8_t, 72> small{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 9; ++x) {
            int sy = y * h / 8, sx = x * w / 9;
            if (sy >= h) sy = h - 1;
            if (sx >= w) sx = w - 1;
            small[static_cast<size_t>(y) * 9 + x] = gray[static_cast<size_t>(sy) * w + sx];
        }
    }
    uint64_t bits = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            bits = (bits << 1) |
                   (small[static_cast<size_t>(y) * 9 + x] > small[static_cast<size_t>(y) * 9 + x + 1]
                        ? 1ULL
                        : 0ULL);
        }
    sig.dhash = bits;
    return sig;
}

FrameDiffer::FrameDiffer(const AnalyzerConfig& cfg) : cfg_(cfg) {}

std::vector<uint8_t> FrameDiffer::resizeGray(const std::vector<uint8_t>& gray, int w, int h,
                                             int targetW) {
    if (targetW <= 0 || w <= targetW) return gray;
    const int factor = std::max(1, w / targetW);
    if (factor >= 2) {
        const int h2 = (h / factor) * factor;
        const int w2 = (w / factor) * factor;
        const int nh = h2 / factor;
        const int nw = w2 / factor;
        std::vector<uint8_t> small(static_cast<size_t>(nh) * nw);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                long sum = 0;
                int n = 0;
                for (int dy = 0; dy < factor; ++dy)
                    for (int dx = 0; dx < factor; ++dx) {
                        sum += gray[static_cast<size_t>((y * factor + dy) * w + (x * factor + dx))];
                        ++n;
                    }
                small[static_cast<size_t>(y) * nw + x] = static_cast<uint8_t>(sum / n);
            }
        }
        return small;
    }
    const int nh = std::max(1, static_cast<int>(std::lround(h * targetW / static_cast<double>(w))));
    std::vector<uint8_t> small(static_cast<size_t>(nh) * targetW);
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < targetW; ++x) {
            int sy = h > 1 ? y * (h - 1) / std::max(1, nh - 1) : 0;
            int sx = w > 1 ? x * (w - 1) / std::max(1, targetW - 1) : 0;
            small[static_cast<size_t>(y) * targetW + x] = gray[static_cast<size_t>(sy) * w + sx];
        }
    }
    return small;
}

DiffResult FrameDiffer::compute(const Frame& frame) {
    if (!frame.valid()) return DiffResult{};
    std::vector<uint8_t> gray(static_cast<size_t>(frame.width) * frame.height);
    for (int i = 0; i < frame.width * frame.height; ++i) {
        const uint8_t* p = frame.rgb.data() + static_cast<size_t>(i) * 3;
        gray[i] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    }
    const auto small = resizeGray(gray, frame.width, frame.height, cfg_.analysisWidth);

    const int smallW = (frame.width <= cfg_.analysisWidth)
                           ? frame.width
                           : std::max(1, frame.width / std::max(1, frame.width / cfg_.analysisWidth));
    const int smallH = static_cast<int>(small.size() / smallW);

    DiffResult result;
    if (prev_.empty() || prevW_ != smallW || prevH_ != smallH) {
        prev_ = small;
        prevW_ = smallW;
        prevH_ = smallH;
        scaleX_ = static_cast<double>(frame.width) / smallW;
        scaleY_ = static_cast<double>(frame.height) / smallH;
        result.changedRatio = 1.0;
        result.changedBbox = Rect{frame.originX, frame.originY, frame.width, frame.height};
        result.firstFrame = true;
        uint64_t hsh = 0xcbf29ce484222325ULL;
        for (uint8_t b : small) {
            hsh ^= b;
            hsh *= 0x100000001b3ULL;
        }
        result.frameHash = hsh;
        return result;
    }

    int changed = 0;
    std::vector<uint8_t> mask(static_cast<size_t>(smallW) * smallH);
    for (size_t i = 0; i < small.size(); ++i) {
        const int d = std::abs(static_cast<int>(small[i]) - static_cast<int>(prev_[i]));
        if (d > cfg_.pixelThreshold) {
            mask[i] = 1;
            ++changed;
        }
    }
    const double ratio = small.empty() ? 0.0 : static_cast<double>(changed) / small.size();
    result.changedRatio = ratio;

    if (changed > 0) {
        int minR = smallH, maxR = 0, minC = smallW, maxC = 0;
        for (int y = 0; y < smallH; ++y)
            for (int x = 0; x < smallW; ++x)
                if (mask[static_cast<size_t>(y) * smallW + x]) {
                    minR = std::min(minR, y);
                    maxR = std::max(maxR, y);
                    minC = std::min(minC, x);
                    maxC = std::max(maxC, x);
                }
        result.changedBbox = Rect{
            frame.originX + static_cast<int>(minC * scaleX_),
            frame.originY + static_cast<int>(minR * scaleY_),
            std::max(1, static_cast<int>((maxC - minC + 1) * scaleX_)),
            std::max(1, static_cast<int>((maxR - minR + 1) * scaleY_))};
        result.cells = changedCells(mask, smallW, smallH, frame);
    }

    uint64_t hsh = 0xcbf29ce484222325ULL;
    for (uint8_t b : small) {
        hsh ^= b;
        hsh *= 0x100000001b3ULL;
    }
    result.frameHash = hsh;

    prev_ = small;
    scaleX_ = static_cast<double>(frame.width) / smallW;
    scaleY_ = static_cast<double>(frame.height) / smallH;
    return result;
}

std::vector<Cell> FrameDiffer::changedCells(const std::vector<uint8_t>& mask, int w, int h,
                                           const Frame& frame) const {
    std::vector<Cell> out;
    const int g = std::max(4, cfg_.grid);
    const int gh = (h + g - 1) / g;
    const int gw = (w + g - 1) / g;
    for (int gy = 0; gy < gh; ++gy) {
        for (int gx = 0; gx < gw; ++gx) {
            int changed = 0;
            int total = 0;
            for (int dy = 0; dy < g; ++dy) {
                for (int dx = 0; dx < g; ++dx) {
                    const int y = gy * g + dy;
                    const int x = gx * g + dx;
                    if (y >= h || x >= w) continue;
                    ++total;
                    if (mask[static_cast<size_t>(y) * w + x]) ++changed;
                }
            }
            if (total == 0) continue;
            const double r = static_cast<double>(changed) / total;
            if (r >= 0.08) {
                out.push_back(Cell{Rect{frame.originX + static_cast<int>(gx * g * scaleX_),
                                        frame.originY + static_cast<int>(gy * g * scaleY_),
                                        std::max(1, static_cast<int>(g * scaleX_)),
                                        std::max(1, static_cast<int>(g * scaleY_))},
                                   r});
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Cell& a, const Cell& b) { return a.changedRatio > b.changedRatio; });
    if (out.size() > 200) out.resize(200);
    return out;
}

RegionSignature FrameDiffer::signature(const Frame& frame, const Rect& rect) const {
    Rect r{rect.x - frame.originX, rect.y - frame.originY, rect.w, rect.h};
    r = r.clipped(frame.width, frame.height);
    if (r.empty()) return RegionSignature{};
    std::vector<uint8_t> crop;
    crop.reserve(static_cast<size_t>(r.w) * r.h * 3);
    for (int y = 0; y < r.h; ++y) {
        const uint8_t* row = frame.rgb.data() + (static_cast<size_t>(r.y + y) * frame.width + r.x) * 3;
        crop.insert(crop.end(), row, row + r.w * 3);
    }
    return signatureFromRgb(crop, r.w, r.h);
}

RegionTracker::RegionTracker(const AnalyzerConfig& analyzer, const TranslateConfig& translate)
    : cfg_(analyzer), trCfg_(translate) {}

std::vector<TextRegion> RegionTracker::observe(const Frame& frame, const FrameDiffer& differ,
                                               const DiffResult& diff, int64_t nowNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TextRegion> dirty;
    std::vector<int> removeIds;
    for (auto& [id, region] : regions_) {
        Rect local{region.rect.x - frame.originX, region.rect.y - frame.originY, region.rect.w,
                   region.rect.h};
        local = local.clipped(frame.width, frame.height);
        if (local.empty()) {
            ++region.misses;
            if (region.misses >= 4) removeIds.push_back(id);
            continue;
        }
        RegionSignature sig = differ.signature(frame, local);
        const auto old = region.signature;
        region.signature = sig;
        region.lastPresentNs = nowNs;
        bool pixelHit = false;
        if (diff.changedBbox) pixelHit = diff.changedBbox->intersects(region.rect);
        bool sigHit = old.has_value() && sig.delta(*old) > cfg_.regionChange;
        if (pixelHit || sigHit) {
            if (!region.dirty) {
                region.dirty = true;
                region.lastChangedNs = nowNs;
            }
            dirty.push_back(region);
        }
    }
    for (int id : removeIds) regions_.erase(id);
    expireLocked(nowNs);
    std::vector<TextRegion> allDirty;
    for (const auto& [id, r] : regions_) {
        if (r.dirty) allDirty.push_back(r);
    }
    std::sort(allDirty.begin(), allDirty.end(),
              [](const TextRegion& a, const TextRegion& b) { return a.rect.area() > b.rect.area(); });
    return allDirty;
}

void RegionTracker::expire(int64_t nowNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(nowNs);
}

void RegionTracker::expireLocked(int64_t nowNs) {
    const int64_t ttl = static_cast<int64_t>(cfg_.expireMs) * 1000000LL;
    std::vector<int> dead;
    for (const auto& [id, r] : regions_) {
        if (nowNs - r.lastPresentNs > ttl || r.misses >= 5) dead.push_back(id);
    }
    for (int id : dead) regions_.erase(id);
}

RegionSignature RegionTracker::signatureForFrameRect(const Frame& frame, const Rect& rect) {
    Rect r{rect.x - frame.originX, rect.y - frame.originY, rect.w, rect.h};
    r = r.clipped(frame.width, frame.height);
    if (r.empty()) return RegionSignature{};
    std::vector<uint8_t> crop;
    crop.reserve(static_cast<size_t>(r.w) * r.h * 3);
    for (int y = 0; y < r.h; ++y) {
        const uint8_t* row = frame.rgb.data() + (static_cast<size_t>(r.y + y) * frame.width + r.x) * 3;
        crop.insert(crop.end(), row, row + r.w * 3);
    }
    return signatureFromRgb(crop, r.w, r.h);
}

void RegionTracker::ingestFull(const Frame& frame, const std::vector<OcrLine>& lines, int64_t nowNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OcrLine> ordered;
    for (const auto& l : lines) {
        if (!normalizeText(l.text).empty()) ordered.push_back(l);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const OcrLine& a, const OcrLine& b) { return a.rect.area() > b.rect.area(); });

    std::vector<int> used;
    for (const auto& line : ordered) {
        const std::string text = normalizeText(line.text);
        if (text.empty()) continue;
        int bestId = -1;
        double bestScore = 0;
        const auto lc = line.rect.center();
        for (const auto& [id, r] : regions_) {
            if (std::find(used.begin(), used.end(), id) != used.end()) continue;
            double score = r.rect.iou(line.rect);
            if (score < 0.20) {
                const auto rc = r.rect.center();
                const double dist = std::hypot(lc.first - rc.first, lc.second - rc.second);
                const double near = std::max(12.0, 0.6 * std::min(r.rect.w, r.rect.h));
                if (dist <= near) score = 0.15 * (1.0 - std::min(1.0, dist / near));
            }
            if (score > bestScore) {
                bestScore = score;
                bestId = id;
            }
        }
        if (bestId != -1 && bestScore >= 0.15) {
            used.push_back(bestId);
            auto& r = regions_[bestId];
            r.rect = line.rect;
            r.lastOcrNs = nowNs;
            r.lastPresentNs = nowNs;
            r.misses = 0;
            r.dirty = false;
            r.signature = signatureForFrameRect(frame, line.rect);
            if (text != r.text) {
                GFT_INFO("analyzer", "Metin değişti [%d]: %s -> %s", r.id, r.text.c_str(),
                         text.c_str());
                r.text = text;
                ++r.revision;
                r.stale = true;
                r.lastChangedNs = nowNs;
                r.lastTranslateAttemptNs = 0;
            } else if (!r.translation.empty()) {
                r.stale = false;
            }
            continue;
        }
        if (regions_.size() >= static_cast<size_t>(cfg_.maxRegions)) continue;
        int id = nextId_++;
        TextRegion region;
        region.id = id;
        region.rect = line.rect;
        region.text = text;
        region.stale = true;
        region.dirty = false;
        region.firstSeenNs = nowNs;
        region.lastChangedNs = nowNs;
        region.lastPresentNs = nowNs;
        region.lastOcrNs = nowNs;
        region.ocrSource = "full";
        region.signature = signatureForFrameRect(frame, line.rect);
        regions_[id] = region;
        used.push_back(id);
        GFT_INFO("analyzer", "Yeni metin bölgesi [%d] %s", id, text.c_str());
    }
}

void RegionTracker::ingestRegion(const Frame& frame, int regionId, const std::vector<OcrLine>& lines,
                                int64_t nowNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = regions_.find(regionId);
    if (it == regions_.end()) return;
    auto& region = it->second;
    std::vector<OcrLine> filtered;
    for (const auto& l : lines) {
        if (!normalizeText(l.text).empty()) filtered.push_back(l);
    }
    if (filtered.size() > 1) {
        const double cy = region.rect.center().second;
        std::vector<OcrLine> keep;
        for (const auto& l : filtered) {
            if (std::abs(l.rect.center().second - cy) <= std::max(region.rect.h * 0.75, 12.0))
                keep.push_back(l);
        }
        if (!keep.empty()) filtered = keep;
    }
    if (filtered.empty()) {
        ++region.misses;
        if (region.misses >= 4) {
            GFT_INFO("analyzer", "Bölge [%d] metni kayboldu, kaldırılıyor", regionId);
            regions_.erase(it);
        }
        return;
    }
    std::string merged;
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (i) merged += " ";
        merged += normalizeText(filtered[i].text);
    }
    merged = normalizeText(merged);
    if (merged.empty()) return;
    Rect rect = filtered[0].rect;
    for (size_t i = 1; i < filtered.size(); ++i) rect = rect.united(filtered[i].rect);
    region.rect = rect;
    region.lastOcrNs = nowNs;
    region.lastPresentNs = nowNs;
    region.misses = 0;
    region.dirty = false;
    region.signature = signatureForFrameRect(frame, rect);
    if (merged != region.text) {
        GFT_INFO("analyzer", "Bölge metni değişti [%d]: %s -> %s", regionId, region.text.c_str(),
                 merged.c_str());
        region.text = merged;
        ++region.revision;
        region.stale = true;
        region.lastChangedNs = nowNs;
        region.lastTranslateAttemptNs = 0;
    } else if (!region.translation.empty()) {
        region.stale = false;
    }
}

std::vector<TextRegion> RegionTracker::pendingTranslations(int64_t nowNs, int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t cooldown = static_cast<int64_t>(trCfg_.retryCooldownMs) * 1000000LL;
    std::vector<TextRegion> out;
    for (const auto& [id, r] : regions_) {
        if (r.text.empty() || !hasLetters(r.text)) continue;
        if (r.translation.empty() || r.stale) {
            if (nowNs - r.lastTranslateAttemptNs >= cooldown) out.push_back(r);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const TextRegion& a, const TextRegion& b) { return a.lastChangedNs < b.lastChangedNs; });
    if (static_cast<int>(out.size()) > limit) out.resize(std::max(1, limit));
    return out;
}

void RegionTracker::markTranslateAttempt(int regionId, int64_t nowNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = regions_.find(regionId);
    if (it != regions_.end()) it->second.lastTranslateAttemptNs = nowNs;
}

bool RegionTracker::setTranslation(int regionId, const std::string& translation,
                                   const std::string& provider, int64_t nowNs,
                                   std::optional<int> expectedRevision) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = regions_.find(regionId);
    if (it == regions_.end()) return false;
    auto& r = it->second;
    if (expectedRevision && r.revision != *expectedRevision) return false;
    if (translation.empty()) return false;
    r.translation = translation;
    r.translationProvider = provider;
    r.translatedAtNs = nowNs;
    r.stale = false;
    r.latencyMs = std::max(0.0, (nowNs - r.lastChangedNs) / 1e6);
    return true;
}

std::vector<OverlayItem> RegionTracker::overlayItems(int64_t nowNs, int ttlMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t ttl = static_cast<int64_t>(ttlMs) * 1000000LL;
    std::vector<OverlayItem> out;
    for (const auto& [id, r] : regions_) {
        if (r.translation.empty()) continue;
        if (nowNs - r.lastPresentNs > ttl) continue;
        out.push_back(OverlayItem{r.id, r.rect, r.translation, r.text, r.stale});
    }
    std::sort(out.begin(), out.end(), [](const OverlayItem& a, const OverlayItem& b) {
        if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
        return a.rect.x < b.rect.x;
    });
    return out;
}

std::tuple<int, int, int> RegionTracker::debugCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int stale = 0, translated = 0;
    for (const auto& [id, r] : regions_) {
        if (r.stale) ++stale;
        if (!r.translation.empty()) ++translated;
    }
    return {static_cast<int>(regions_.size()), translated, stale};
}

size_t RegionTracker::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return regions_.size();
}

std::optional<TextRegion> RegionTracker::get(int regionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = regions_.find(regionId);
    if (it == regions_.end()) return std::nullopt;
    return it->second;
}

}  // namespace gft
