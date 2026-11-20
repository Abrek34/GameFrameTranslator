#include "ocr_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#include "common/config.hpp"
#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {
namespace {

bool sanitizeLang(const std::string& lang) {
    if (lang.empty()) return false;
    for (char c : lang) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '_'))
            return false;
    }
    return true;
}

std::string safeLang(const std::string& lang) {
    return sanitizeLang(lang) ? lang : "eng";
}

std::vector<uint8_t> resizeNearest(const std::vector<uint8_t>& src, int sw, int sh,
                                   int dw, int dh) {
    if (sw == dw && sh == dh) return src;
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 3);
    for (int y = 0; y < dh; ++y) {
        const int sy = sh > 1 ? y * (sh - 1) / std::max(1, dh - 1) : 0;
        for (int x = 0; x < dw; ++x) {
            const int sx = sw > 1 ? x * (sw - 1) / std::max(1, dw - 1) : 0;
            const size_t si = (static_cast<size_t>(sy) * sw + sx) * 3;
            const size_t di = (static_cast<size_t>(y) * dw + x) * 3;
            dst[di] = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
        }
    }
    return dst;
}

std::string trimStr(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, delim)) out.push_back(cur);
    if (!s.empty() && s.back() == delim) out.emplace_back();
    return out;
}

class TesseractCliOcr : public OcrEngine {
public:
    explicit TesseractCliOcr(const OcrConfig& cfg) : cfg_(cfg) {}

    std::string name() const override { return "tesseract"; }

    std::vector<OcrLine> recognize(const Frame& frame, const std::optional<Rect>& roi,
                                    int psm, double upscale) override {
        if (!frame.valid()) return {};
        Rect local;
        if (roi) {
            local = Rect{roi->x - frame.originX, roi->y - frame.originY, roi->w, roi->h};
            local = local.clipped(frame.width, frame.height);
        } else {
            local = Rect{0, 0, frame.width, frame.height};
        }
        if (local.empty()) return {};

        // crop
        std::vector<uint8_t> crop;
        crop.reserve(static_cast<size_t>(local.w) * local.h * 3);
        for (int y = 0; y < local.h; ++y) {
            const uint8_t* row = frame.rgb.data() +
                                 (static_cast<size_t>(local.y + y) * frame.width + local.x) * 3;
            crop.insert(crop.end(), row, row + local.w * 3);
        }

        double scale = upscale > 0 ? upscale : cfg_.upscale;
        if (local.h < cfg_.autoUpscaleBelowPx) scale = std::max(scale, 2.0);
        if (std::abs(scale - 1.0) > 0.01) {
            const int nw = std::max(1, static_cast<int>(std::lround(local.w * scale)));
            const int nh = std::max(1, static_cast<int>(std::lround(local.h * scale)));
            crop = resizeNearest(crop, local.w, local.h, nw, nh);
            local.w = nw;
            local.h = nh;
            // scale factor already applied; store original scale for coordinate division.
        }
        const double usedScale = scale;

        // write PPM
        char pathBuf[] = "/tmp/gft-ocr-XXXXXX.ppm";
        const int fd = mkstemps(pathBuf, 4);
        if (fd < 0) throw OcrError("geçici dosya oluşturulamadı");
        {
            std::string header = "P6\n" + std::to_string(local.w) + " " +
                                 std::to_string(local.h) + "\n255\n";
            if (write(fd, header.data(), header.size()) < 0) {
                close(fd);
                throw OcrError("PPM başlığı yazılamadı");
            }
            size_t off = 0;
            while (off < crop.size()) {
                const ssize_t n = write(fd, crop.data() + off, crop.size() - off);
                if (n < 0) {
                    close(fd);
                    throw OcrError("PPM verisi yazılamadı");
                }
                off += static_cast<size_t>(n);
            }
            close(fd);
        }

        std::string output;
        try {
            const std::string lang = safeLang(cfg_.lang);
            std::ostringstream cmd;
            cmd << "timeout " << std::max(1, static_cast<int>(cfg_.timeoutS) + 2) << "s "
                << cfg_.tesseractCmd << " " << pathBuf << " stdout -l " << lang
                << " --psm " << psm << " --oem 1 tsv 2>/dev/null";
            FILE* pipe = popen(cmd.str().c_str(), "r");
            if (!pipe) throw OcrError("tesseract çalıştırılamadı");
            char buf[4096];
            while (fgets(buf, sizeof(buf), pipe)) output += buf;
            const int status = pclose(pipe);
            if (status != 0 && output.empty()) {
                throw OcrError("Tesseract hatası, çıkış kodu " + std::to_string(status));
            }
        } catch (...) {
            unlink(pathBuf);
            throw;
        }
        unlink(pathBuf);

        // parse TSV
        struct Group {
            std::vector<std::string> words;
            int x1{0}, y1{0}, x2{0}, y2{0};
            std::vector<double> conf;
        };
        std::map<std::tuple<int, int, int, int>, Group> groups;
        std::vector<std::tuple<int, int, int, int>> order;

        std::istringstream in(output);
        std::string row;
        std::getline(in, row);  // header
        while (std::getline(in, row)) {
            if (row.empty()) continue;
            const auto cols = split(row, '\t');
            if (cols.size() < 12) continue;
            int level = 0, page = 0, block = 0, par = 0, line = 0, left = 0, top = 0, width = 0,
                height = 0;
            double conf = 0;
            try {
                level = std::stoi(cols[0]);
                if (level != 5) continue;
                page = std::stoi(cols[1]);
                block = std::stoi(cols[2]);
                par = std::stoi(cols[3]);
                line = std::stoi(cols[4]);
                left = std::stoi(cols[6]);
                top = std::stoi(cols[7]);
                width = std::stoi(cols[8]);
                height = std::stoi(cols[9]);
                conf = std::stod(cols[10]);
            } catch (...) {
                continue;
            }
            std::string word = trimStr(cols[11]);
            if (word.empty() || conf < cfg_.minConfidence) continue;
            auto key = std::make_tuple(page, block, par, line);
            if (groups.find(key) == groups.end()) {
                groups[key] = Group{ {}, left, top, left + width, top + height, {} };
                order.push_back(key);
            }
            auto& g = groups[key];
            g.words.push_back(word);
            g.x1 = std::min(g.x1, left);
            g.y1 = std::min(g.y1, top);
            g.x2 = std::max(g.x2, left + width);
            g.y2 = std::max(g.y2, top + height);
            g.conf.push_back(conf);
        }

        std::vector<OcrLine> out;
        for (const auto& key : order) {
            const auto& g = groups[key];
            std::string text;
            for (size_t i = 0; i < g.words.size(); ++i) {
                if (i) text += " ";
                text += g.words[i];
            }
            text = normalizeText(text);
            if (text.empty()) continue;
            bool has = false;
            for (unsigned char c : text)
                if (std::isalnum(c)) {
                    has = true;
                    break;
                }
            if (!has) continue;
            const int x = local.x + static_cast<int>(std::lround(g.x1 / usedScale));
            const int y = local.y + static_cast<int>(std::lround(g.y1 / usedScale));
            const int w = std::max(1, static_cast<int>(std::lround((g.x2 - g.x1) / usedScale)));
            const int h = std::max(1, static_cast<int>(std::lround((g.y2 - g.y1) / usedScale)));
            if (h < cfg_.minTextHeight) continue;
            double avgConf = 0;
            if (!g.conf.empty()) {
                double sum = 0;
                for (double c : g.conf) sum += c;
                avgConf = sum / g.conf.size();
            }
            out.push_back(OcrLine{Rect{frame.originX + x, frame.originY + y, w, h}, text, avgConf});
        }
        std::sort(out.begin(), out.end(),
                  [](const OcrLine& a, const OcrLine& b) {
                      if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
                      return a.rect.x < b.rect.x;
                  });
        return out;
    }

private:
    OcrConfig cfg_;
};

class NullOcr : public OcrEngine {
public:
    std::string name() const override { return "null"; }
    std::vector<OcrLine> recognize(const Frame&, const std::optional<Rect>&, int, double) override {
        return {};
    }
};

}  // namespace

std::unique_ptr<OcrEngine> createOcr(const Config& cfg) {
    if (cfg.ocr.backend == "null") return std::make_unique<NullOcr>();
    if (cfg.ocr.backend == "tesseract") return std::make_unique<TesseractCliOcr>(cfg.ocr);
    if (cfg.ocr.backend == "auto") {
        if (access(cfg.ocr.tesseractCmd.c_str(), X_OK) == 0)
            return std::make_unique<TesseractCliOcr>(cfg.ocr);
    }
    GFT_WARN("ocr", "OCR backend bulunamadı; null kullanılıyor");
    return std::make_unique<NullOcr>();
}

}  // namespace gft
