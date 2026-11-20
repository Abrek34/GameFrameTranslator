#pragma once
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "types.hpp"

namespace gft {

std::string jsonEscape(const std::string& s);
std::string jsonString(const std::string& s);
std::string jsonNumber(double v);
std::string jsonBool(bool v);

// JSONL olayları + gecikme/CPU/RSS özeti. Benchmark, aynı CLOCK_MONOTONIC
// üzerinden Python sentetik sahnesiyle eşleştirilebilir.
class Metrics {
public:
    Metrics(std::filesystem::path logDir, bool enabled, double statsIntervalS);
    ~Metrics();

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    void event(const std::string& kind, const std::map<std::string, std::string>& fields = {});
    void inc(const std::string& name, int n = 1);
    void timing(const std::string& name, double ms);
    void latency(double ms, const std::map<std::string, std::string>& fields = {});
    void overlay(const OverlayModel& model, double latencyMs);

    [[nodiscard]] std::string summaryJson() const;
    std::filesystem::path writeSummary();
    [[nodiscard]] int counter(const std::string& name) const;
    [[nodiscard]] const std::string& session() const { return session_; }
    [[nodiscard]] std::filesystem::path logPath() const { return logPath_; }
    void close();

private:
    void samplerLoop();
    std::optional<std::pair<double, double>> readProc() const;

    std::filesystem::path logDir_;
    std::filesystem::path logPath_;
    std::string session_;
    bool enabled_{false};
    double statsIntervalS_{1.0};
    int64_t startedNs_{0};
    mutable std::mutex mutex_;
    std::ofstream out_;
    std::map<std::string, int> counters_;
    std::vector<double> latencies_;
    std::unordered_map<std::string, std::vector<double>> timings_;
    std::vector<double> cpuSamples_;
    std::vector<double> rssSamples_;
    std::atomic<bool> stop_{false};
    std::thread sampler_;
    std::optional<std::pair<double, double>> lastProc_;
    int64_t lastSampleNs_{0};
};

}  // namespace gft
