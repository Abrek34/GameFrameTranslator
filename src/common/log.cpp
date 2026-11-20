#include "log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace gft {
namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_logMutex;

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}
}  // namespace

void setLogLevel(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }
LogLevel logLevel() { return g_level.load(std::memory_order_relaxed); }

void logMessage(LogLevel level, const char* module, const char* fmt, ...) {
    if (static_cast<int>(level) < static_cast<int>(g_level.load(std::memory_order_relaxed))) return;
    char message[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::fprintf(stderr, "%02d:%02d:%02d.%03lld %-5s %s: %s\n", tm.tm_hour, tm.tm_min,
                 tm.tm_sec, static_cast<long long>(ms.count()), levelName(level), module,
                 message);
    std::fflush(stderr);
}

}  // namespace gft
