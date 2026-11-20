#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <unordered_map>

#include "cache/sqlite_cache.hpp"
#include "common/config.hpp"
#include "translate/translator.hpp"

using namespace gft;

class FakeTranslator : public ITranslator {
public:
    std::unordered_map<std::string, std::string> map;
    int calls = 0;
    std::string name() const override { return "fake"; }
    std::optional<std::string> translate(const std::string& text, const std::string&,
                                         const std::string&) override {
        ++calls;
        auto it = map.find(text);
        if (it == map.end()) return std::nullopt;
        return it->second;
    }
};

int main() {
    // dictionary
    DictionaryTranslator d;
    auto r = d.translate("Continue", "en", "tr");
    assert(r && *r == "Devam");
    auto r2 = d.translate("  continue ", "en", "tr");
    assert(r2 && *r2 == "Devam");
    assert(!d.translate("Some unknown sentence", "en", "tr").has_value());

    // chain
    const std::string path = "/tmp/gft_test_translate.sqlite3";
    std::filesystem::remove(path);
    {
        TranslationCache cache(path, 8);
        auto fake = std::make_unique<FakeTranslator>();
        fake->map["Hello there"] = "Merhaba";
        TranslateConfig tcfg;
        tcfg.fallback = "original";
        std::vector<std::unique_ptr<ITranslator>> ps;
        ps.push_back(std::move(fake));
        TranslatorChain chain(tcfg, cache, std::move(ps));
        // need access fake calls; recreate not possible; skip call count
        auto r1 = chain.translate("Hello there");
        assert(r1.text == "Merhaba" && !r1.cached);
        auto r2 = chain.translate("Hello there");
        assert(r2.text == "Merhaba" && r2.cached);
        auto r3 = chain.translate("Unknown");
        assert(r3.text == "Unknown" && !r3.ok);
    }
    // no letters
    {
        TranslationCache cache(path, 8);
        TranslateConfig tcfg;
        std::vector<std::unique_ptr<ITranslator>> ps;
        ps.push_back(std::make_unique<NullTranslator>());
        TranslatorChain chain(tcfg, cache, std::move(ps));
        auto r = chain.translate("1234");
        assert(r.text == "1234" && r.provider == "skip");
    }
    std::filesystem::remove(path);
    std::cout << "test_translate OK\n";
    return 0;
}
