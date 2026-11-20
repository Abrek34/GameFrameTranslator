#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/sqlite_cache.hpp"
#include "common/config.hpp"
#include "common/types.hpp"

namespace gft {

struct TranslationResult {
    std::string text;
    std::string provider;
    bool cached{false};
    bool ok{true};
    double elapsedMs{0.0};
};

class ITranslator {
public:
    virtual ~ITranslator() = default;
    virtual std::string name() const = 0;
    virtual std::optional<std::string> translate(const std::string& text,
                                                const std::string& sourceLang,
                                                const std::string& targetLang) = 0;
};

class DictionaryTranslator : public ITranslator {
public:
    explicit DictionaryTranslator(const std::string& dictPath = "");
    std::string name() const override { return "dictionary"; }
    std::optional<std::string> translate(const std::string& text, const std::string& sourceLang,
                                         const std::string& targetLang) override;

private:
    std::unordered_map<std::string, std::string> map_;
};

class GoogleWebTranslator : public ITranslator {
public:
    explicit GoogleWebTranslator(const TranslateConfig& cfg) : cfg_(cfg) {}
    std::string name() const override { return "google"; }
    std::optional<std::string> translate(const std::string& text, const std::string& sourceLang,
                                         const std::string& targetLang) override;

private:
    TranslateConfig cfg_;
};

class NullTranslator : public ITranslator {
public:
    std::string name() const override { return "none"; }
    std::optional<std::string> translate(const std::string&, const std::string&,
                                         const std::string&) override {
        return std::nullopt;
    }
};

class TranslatorChain {
public:
    TranslatorChain(const TranslateConfig& cfg, TranslationCache& cache,
                    std::vector<std::unique_ptr<ITranslator>> providers);
    TranslationResult translate(const std::string& text,
                                const std::string& sourceLang = "",
                                const std::string& targetLang = "");

private:
    TranslateConfig cfg_;
    TranslationCache& cache_;
    std::vector<std::unique_ptr<ITranslator>> providers_;
};

std::unique_ptr<TranslatorChain> createTranslator(const Config& cfg, TranslationCache& cache);

}  // namespace gft
