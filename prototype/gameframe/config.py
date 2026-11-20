"""Yapılandırma: dataclass varsayılanları + YAML yükleme.

Örnek dosya: prototype/config.example.yaml
"""
from __future__ import annotations

import logging
import os
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path
from typing import Any, Optional

import yaml

log = logging.getLogger(__name__)


@dataclass
class CaptureConfig:
    # auto: window_title varsa xcomposite, yoksa mss (root)
    backend: str = "auto"
    monitor: int = 1
    region: Optional[tuple[int, int, int, int]] = None  # x, y, w, h (ekran)
    window_title: str = ""
    window_class: str = ""
    window_id: Optional[int] = None
    window_pid: Optional[int] = None
    fps: int = 30
    display: str = ""
    # Root yakalama overlay'i de yakalar. XComposite hedef pencere kullanın.
    allow_root_with_overlay: bool = False


@dataclass
class AnalyzerConfig:
    analysis_width: int = 640
    grid: int = 16
    pixel_threshold: int = 16
    change_trigger: float = 0.0025  # tam karede değişen piksel oranı
    region_change: float = 0.015  # bölge küçük resminde ortalama fark
    discovery_ms: int = 700
    heartbeat_ms: int = 3000
    expire_ms: int = 6000
    region_padding: int = 6
    max_regions: int = 32
    max_region_ocr_per_cycle: int = 2
    min_dirty_frames: int = 1
    max_analysis_fps: float = 15.0


@dataclass
class OcrConfig:
    backend: str = "auto"  # auto | tesseract | rapidocr | null
    lang: str = "eng"
    psm: int = 11
    region_psm: int = 7
    min_confidence: float = 45.0
    max_calls_per_sec: float = 4.0
    burst: int = 2
    upscale: float = 1.0
    region_upscale: float = 1.5
    auto_upscale_below_px: int = 26
    min_text_height: int = 8
    timeout_s: float = 5.0
    tesseract_cmd: str = "tesseract"
    omp_thread_limit: int = 2


@dataclass
class TranslateConfig:
    provider: str = "auto"  # auto | dictionary | google | libre | none
    source_lang: str = "en"
    target_lang: str = "tr"
    timeout_s: float = 4.0
    max_chars: int = 500
    fallback: str = "original"  # original | hide | marker
    google_endpoint: str = "https://translate.googleapis.com/translate_a/single"
    libre_endpoint: str = ""
    dict_path: str = ""
    retry_cooldown_ms: int = 2000
    max_translations_per_cycle: int = 4


@dataclass
class OverlayConfig:
    backend: str = "auto"  # auto | gtk | tk | none
    monitor: int = 1
    gdk_backend: str = "x11"  # Wayland'de XWayland overlay; native için Faz 2 layer-shell
    font_family: str = "Noto Sans"
    font_size: int = 20
    min_font_size: int = 10
    bg_alpha: float = 0.80
    bg_color: str = "#0b0d12"
    text_color: str = "#ffffff"
    show_original: bool = False
    padding: int = 6
    ttl_ms: int = 2500
    click_through: bool = True
    debug: bool = False
    max_width_scale: float = 1.25


@dataclass
class CacheConfig:
    path: str = "~/.cache/gameframe/translations.sqlite3"
    memory_size: int = 512


@dataclass
class RuntimeConfig:
    log_dir: str = "~/.local/state/gameframe"
    metrics_jsonl: bool = True
    debug: bool = False
    stats_interval_s: float = 1.0


@dataclass
class AppConfig:
    capture: CaptureConfig = field(default_factory=CaptureConfig)
    analyzer: AnalyzerConfig = field(default_factory=AnalyzerConfig)
    ocr: OcrConfig = field(default_factory=OcrConfig)
    translate: TranslateConfig = field(default_factory=TranslateConfig)
    overlay: OverlayConfig = field(default_factory=OverlayConfig)
    cache: CacheConfig = field(default_factory=CacheConfig)
    runtime: RuntimeConfig = field(default_factory=RuntimeConfig)
    config_path: str = ""

    def resolved_cache_path(self) -> Path:
        return Path(os.path.expandvars(os.path.expanduser(self.cache.path))).resolve()

    def resolved_log_dir(self) -> Path:
        return Path(os.path.expandvars(os.path.expanduser(self.runtime.log_dir))).resolve()

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _coerce(value: Any, current: Any) -> Any:
    if value is None:
        return current
    if isinstance(current, bool):
        if isinstance(value, str):
            return value.strip().lower() in {"1", "true", "yes", "on"}
        return bool(value)
    if isinstance(current, int) and not isinstance(current, bool):
        return int(value)
    if isinstance(current, float):
        return float(value)
    if isinstance(current, tuple):
        if isinstance(value, str):
            parts = [p.strip() for p in value.split(",") if p.strip()]
            return tuple(int(float(p)) for p in parts)
        if isinstance(value, (list, tuple)):
            return tuple(int(v) for v in value)
    return value


def _apply(obj: Any, data: dict[str, Any], prefix: str) -> None:
    valid = {f.name: f for f in fields(obj)}
    for key, value in (data or {}).items():
        key = str(key)
        if key not in valid:
            log.warning("Bilinmeyen ayar: %s.%s (yok sayıldı)", prefix, key)
            continue
        current = getattr(obj, key)
        if hasattr(current, "__dataclass_fields__") and isinstance(value, dict):
            _apply(current, value, f"{prefix}.{key}")
            continue
        if key == "region" and isinstance(value, list):
            value = tuple(int(v) for v in value)
        try:
            setattr(obj, key, _coerce(value, current))
        except (TypeError, ValueError) as exc:
            log.warning("Ayar dönüştürülemedi: %s.%s=%r (%s)", prefix, key, value, exc)


def load_config(path: Optional[str | Path]) -> AppConfig:
    cfg = AppConfig()
    if path:
        p = Path(path).expanduser()
        if not p.exists():
            raise FileNotFoundError(f"Ayar dosyası bulunamadı: {p}")
        with p.open("r", encoding="utf-8") as fh:
            data = yaml.safe_load(fh) or {}
        if not isinstance(data, dict):
            raise ValueError("Ayar dosyasının kökü bir sözlük olmalı")
        for section in ("capture", "analyzer", "ocr", "translate", "overlay", "cache", "runtime"):
            if section in data:
                _apply(getattr(cfg, section), data[section] or {}, section)
        cfg.config_path = str(p)
    return cfg


def resolve_backends(cfg: AppConfig) -> None:
    """auto seçimlerini somut backend adlarına çevirir."""
    if cfg.capture.backend == "auto":
        cfg.capture.backend = (
            "xcomposite" if (cfg.capture.window_title or cfg.capture.window_class or cfg.capture.window_id) else "mss"
        )
    if cfg.overlay.backend == "auto":
        cfg.overlay.backend = "gtk"
    if cfg.ocr.backend == "auto":
        cfg.ocr.backend = "tesseract"
