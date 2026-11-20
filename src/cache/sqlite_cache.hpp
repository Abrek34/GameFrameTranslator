#pragma once
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

struct sqlite3;

namespace gft {

// Bellek LRU + SQLite kalıcı çeviri önbelleği. Sağlayıcı anahtara dahil değil;
// aynı metin farklı sağlayıcıdan da olsa tekrar kullanılır.
class TranslationCache {
public:
    TranslationCache(std::filesystem::path path, int memorySize = 512);
    ~TranslationCache();

    TranslationCache(const TranslationCache&) = delete;
    TranslationCache& operator=(const TranslationCache&) = delete;

    std::optional<std::pair<std::string, std::string>> get(const std::string& sourceLang,
                                                           const std::string& targetLang,
                                                           const std::string& text);
    void put(const std::string& sourceLang, const std::string& targetLang,
             const std::string& text, const std::string& translated,
             const std::string& provider);
    [[nodiscard]] std::unordered_map<std::string, int> stats() const;
    void close();

private:
    struct Entry {
        std::string translated;
        std::string provider;
        std::list<std::string>::iterator lru;
    };
    static std::string makeKey(const std::string& sourceLang, const std::string& targetLang,
                               const std::string& text);
    void remember(const std::string& key, const std::string& translated,
                  const std::string& provider);

    std::filesystem::path path_;
    size_t memorySize_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> memory_;
    std::list<std::string> lru_;
    sqlite3* db_{nullptr};
    int memoryHits_{0};
    int diskHits_{0};
    int misses_{0};
    int puts_{0};
};

}  // namespace gft
