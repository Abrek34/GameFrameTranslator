#include "metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unistd.h>

#include "log.hpp"

namespace gft {
namespace {
double percentile(std::vector<double> vals, double p) {
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    const size_t idx = static_cast<size_t>(std::ceil((p / 100.0) * vals.size())) - 1;
    return vals[std::min(idx, vals.size() - 1)];
}

std::string fmt(double v, int precision = 2) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(precision) << v;
    return o.str();
}
}  // namespace

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string jsonString(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }
std::string jsonNumber(double v) { return fmt(v, 4); }
std::string jsonBool(bool v) { return v ? "true" : "false"; }

Metrics::Metrics(std::filesystem::path logDir, bool enabled, double statsIntervalS)
    : logDir_(std::move(logDir)), enabled_(enabled), statsIntervalS_(std::max(0.25, statsIntervalS)) {
    startedNs_ = nowNs();
    const std::time_t tt = std::time(nullptr);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    session_ = buf;
    if (enabled_) {
        std::error_code ec;
        std::filesystem::create_directories(logDir_, ec);
        logPath_ = logDir_ / ("metrics-" + session_ + ".jsonl");
        out_.open(logPath_, std::ios::app);
        if (!out_) {
            GFT_WARN("metrics", "Metrik dosyası açılamadı: %s", logPath_.c_str());
            enabled_ = false;
        } else {
            sampler_ = std::thread([this] { samplerLoop(); });
        }
    }
}

Metrics::~Metrics() { close(); }

void Metrics::event(const std::string& kind, const std::map<std::string, std::string>& fields) {
    if (!enabled_ || !out_) return;
    std::ostringstream o;
    o << "{\"t_ns\":" << nowNs() << ",\"wall\":" << std::time(nullptr)
      << ",\"kind\":" << jsonString(kind);
    for (const auto& [k, v] : fields) o << "," << jsonString(k) << ":" << v;
    o << "}\n";
    std::lock_guard<std::mutex> lock(mutex_);
    out_ << o.str();
}

void Metrics::inc(const std::string& name, int n) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[name] += n;
}

void Metrics::timing(const std::string& name, double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    timings_[name].push_back(ms);
}

void Metrics::latency(double ms, const std::map<std::string, std::string>& fields) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latencies_.push_back(ms);
    }
    auto all = fields;
    all["ms"] = jsonNumber(ms);
    event("latency", all);
}

void Metrics::overlay(const OverlayModel& model, double latencyMs) {
    std::ostringstream items;
    items << "[";
    for (size_t i = 0; i < model.items.size(); ++i) {
        const auto& item = model.items[i];
        if (i) items << ",";
        items << "{\"id\":" << item.regionId << ",\"rect\":[" << item.rect.x << ","
              << item.rect.y << "," << item.rect.w << "," << item.rect.h
              << "],\"text\":" << jsonString(item.text)
              << ",\"source\":" << jsonString(item.sourceText)
              << ",\"stale\":" << jsonBool(item.stale) << "}";
    }
    items << "]";
    std::map<std::string, std::string> fields;
    fields["items"] = items.str();
    fields["latency_ms"] = jsonNumber(latencyMs);
    inc("overlay_updates");
    event("overlay", fields);
}

int Metrics::counter(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = counters_.find(name);
    return it == counters_.end() ? 0 : it->second;
}

std::string Metrics::summaryJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream o;
    const double duration = (nowNs() - startedNs_) / 1e9;
    o << "{\"session\":" << jsonString(session_) << ",\"duration_s\":" << fmt(duration, 3)
      << ",\"counters\":{";
    bool first = true;
    for (const auto& [k, v] : counters_) {
        if (!first) o << ",";
        first = false;
        o << jsonString(k) << ":" << v;
    }
    o << "},\"latency_ms\":{\"count\":" << latencies_.size() << ",\"p50\":"
      << fmt(percentile(latencies_, 50)) << ",\"p95\":" << fmt(percentile(latencies_, 95))
      << ",\"max\":" << fmt(latencies_.empty() ? 0.0 : *std::max_element(latencies_.begin(), latencies_.end()))
      << "},\"timings_ms\":{";
    first = true;
    for (const auto& [k, v] : timings_) {
        if (v.empty()) continue;
        if (!first) o << ",";
        first = false;
        double sum = 0;
        for (double x : v) sum += x;
        o << jsonString(k) << ":{\"count\":" << v.size() << ",\"avg\":" << fmt(sum / v.size())
          << ",\"p95\":" << fmt(percentile(v, 95)) << "}";
    }
    double cpuSum = 0, cpuMax = 0, rssSum = 0, rssMax = 0;
    for (double x : cpuSamples_) {
        cpuSum += x;
        cpuMax = std::max(cpuMax, x);
    }
    for (double x : rssSamples_) {
        rssSum += x;
        rssMax = std::max(rssMax, x);
    }
    o << "},\"system\":{\"cpu_avg_pct\":" << fmt(cpuSamples_.empty() ? 0 : cpuSum / cpuSamples_.size())
      << ",\"cpu_max_pct\":" << fmt(cpuMax)
      << ",\"rss_avg_mb\":" << fmt(rssSamples_.empty() ? 0 : rssSum / rssSamples_.size(), 1)
      << ",\"rss_max_mb\":" << fmt(rssMax, 1) << "},\"metrics_path\":"
      << jsonString(logPath_.string()) << "}";
    return o.str();
}

std::filesystem::path Metrics::writeSummary() {
    if (!enabled_) return {};
    const auto path = logDir_ / ("summary-" + session_ + ".json");
    std::ofstream out(path);
    if (!out) return {};
    out << summaryJson() << "\n";
    return path;
}

void Metrics::close() {
    if (stop_.exchange(true)) return;
    if (sampler_.joinable()) sampler_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open()) out_.close();
}

std::optional<std::pair<double, double>> Metrics::readProc() const {
    try {
        std::ifstream stat("/proc/self/stat");
        std::string line;
        std::getline(stat, line);
        const auto close = line.rfind(')');
        if (close == std::string::npos) return std::nullopt;
        std::istringstream rest(line.substr(close + 1));
        std::vector<std::string> tokens;
        std::string token;
        while (rest >> token) tokens.push_back(token);
        if (tokens.size() < 13) return std::nullopt;
        const long ticks = sysconf(_SC_CLK_TCK);
        const double cpuSeconds =
            (std::stod(tokens[11]) + std::stod(tokens[12])) / static_cast<double>(ticks);
        double rssMb = 0;
        std::ifstream status("/proc/self/status");
        std::string sline;
        while (std::getline(status, sline)) {
            if (sline.rfind("VmRSS:", 0) == 0) {
                std::istringstream ss(sline.substr(6));
                double kb = 0;
                ss >> kb;
                rssMb = kb / 1024.0;
                break;
            }
        }
        return std::make_pair(cpuSeconds, rssMb);
    } catch (...) {
        return std::nullopt;
    }
}

void Metrics::samplerLoop() {
    while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(statsIntervalS_ * 1000)));
        if (stop_.load()) break;
        const auto proc = readProc();
        if (!proc) continue;
        const int64_t now = nowNs();
        if (lastProc_) {
            const double elapsed = std::max(1e-3, (now - lastSampleNs_) / 1e9);
            const double cpu = std::max(0.0, (proc->first - lastProc_->first) / elapsed * 100.0);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cpuSamples_.push_back(cpu);
                rssSamples_.push_back(proc->second);
            }
            event("system", {{"cpu_pct", jsonNumber(cpu)}, {"rss_mb", jsonNumber(proc->second)}});
        }
        lastProc_ = proc;
        lastSampleNs_ = now;
    }
}

}  // namespace gft
