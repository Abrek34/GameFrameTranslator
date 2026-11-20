#include "sqlite_cache.hpp"

#include <sqlite3.h>

#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {
namespace {
bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        GFT_WARN("cache", "SQLite: %s", err ? err : "bilinmeyen hata");
        sqlite3_free(err);
        return false;
    }
    return true;
}
}  // namespace

TranslationCache::TranslationCache(std::filesystem::path path, int memorySize)
    : path_(std::move(path)), memorySize_(static_cast<size_t>(std::max(16, memorySize))) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        GFT_ERROR("cache", "SQLite açılamadı: %s", sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 3000);
    exec(db_, "PRAGMA journal_mode=WAL;");
    exec(db_,
         "CREATE TABLE IF NOT EXISTS translations ("
         "key TEXT PRIMARY KEY, source_lang TEXT NOT NULL, target_lang TEXT NOT NULL,"
         "provider TEXT NOT NULL, source_text TEXT NOT NULL, translated TEXT NOT NULL,"
         "created_at INTEGER NOT NULL, hits INTEGER NOT NULL DEFAULT 0);");
    GFT_INFO("cache", "Çeviri önbelleği: %s", path_.c_str());
}

TranslationCache::~TranslationCache() { close(); }

std::string TranslationCache::makeKey(const std::string& sourceLang,
                                      const std::string& targetLang,
                                      const std::string& text) {
    return sourceLang + "\x1f" + targetLang + "\x1f" + normalizeText(text);
}

std::optional<std::pair<std::string, std::string>> TranslationCache::get(
    const std::string& sourceLang, const std::string& targetLang, const std::string& text) {
    const std::string norm = normalizeText(text);
    if (norm.empty()) return std::nullopt;
    const std::string key = makeKey(sourceLang, targetLang, norm);
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = memory_.find(key); it != memory_.end()) {
        ++memoryHits_;
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return std::make_pair(it->second.translated, it->second.provider);
    }
    ++misses_;
    if (!db_) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT translated, provider, hits FROM translations WHERE key=?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::pair<std::string, std::string>> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string translated = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const int hits = sqlite3_column_int(stmt, 2);
        result = std::make_pair(translated, provider);
        sqlite3_finalize(stmt);
        sqlite3_stmt* update = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE translations SET hits=? WHERE key=?", -1, &update,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_int(update, 1, hits + 1);
            sqlite3_bind_text(update, 2, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(update);
            sqlite3_finalize(update);
        }
        ++diskHits_;
        remember(key, translated, provider);
        return result;
    }
    sqlite3_finalize(stmt);
    return std::nullopt;
}

void TranslationCache::put(const std::string& sourceLang, const std::string& targetLang,
                           const std::string& text, const std::string& translated,
                           const std::string& provider) {
    const std::string norm = normalizeText(text);
    const std::string out = trim(translated);
    if (norm.empty() || out.empty()) return;
    const std::string key = makeKey(sourceLang, targetLang, norm);
    std::lock_guard<std::mutex> lock(mutex_);
    remember(key, out, provider);
    ++puts_;
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO translations (key,source_lang,target_lang,provider,source_text,translated,created_at,hits)"
            " VALUES (?,?,?,?,?,?,?,0) ON CONFLICT(key) DO UPDATE SET"
            " translated=excluded.translated, provider=excluded.provider,"
            " source_text=excluded.source_text",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sourceLang.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, targetLang.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, norm.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, out.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(std::time(nullptr)));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void TranslationCache::remember(const std::string& key, const std::string& translated,
                                const std::string& provider) {
    if (const auto it = memory_.find(key); it != memory_.end()) {
        it->second.translated = translated;
        it->second.provider = provider;
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return;
    }
    lru_.push_front(key);
    memory_.emplace(key, Entry{translated, provider, lru_.begin()});
    while (memory_.size() > memorySize_) {
        const std::string& old = lru_.back();
        memory_.erase(old);
        lru_.pop_back();
    }
}

std::unordered_map<std::string, int> TranslationCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{"memory_hits", memoryHits_},
            {"disk_hits", diskHits_},
            {"misses", misses_},
            {"puts", puts_},
            {"memory_entries", static_cast<int>(memory_.size())}};
}

void TranslationCache::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

}  // namespace gft
