#pragma once
// Basit, bağımlılıksız loglama. Üretimde spdlog'a geçiş için tek nokta.
#include <cstdarg>
#include <string>

namespace gft {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

void setLogLevel(LogLevel level);
LogLevel logLevel();
void logMessage(LogLevel level, const char* module, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

}  // namespace gft

#define GFT_LOG(level, module, ...) ::gft::logMessage(level, module, __VA_ARGS__)
#define GFT_TRACE(module, ...) GFT_LOG(::gft::LogLevel::Trace, module, __VA_ARGS__)
#define GFT_DEBUG(module, ...) GFT_LOG(::gft::LogLevel::Debug, module, __VA_ARGS__)
#define GFT_INFO(module, ...) GFT_LOG(::gft::LogLevel::Info, module, __VA_ARGS__)
#define GFT_WARN(module, ...) GFT_LOG(::gft::LogLevel::Warn, module, __VA_ARGS__)
#define GFT_ERROR(module, ...) GFT_LOG(::gft::LogLevel::Error, module, __VA_ARGS__)
