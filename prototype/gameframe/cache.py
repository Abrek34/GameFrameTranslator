"""Çeviri önbelleği: bellek içi LRU + SQLite kalıcı katman.

Anahtar kaynak/target dil + normalize metindir. Sağlayıcı anahtara dahil
edilmez; böylece sözlükten gelen sonuç Google'a da cevap olur. Sağlayıcı
bilgisi yalnızca istatistik için saklanır.
"""
from __future__ import annotations

import hashlib
import logging
import os
import sqlite3
import threading
import time
from collections import OrderedDict
from pathlib import Path
from typing import Optional

from .types import normalize_text

log = logging.getLogger(__name__)


class TranslationCache:
    def __init__(self, path: str | Path, memory_size: int = 512):
        self.path = Path(path)
        self.memory_size = max(16, int(memory_size))
        self._lock = threading.RLock()
        self._mem: OrderedDict[str, tuple[str, str, int]] = OrderedDict()
        self._db: Optional[sqlite3.Connection] = None
        self.memory_hits = 0
        self.disk_hits = 0
        self.misses = 0
        self.puts = 0
        self._open()

    # ------------------------------------------------------------------
    @staticmethod
    def make_key(source_lang: str, target_lang: str, text: str) -> str:
        norm = normalize_text(text)
        raw = f"{source_lang}\x1f{target_lang}\x1f{norm}".encode("utf-8")
        return hashlib.sha256(raw).hexdigest()

    def _open(self) -> None:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._db = sqlite3.connect(str(self.path), timeout=5.0, check_same_thread=False)
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.execute("PRAGMA busy_timeout=3000")
            self._db.execute(
                """
                CREATE TABLE IF NOT EXISTS translations (
                    key TEXT PRIMARY KEY,
                    source_lang TEXT NOT NULL,
                    target_lang TEXT NOT NULL,
                    provider TEXT NOT NULL,
                    source_text TEXT NOT NULL,
                    translated TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    hits INTEGER NOT NULL DEFAULT 0
                )
                """
            )
            self._db.commit()
            log.info("Çeviri önbelleği: %s", self.path)
        except Exception as exc:
            log.error("Önbellek açılamadı (%s); yalnızca bellek kullanılacak", exc)
            self._db = None

    # ------------------------------------------------------------------
    def get(self, source_lang: str, target_lang: str, text: str) -> Optional[tuple[str, str]]:
        norm = normalize_text(text)
        if not norm:
            return None
        key = self.make_key(source_lang, target_lang, norm)
        with self._lock:
            hit = self._mem.get(key)
            if hit is not None:
                self._mem.move_to_end(key)
                self.memory_hits += 1
                return hit[0], hit[1]
            self.misses += 1
            if self._db is None:
                return None
            try:
                row = self._db.execute(
                    "SELECT translated, provider, hits FROM translations WHERE key=?",
                    (key,),
                ).fetchone()
                if not row:
                    return None
                translated, provider, hits = str(row[0]), str(row[1]), int(row[2])
                self._db.execute(
                    "UPDATE translations SET hits=? WHERE key=?", (hits + 1, key)
                )
                self._db.commit()
                self.disk_hits += 1
                self._remember(key, translated, provider)
                return translated, provider
            except Exception as exc:
                log.warning("Önbellek okuma hatası: %s", exc)
                return None

    def put(self, source_lang: str, target_lang: str, text: str, translated: str, provider: str) -> None:
        norm = normalize_text(text)
        translated = (translated or "").strip()
        if not norm or not translated:
            return
        key = self.make_key(source_lang, target_lang, norm)
        with self._lock:
            self._remember(key, translated, provider)
            self.puts += 1
            if self._db is None:
                return
            try:
                self._db.execute(
                    """
                    INSERT INTO translations
                        (key, source_lang, target_lang, provider, source_text, translated, created_at, hits)
                    VALUES (?, ?, ?, ?, ?, ?, ?, 0)
                    ON CONFLICT(key) DO UPDATE SET
                        translated=excluded.translated,
                        provider=excluded.provider,
                        source_text=excluded.source_text
                    """,
                    (key, source_lang, target_lang, provider, norm, translated, int(time.time())),
                )
                self._db.commit()
            except Exception as exc:
                log.warning("Önbellek yazma hatası: %s", exc)

    def _remember(self, key: str, translated: str, provider: str) -> None:
        self._mem[key] = (translated, provider, int(time.time()))
        self._mem.move_to_end(key)
        while len(self._mem) > self.memory_size:
            self._mem.popitem(last=False)

    def stats(self) -> dict[str, int]:
        with self._lock:
            return {
                "memory_hits": self.memory_hits,
                "disk_hits": self.disk_hits,
                "misses": self.misses,
                "puts": self.puts,
                "memory_entries": len(self._mem),
            }

    def close(self) -> None:
        with self._lock:
            if self._db is not None:
                try:
                    self._db.commit()
                    self._db.close()
                except Exception:
                    pass
                self._db = None
