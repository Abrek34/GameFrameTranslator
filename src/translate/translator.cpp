#include "translator.hpp"

#include <algorithm>
#include <chrono>
#include <curl/curl.h>
#include <fstream>
#include <json-c/json.h>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "common/log.hpp"
#include "common/types.hpp"

namespace gft {
namespace {

static const std::unordered_map<std::string, std::string> BUILTIN = {
    {"continue", "Devam"},
    {"press e to interact", "Etkileşim için E'ye bas"},
    {"press space to continue", "Devam etmek için Boşluk'a bas"},
    {"settings", "Ayarlar"},
    {"audio, video, controls", "Ses, Görüntü, Kontroller"},
    {"inventory", "Envanter"},
    {"health potion x3", "Can İksiri x3"},
    {"quest updated", "Görev güncellendi"},
    {"find the missing blacksmith", "Kayıp demirciyi bul"},
    {"game paused", "Oyun duraklatıldı"},
    {"resume   save   quit", "Devam Et   Kaydet   Çık"},
    {"level up!", "Seviye atladın!"},
    {"you reached level 12", "12. seviyeye ulaştın"},
    {"new game", "Yeni Oyun"},
    {"load game", "Oyun Yükle"},
    {"save game", "Oyunu Kaydet"},
    {"quit", "Çık"},
    {"exit", "Çıkış"},
    {"yes", "Evet"},
    {"no", "Hayır"},
    {"ok", "Tamam"},
    {"cancel", "İptal"},
    {"health", "Can"},
    {"mana", "Mana"},
    {"stamina", "Dayanıklılık"},
    {"attack", "Saldır"},
    {"defend", "Savun"},
    {"use", "Kullan"},
    {"equip", "Kuşan"},
    {"drop", "Bırak"},
    {"buy", "Satın Al"},
    {"sell", "Sat"},
    {"objective", "Hedef"},
    {"map", "Harita"},
    {"journal", "Günlük"},
    {"skills", "Yetenekler"},
    {"perks", "Avantajlar"},
    {"level", "Seviye"},
    {"experience", "Deneyim"},
    {"gold", "Altın"},
    {"armor", "Zırh"},
    {"weapon", "Silah"},
    {"shield", "Kalkan"},
    {"potion", "İksir"},
    {"quest", "Görev"},
    {"completed", "Tamamlandı"},
    {"failed", "Başarısız"},
    {"loading...", "Yükleniyor..."},
    {"please wait", "Lütfen bekleyin"},
    {"are you sure?", "Emin misiniz?"},
    {"game over", "Oyun Bitti"},
    {"paused", "Duraklatıldı"},
    {"resume", "Devam Et"},
    {"back", "Geri"},
    {"next", "İleri"},
    {"close", "Kapat"},
    {"open", "Aç"},
    {"select", "Seç"},
    {"start", "Başla"},
    {"credits", "Emeği Geçenler"},
    {"options", "Seçenekler"},
    {"difficulty", "Zorluk"},
    {"easy", "Kolay"},
    {"normal", "Normal"},
    {"hard", "Zor"},
    {"language", "Dil"},
    {"subtitles", "Altyazılar"},
    {"fullscreen", "Tam Ekran"},
    {"resolution", "Çözünürlük"},
    {"volume", "Ses Seviyesi"},
    {"keyboard", "Klavye"},
    {"mouse", "Fare"},
    {"gamepad", "Oyun Kolu"},
    {"sensitivity", "Hassasiyet"},
    {"tutorial", "Eğitim"},
    {"hint", "İpucu"},
    {"warning", "Uyarı"},
    {"error", "Hata"},
    {"unknown", "Bilinmiyor"},
    {"locked", "Kilitli"},
    {"unlocked", "Açık"},
    {"new", "Yeni"},
    {"welcome", "Hoş geldin"},
    {"good luck", "İyi şanslar"},
    {"thank you", "Teşekkürler"},
    {"hello", "Merhaba"},
    {"goodbye", "Hoşça kal"},
    {"follow me", "Beni takip et"},
    {"wait here", "Burada bekle"},
    {"help me", "Bana yardım et"},
    {"it's dangerous", "Bu tehlikeli"},
    {"you can't do that", "Bunu yapamazsın"},
    {"not enough gold", "Yeterli altın yok"},
    {"not enough mana", "Yeterli mana yok"},
    {"inventory is full", "Envanter dolu"},
    {"item acquired", "Eşya alındı"},
    {"quest complete", "Görev tamamlandı"},
    {"you died", "Öldün"},
    {"the blacksmith is missing", "Demirci kayıp"},
    {"press any key", "Herhangi bir tuşa basın"},
    {"are you sure?", "Emin misiniz?"},
    {"save changes?", "Değişiklikler kaydedilsin mi?"},
    {"paused", "Duraklatıldı"},
};

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}
}  // namespace

DictionaryTranslator::DictionaryTranslator(const std::string& dictPath) {
    for (const auto& [k, v] : BUILTIN) {
        std::string key = normalizeText(k);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        map_[key] = v;
    }
    if (dictPath.empty()) return;
    std::ifstream in(dictPath);
    if (!in) {
        GFT_WARN("translate", "Sözlük dosyası yok: %s", dictPath.c_str());
        return;
    }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string src = normalizeText(line.substr(0, tab));
        std::transform(src.begin(), src.end(), src.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string dst = trim(line.substr(tab + 1));
        if (!src.empty() && !dst.empty()) {
            map_[src] = dst;
            ++n;
        }
    }
    GFT_INFO("translate", "Sözlükten %d terim yüklendi", n);
}

std::optional<std::string> DictionaryTranslator::translate(const std::string& text,
                                                          const std::string& sourceLang,
                                                          const std::string& targetLang) {
    if (targetLang != "tr") return std::nullopt;
    (void)sourceLang;
    std::string key = normalizeText(text);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
}


std::optional<std::string> GoogleWebTranslator::translate(const std::string& text,
                                                         const std::string& sourceLang,
                                                         const std::string& targetLang) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;
    char* esc = curl_easy_escape(curl, text.c_str(), static_cast<int>(text.size()));
    std::ostringstream url;
    url << cfg_.googleEndpoint << "?client=gtx&sl=" << sourceLang << "&tl=" << targetLang
        << "&dt=t&q=" << (esc ? esc : "");
    if (esc) curl_free(esc);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeoutS));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GameFrameTranslator/0.1");
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        GFT_WARN("translate", "Google çeviri hatası: %s", curl_easy_strerror(rc));
        return std::nullopt;
    }

    json_object* root = json_tokener_parse(response.c_str());
    if (!root) return std::nullopt;
    std::string out;
    json_object* arr0 = nullptr;
    if (json_object_is_type(root, json_type_array) && json_object_array_length(root) > 0) {
        arr0 = json_object_array_get_idx(root, 0);
    }
    if (arr0 && json_object_is_type(arr0, json_type_array)) {
        const size_t n = json_object_array_length(arr0);
        for (size_t i = 0; i < n; ++i) {
            json_object* seg = json_object_array_get_idx(arr0, i);
            if (!seg || !json_object_is_type(seg, json_type_array) || json_object_array_length(seg) == 0)
                continue;
            json_object* first = json_object_array_get_idx(seg, 0);
            if (first && json_object_is_type(first, json_type_string)) {
                const char* s = json_object_get_string(first);
                if (s) out += s;
            }
        }
    }
    json_object_put(root);
    out = trim(out);
    if (out.empty()) return std::nullopt;
    return out;
}

TranslatorChain::TranslatorChain(const TranslateConfig& cfg, TranslationCache& cache,
                                std::vector<std::unique_ptr<ITranslator>> providers)
    : cfg_(cfg), cache_(cache), providers_(std::move(providers)) {}

TranslationResult TranslatorChain::translate(const std::string& text, const std::string& sourceLang,
                                            const std::string& targetLang) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string src = sourceLang.empty() ? cfg_.sourceLang : sourceLang;
    const std::string tgt = targetLang.empty() ? cfg_.targetLang : targetLang;
    const std::string norm = normalizeText(text);
    if (norm.empty()) return TranslationResult{"", "skip", false, true, 0.0};
    if (static_cast<int>(norm.size()) > cfg_.maxChars) {
        GFT_WARN("translate", "Metin çok uzun (%zu > %d)", norm.size(), cfg_.maxChars);
        return TranslationResult{norm, "too_long", false, false,
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count()};
    }
    if (!hasLetters(norm)) {
        return TranslationResult{norm, "skip", false, true,
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count()};
    }
    if (auto hit = cache_.get(src, tgt, norm)) {
        return TranslationResult{hit->first, hit->second, true, true,
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count()};
    }
    for (const auto& provider : providers_) {
        try {
            if (auto out = provider->translate(norm, src, tgt)) {
                const std::string value = trim(*out);
                if (!value.empty()) {
                    cache_.put(src, tgt, norm, value, provider->name());
                    return TranslationResult{value, provider->name(), false, true,
                                             std::chrono::duration<double, std::milli>(
                                                 std::chrono::steady_clock::now() - t0)
                                                 .count()};
                }
            }
        } catch (const std::exception& e) {
            GFT_WARN("translate", "%s sağlayıcısı hata verdi: %s", provider->name().c_str(),
                     e.what());
        }
    }
    std::string out;
    if (cfg_.fallback == "hide") out.clear();
    else if (cfg_.fallback == "marker") out = "[" + norm + "]";
    else out = norm;
    return TranslationResult{out, "fallback", false, false,
                             std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count()};
}

std::unique_ptr<TranslatorChain> createTranslator(const Config& cfg, TranslationCache& cache) {
    std::vector<std::unique_ptr<ITranslator>> providers;
    const std::string& p = cfg.translate.provider;
    if (p == "auto" || p == "dictionary") providers.push_back(std::make_unique<DictionaryTranslator>(cfg.translate.dictPath));
    if (p == "auto" || p == "google") providers.push_back(std::make_unique<GoogleWebTranslator>(cfg.translate));
    if (p == "none") providers.push_back(std::make_unique<NullTranslator>());
    if (providers.empty()) providers.push_back(std::make_unique<NullTranslator>());
    GFT_INFO("translate", "Çeviri sağlayıcıları: %s",
             ([&]() {
                 std::string s;
                 for (size_t i = 0; i < providers.size(); ++i) {
                     if (i) s += ", ";
                     s += providers[i]->name();
                 }
                 return s;
             })()
                 .c_str());
    return std::make_unique<TranslatorChain>(cfg.translate, cache, std::move(providers));
}

}  // namespace gft
