#pragma once
// GameFrame Translator - ortak veri tipleri.
// Python prototipindeki (prototype/gameframe/types.py) alan adlarıyla uyumludur.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gft {

inline int64_t nowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Rect {
    int x{0};
    int y{0};
    int w{0};
    int h{0};

    [[nodiscard]] int right() const { return x + w; }
    [[nodiscard]] int bottom() const { return y + h; }
    [[nodiscard]] int area() const { return w > 0 && h > 0 ? w * h : 0; }
    [[nodiscard]] bool empty() const { return w <= 0 || h <= 0; }

    [[nodiscard]] Rect clipped(int width, int height) const {
        const int x1 = std::max(0, std::min(x, width));
        const int y1 = std::max(0, std::min(y, height));
        const int x2 = std::max(0, std::min(right(), width));
        const int y2 = std::max(0, std::min(bottom(), height));
        return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
    }

    [[nodiscard]] Rect expanded(int pad, int width, int height) const {
        Rect r{x - pad, y - pad, w + 2 * pad, h + 2 * pad};
        return r.clipped(width, height);
    }

    [[nodiscard]] Rect united(const Rect& o) const {
        const int x1 = std::min(x, o.x);
        const int y1 = std::min(y, o.y);
        const int x2 = std::max(right(), o.right());
        const int y2 = std::max(bottom(), o.bottom());
        return {x1, y1, x2 - x1, y2 - y1};
    }

    [[nodiscard]] bool intersects(const Rect& o) const {
        return !(right() <= o.x || o.right() <= x || bottom() <= o.y || o.bottom() <= y);
    }

    [[nodiscard]] Rect intersection(const Rect& o) const {
        const int x1 = std::max(x, o.x);
        const int y1 = std::max(y, o.y);
        const int x2 = std::min(right(), o.right());
        const int y2 = std::min(bottom(), o.bottom());
        return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
    }

    [[nodiscard]] double iou(const Rect& o) const {
        const int inter = intersection(o).area();
        if (inter <= 0) return 0.0;
        return static_cast<double>(inter) /
               static_cast<double>(area() + o.area() - inter);
    }

    [[nodiscard]] std::pair<double, double> center() const {
        return {x + w / 2.0, y + h / 2.0};
    }

    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct Frame {
    int width{0};
    int height{0};
    int originX{0};
    int originY{0};
    std::vector<uint8_t> rgb;  // RGB888, width*height*3
    int64_t capturedAtNs{0};
    uint64_t seq{0};
    std::string source;

    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0 &&
               rgb.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    }
};

struct OcrLine {
    Rect rect;
    std::string text;
    double confidence{0.0};
};

struct RegionSignature {
    uint64_t dhash{0};
    std::array<uint8_t, 256> thumb{};  // 16x16 gri

    [[nodiscard]] double delta(const RegionSignature& o) const {
        double sum = 0.0;
        for (size_t i = 0; i < thumb.size(); ++i) {
            sum += std::abs(static_cast<int>(thumb[i]) - static_cast<int>(o.thumb[i]));
        }
        return sum / (thumb.size() * 255.0);
    }
};

struct TextRegion {
    int id{0};
    Rect rect;
    std::string text;
    std::string translation;
    std::string translationProvider;
    std::optional<RegionSignature> signature;
    int revision{0};
    bool stale{true};
    bool dirty{false};
    int misses{0};
    int64_t firstSeenNs{0};
    int64_t lastChangedNs{0};
    int64_t lastPresentNs{0};
    int64_t lastOcrNs{0};
    int64_t translatedAtNs{0};
    int64_t lastTranslateAttemptNs{0};
    double latencyMs{0.0};
    std::string ocrSource;
};

struct OverlayItem {
    int regionId{0};
    Rect rect;
    std::string text;
    std::string sourceText;
    bool stale{false};
};

struct OverlayModel {
    std::vector<OverlayItem> items;
    int64_t updatedAtNs{0};
    std::string debug;
};

struct Cell {
    Rect rect;
    double changedRatio{0.0};
};

struct DiffResult {
    double changedRatio{1.0};
    std::optional<Rect> changedBbox;
    std::vector<Cell> cells;
    uint64_t frameHash{0};
    bool firstFrame{false};
};

inline std::string trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

inline std::string normalizeText(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool space = false;
    for (unsigned char c : input) {
        if (c == 0xAD || c == 0xE2) continue;  // kaba soft-hyphen/UTF-8 zero-width temizliği
        if (std::isspace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(c));
    }
    return trim(out);
}

inline bool hasLetters(const std::string& s) {
    for (unsigned char c : s) {
        if (std::isalpha(c)) return true;
    }
    return false;
}

}  // namespace gft
