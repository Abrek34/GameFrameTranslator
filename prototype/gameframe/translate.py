"""Çeviri katmanı.

ITranslator benzeri sağlayıcılar zincirlenir:
  önbellek -> sözlük (hızlı/çevrimdışı) -> Google web / LibreTranslate
API anahtarı gerektiren sağlayıcılar bu arayüzün arkasında kalır. Yeni bir
sağlayıcı eklemek için `translate()` metodu olan bir sınıf yazmak yeterlidir.
"""
from __future__ import annotations

import json
import logging
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Protocol

from .cache import TranslationCache
from .config import AppConfig, TranslateConfig
from .types import has_letters, normalize_text

log = logging.getLogger(__name__)


class Translator(Protocol):
    name: str

    def translate(self, text: str, source_lang: str, target_lang: str) -> Optional[str]:
        ...


# ---------------------------------------------------------------------------
# Sözlük
# ---------------------------------------------------------------------------
BUILTIN_DICTIONARY: dict[str, str] = {
    "continue": "Devam",
    "press e to interact": "Etkileşim için E'ye bas",
    "press space to continue": "Devam etmek için Boşluk'a bas",
    "press any key": "Herhangi bir tuşa basın",
    "settings": "Ayarlar",
    "audio, video, controls": "Ses, Görüntü, Kontroller",
    "audio": "Ses",
    "video": "Görüntü",
    "controls": "Kontroller",
    "inventory": "Envanter",
    "health potion x3": "Can İksiri x3",
    "quest updated": "Görev güncellendi",
    "find the missing blacksmith": "Kayıp demirciyi bul",
    "game paused": "Oyun duraklatıldı",
    "resume   save   quit": "Devam Et   Kaydet   Çık",
    "level up!": "Seviye atladın!",
    "you reached level 12": "12. seviyeye ulaştın",
    "new game": "Yeni Oyun",
    "load game": "Oyun Yükle",
    "save game": "Oyunu Kaydet",
    "quit": "Çık",
    "exit": "Çıkış",
    "yes": "Evet",
    "no": "Hayır",
    "ok": "Tamam",
    "cancel": "İptal",
    "health": "Can",
    "mana": "Mana",
    "stamina": "Dayanıklılık",
    "attack": "Saldır",
    "defend": "Savun",
    "use": "Kullan",
    "equip": "Kuşan",
    "drop": "Bırak",
    "buy": "Satın Al",
    "sell": "Sat",
    "objective": "Hedef",
    "map": "Harita",
    "journal": "Günlük",
    "skills": "Yetenekler",
    "perks": "Avantajlar",
    "level": "Seviye",
    "experience": "Deneyim",
    "gold": "Altın",
    "coins": "Sikkeler",
    "armor": "Zırh",
    "weapon": "Silah",
    "shield": "Kalkan",
    "potion": "İksir",
    "quest": "Görev",
    "completed": "Tamamlandı",
    "failed": "Başarısız",
    "loading...": "Yükleniyor...",
    "please wait": "Lütfen bekleyin",
    "are you sure?": "Emin misiniz?",
    "save changes?": "Değişiklikler kaydedilsin mi?",
    "game over": "Oyun Bitti",
    "paused": "Duraklatıldı",
    "resume": "Devam Et",
    "back": "Geri",
    "next": "İleri",
    "close": "Kapat",
    "open": "Aç",
    "select": "Seç",
    "start": "Başla",
    "credits": "Emeği Geçenler",
    "options": "Seçenekler",
    "difficulty": "Zorluk",
    "easy": "Kolay",
    "normal": "Normal",
    "hard": "Zor",
    "language": "Dil",
    "subtitles": "Altyazılar",
    "fullscreen": "Tam Ekran",
    "resolution": "Çözünürlük",
    "volume": "Ses Seviyesi",
    "master volume": "Ana Ses",
    "keyboard": "Klavye",
    "mouse": "Fare",
    "gamepad": "Oyun Kolu",
    "sensitivity": "Hassasiyet",
    "invert y axis": "Y Eksenini Ters Çevir",
    "tutorial": "Eğitim",
    "hint": "İpucu",
    "warning": "Uyarı",
    "error": "Hata",
    "unknown": "Bilinmiyor",
    "locked": "Kilitli",
    "unlocked": "Açık",
    "new": "Yeni",
    "welcome": "Hoş geldin",
    "good luck": "İyi şanslar",
    "thank you": "Teşekkürler",
    "hello": "Merhaba",
    "goodbye": "Hoşça kal",
    "i don't know": "Bilmiyorum",
    "what happened?": "Ne oldu?",
    "we need to go": "Gitmemiz gerek",
    "follow me": "Beni takip et",
    "wait here": "Burada bekle",
    "help me": "Bana yardım et",
    "it's dangerous": "Bu tehlikeli",
    "you can't do that": "Bunu yapamazsın",
    "not enough gold": "Yeterli altın yok",
    "not enough mana": "Yeterli mana yok",
    "inventory is full": "Envanter dolu",
    "item acquired": "Eşya alındı",
    "quest complete": "Görev tamamlandı",
    "you died": "Öldün",
    "the blacksmith is missing": "Demirci kayıp",
}


class DictionaryTranslator:
    name = "dictionary"

    def __init__(self, path: str = ""):
        self._map = {normalize_text(k).casefold(): v for k, v in BUILTIN_DICTIONARY.items()}
        if path:
            self._load(path)

    def _load(self, path: str) -> None:
        p = Path(path).expanduser()
        if not p.exists():
            log.warning("Sözlük dosyası yok: %s", p)
            return
        n = 0
        with p.open("r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "\t" not in line:
                    continue
                src, dst = line.split("\t", 1)
                src, dst = normalize_text(src), dst.strip()
                if src and dst:
                    self._map[src.casefold()] = dst
                    n += 1
        log.info("Sözlükten %d terim yüklendi: %s", n, p)

    def translate(self, text: str, source_lang: str, target_lang: str) -> Optional[str]:
        if target_lang != "tr":
            return None
        return self._map.get(normalize_text(text).casefold())


# ---------------------------------------------------------------------------
# Google web (anahtarsız uç nokta; üretimde resmi API'ye taşınmalı)
# ---------------------------------------------------------------------------
class GoogleWebTranslator:
    name = "google"

    def __init__(self, cfg: TranslateConfig):
        self.cfg = cfg

    def translate(self, text: str, source_lang: str, target_lang: str) -> Optional[str]:
        query = urllib.parse.urlencode(
            {
                "client": "gtx",
                "sl": source_lang,
                "tl": target_lang,
                "dt": "t",
                "q": text,
            }
        )
        url = f"{self.cfg.google_endpoint}?{query}"
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) GameFrameTranslator/0.1",
                "Accept": "application/json",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=float(self.cfg.timeout_s)) as resp:
                data = json.loads(resp.read().decode("utf-8", "replace"))
            segments = data[0] if isinstance(data, list) and data else []
            out = "".join(
                str(seg[0]) for seg in segments if isinstance(seg, list) and seg and seg[0]
            ).strip()
            return out or None
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as exc:
            log.warning("Google çeviri hatası: %s", exc)
            return None
        except Exception as exc:
            log.warning("Google çeviri beklenmeyen hata: %s", exc)
            return None


class LibreTranslateTranslator:
    name = "libre"

    def __init__(self, cfg: TranslateConfig):
        self.cfg = cfg

    def translate(self, text: str, source_lang: str, target_lang: str) -> Optional[str]:
        if not self.cfg.libre_endpoint:
            return None
        body = json.dumps(
            {"q": text, "source": source_lang, "target": target_lang, "format": "text"}
        ).encode("utf-8")
        req = urllib.request.Request(
            self.cfg.libre_endpoint,
            data=body,
            headers={"Content-Type": "application/json", "Accept": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=float(self.cfg.timeout_s)) as resp:
                data = json.loads(resp.read().decode("utf-8", "replace"))
            out = str(data.get("translatedText", "")).strip()
            return out or None
        except Exception as exc:
            log.warning("LibreTranslate hatası: %s", exc)
            return None


class NullTranslator:
    name = "none"

    def translate(self, text: str, source_lang: str, target_lang: str) -> Optional[str]:
        return None


# ---------------------------------------------------------------------------
@dataclass
class TranslationResult:
    text: str
    provider: str
    cached: bool = False
    ok: bool = True
    elapsed_ms: float = 0.0


class TranslatorChain:
    def __init__(self, cfg: TranslateConfig, cache: TranslationCache, providers: list[Translator]):
        self.cfg = cfg
        self.cache = cache
        self.providers = providers

    def translate(self, text: str, source_lang: Optional[str] = None, target_lang: Optional[str] = None) -> TranslationResult:
        t0 = time.perf_counter()
        src = source_lang or self.cfg.source_lang
        tgt = target_lang or self.cfg.target_lang
        norm = normalize_text(text)
        if not norm:
            return TranslationResult("", "skip", ok=True)
        if len(norm) > self.cfg.max_chars:
            log.warning("Metin çok uzun (%d > %d), çevrilmiyor", len(norm), self.cfg.max_chars)
            return self._fallback(norm, "too_long", t0)
        if not has_letters(norm):
            return TranslationResult(norm, "skip", ok=True, elapsed_ms=(time.perf_counter() - t0) * 1000)

        hit = self.cache.get(src, tgt, norm)
        if hit:
            translated, provider = hit
            return TranslationResult(
                translated,
                provider,
                cached=True,
                ok=True,
                elapsed_ms=(time.perf_counter() - t0) * 1000,
            )

        for provider in self.providers:
            try:
                out = provider.translate(norm, src, tgt)
            except Exception as exc:
                log.warning("%s sağlayıcısı hata verdi: %s", provider.name, exc)
                out = None
            if out and out.strip():
                out = out.strip()
                self.cache.put(src, tgt, norm, out, provider.name)
                return TranslationResult(
                    out,
                    provider.name,
                    cached=False,
                    ok=True,
                    elapsed_ms=(time.perf_counter() - t0) * 1000,
                )
        return self._fallback(norm, "fallback", t0)

    def _fallback(self, text: str, provider: str, t0: float) -> TranslationResult:
        mode = self.cfg.fallback
        if mode == "hide":
            out = ""
        elif mode == "marker":
            out = f"[{text}]"
        else:
            out = text
        return TranslationResult(
            out,
            provider,
            cached=False,
            ok=False,
            elapsed_ms=(time.perf_counter() - t0) * 1000,
        )


def create_translator(cfg: AppConfig, cache: TranslationCache) -> TranslatorChain:
    provider = cfg.translate.provider
    providers: list[Translator] = []
    if provider in {"auto", "dictionary"}:
        providers.append(DictionaryTranslator(cfg.translate.dict_path))
    if provider in {"auto", "google"}:
        providers.append(GoogleWebTranslator(cfg.translate))
    if provider in {"auto", "libre"} and cfg.translate.libre_endpoint:
        providers.append(LibreTranslateTranslator(cfg.translate))
    if provider == "none":
        providers.append(NullTranslator())
    if not providers:
        log.warning("Hiç çeviri sağlayıcısı yapılandırılmadı; fallback kullanılacak")
        providers.append(NullTranslator())
    log.info("Çeviri sağlayıcıları: %s", ", ".join(p.name for p in providers))
    return TranslatorChain(cfg.translate, cache, providers)
