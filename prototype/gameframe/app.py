"""Uygulama giriş noktası: CLI, bileşenlerin kurulumu, yaşam döngüsü."""
from __future__ import annotations

import argparse
import json
import logging
import os
import signal
import sys
import threading
import time
from dataclasses import asdict
from pathlib import Path
from typing import Optional

import yaml

from . import __version__
from .cache import TranslationCache
from .capture import CaptureError, create_source
from .config import AppConfig, load_config, resolve_backends
from .metrics import Metrics
from .ocr import OcrError, create_ocr
from .overlay import OverlayError, create_overlay
from .pipeline import LatestModelSlot, Pipeline
from .translate import create_translator
from .types import Frame, normalize_text

log = logging.getLogger(__name__)


def setup_logging(cfg: AppConfig) -> None:
    level = logging.DEBUG if cfg.runtime.debug else logging.INFO
    fmt = logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s", "%H:%M:%S")
    root = logging.getLogger()
    root.setLevel(level)
    root.handlers.clear()
    console = logging.StreamHandler(sys.stderr)
    console.setFormatter(fmt)
    root.addHandler(console)
    try:
        log_dir = cfg.resolved_log_dir()
        log_dir.mkdir(parents=True, exist_ok=True)
        fh = logging.FileHandler(log_dir / "gft.log", encoding="utf-8")
        fh.setFormatter(fmt)
        root.addHandler(fh)
    except Exception as exc:
        log.warning("Log dosyası açılamadı: %s", exc)


def parse_region(value: str) -> tuple[int, int, int, int]:
    parts = [p.strip() for p in value.replace("x", ",").split(",") if p.strip()]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("Bölge x,y,w,h biçiminde olmalı")
    return tuple(int(float(p)) for p in parts)  # type: ignore[return-value]


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="gft",
        description="GameFrame Translator - gerçek karelerden değişen metni OCR'layıp Türkçe overlay çizer.",
    )
    p.add_argument("--config", "-c", default="", help="YAML ayar dosyası")
    p.add_argument("--once", action="store_true", help="Tek kare yakala, OCR + çeviri dökümü ver, çık")
    p.add_argument("--no-overlay", action="store_true", help="Overlay açma (headless/benchmark)")
    p.add_argument("--overlay", choices=["auto", "gtk", "tk", "none"], default="", help="Overlay backend")
    p.add_argument("--source", choices=["auto", "mss", "xcomposite", "synthetic"], default="", help="Yakalama backend")
    p.add_argument("--window-title", default="", help="XComposite ile yakalanacak pencere başlığı (alt dize)")
    p.add_argument("--window-class", default="", help="XComposite pencere sınıfı (alt dize)")
    p.add_argument("--window-id", default="", help="X11 pencere id (0x...)")
    p.add_argument("--region", type=parse_region, default=None, help="Yakalama bölgesi x,y,w,h (mss)")
    p.add_argument("--monitor", type=int, default=0, help="Monitör (1 tabanlı; 0=ayardan)")
    p.add_argument("--fps", type=int, default=0, help="Yakalama FPS hedefi")
    p.add_argument("--lang", default="", help="OCR dili (tesseract -l, örn. eng, deu, jpn)")
    p.add_argument("--provider", choices=["auto", "dictionary", "google", "libre", "none"], default="", help="Çeviri sağlayıcı")
    p.add_argument("--target", default="", help="Hedef dil (varsayılan tr)")
    p.add_argument("--cache", default="", help="SQLite önbellek yolu")
    p.add_argument("--duration", type=float, default=0.0, help="Saniye sonunda otomatik dur (0=sınırsız)")
    p.add_argument("--save-frame", default="", help="--once ile yakalanan kareyi PNG olarak kaydet")
    p.add_argument("--list-monitors", action="store_true", help="Monitörleri listele ve çık")
    p.add_argument("--dump-config", action="store_true", help="Etkin ayarı YAML olarak yaz ve çık")
    p.add_argument("--debug", action="store_true", help="Ayrıntılı log")
    p.add_argument("--version", action="version", version=f"GameFrame Translator {__version__}")
    return p


def apply_overrides(cfg: AppConfig, args: argparse.Namespace) -> None:
    if args.source:
        cfg.capture.backend = args.source
    if args.window_title:
        cfg.capture.window_title = args.window_title
    if args.window_class:
        cfg.capture.window_class = args.window_class
    if args.window_id:
        cfg.capture.window_id = int(args.window_id, 0)
    if args.region:
        cfg.capture.region = args.region
    if args.monitor:
        cfg.capture.monitor = args.monitor
        cfg.overlay.monitor = args.monitor
    if args.fps:
        cfg.capture.fps = args.fps
    if args.lang:
        cfg.ocr.lang = args.lang
    if args.provider:
        cfg.translate.provider = args.provider
    if args.target:
        cfg.translate.target_lang = args.target
    if args.cache:
        cfg.cache.path = args.cache
    if args.overlay:
        cfg.overlay.backend = args.overlay
    if args.no_overlay:
        cfg.overlay.backend = "none"
    if args.debug:
        cfg.runtime.debug = True
        cfg.overlay.debug = True


def list_monitors() -> int:
    try:
        import mss

        with mss.mss() as sct:
            for i, m in enumerate(sct.monitors):
                label = "tümü" if i == 0 else f"monitör {i}"
                print(f"{i}: {label}: {m['width']}x{m['height']}+{m['left']}+{m['top']}")
        return 0
    except Exception as exc:
        print(f"Monitör listesi alınamadı: {exc}", file=sys.stderr)
        return 1


def run_once(cfg: AppConfig, args: argparse.Namespace) -> int:
    source = create_source(cfg)
    ocr = create_ocr(cfg)
    cache = TranslationCache(cfg.resolved_cache_path(), cfg.cache.memory_size)
    translator = create_translator(cfg, cache)
    try:
        frame = source.grab_once()
        if args.save_frame:
            from PIL import Image

            path = Path(args.save_frame).expanduser()
            path.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(frame.image).save(path)
            print(f"Kare kaydedildi: {path}")
        print(f"Kare: {frame.width}x{frame.height} origin={frame.origin_x},{frame.origin_y} source={frame.source}")
        t0 = time.perf_counter()
        lines = ocr.recognize(frame, None, cfg.ocr.psm)
        ocr_ms = (time.perf_counter() - t0) * 1000
        print(f"OCR: {len(lines)} satır, {ocr_ms:.1f} ms, dil={cfg.ocr.lang}")
        for i, line in enumerate(lines):
            tr = translator.translate(line.text)
            mark = "~" if tr.cached else " "
            print(
                f"  [{i:02d}] conf={line.confidence:5.1f} rect={line.rect.x},{line.rect.y},"
                f"{line.rect.w},{line.rect.h} {mark} {line.text!r} -> {tr.text!r} ({tr.provider})"
            )
        return 0
    except (CaptureError, OcrError, OSError) as exc:
        print(f"Hata: {exc}", file=sys.stderr)
        return 1
    finally:
        ocr.close()
        cache.close()


def run_app(cfg: AppConfig, args: argparse.Namespace) -> int:
    if cfg.capture.backend == "mss" and cfg.overlay.backend != "none" and not cfg.capture.allow_root_with_overlay:
        print(
            "Hata: root ekran yakalama + overlay birlikte overlay'in kendi çıktısını yakalar "
            "(OCR geri besleme döngüsü).\n"
            "  Çözüm: --window-title \"Oyun Başlığı\" (XComposite) veya --no-overlay.\n"
            "  Zorlamak için capture.allow_root_with_overlay=true.",
            file=sys.stderr,
        )
        return 2

    source = create_source(cfg)
    ocr = create_ocr(cfg)
    cache = TranslationCache(cfg.resolved_cache_path(), cfg.cache.memory_size)
    translator = create_translator(cfg, cache)
    metrics = Metrics(cfg.resolved_log_dir(), cfg.runtime.metrics_jsonl, cfg.runtime.stats_interval_s)
    models = LatestModelSlot()
    stop = threading.Event()

    def _signal(_signum, _frame):
        stop.set()

    signal.signal(signal.SIGINT, _signal)
    signal.signal(signal.SIGTERM, _signal)
    if args.duration > 0:
        timer = threading.Timer(args.duration, stop.set)
        timer.daemon = True
        timer.start()

    overlay = create_overlay(cfg.overlay, models, stop)
    pipeline = Pipeline(cfg, source, ocr, translator, metrics, models)
    metrics.event(
        "app_start",
        version=__version__,
        capture=source.name,
        overlay=overlay.name,
        ocr=cfg.ocr.backend,
        lang=cfg.ocr.lang,
        translator=cfg.translate.provider,
        target=cfg.translate.target_lang,
        window=cfg.capture.window_title,
        region=list(cfg.capture.region) if cfg.capture.region else None,
    )
    try:
        overlay.init()
    except OverlayError as exc:
        print(f"Overlay başlatılamadı: {exc}", file=sys.stderr)
        metrics.close()
        cache.close()
        return 3
    try:
        pipeline.start()
        log.info(
            "Çalışıyor. Durdurmak için Ctrl+C. Overlay=%s, kaynak=%s, %s->%s",
            overlay.name,
            source.name,
            cfg.translate.source_lang,
            cfg.translate.target_lang,
        )
        overlay.run()
    finally:
        stop.set()
        pipeline.stop()
        overlay.shutdown()
        summary_path = metrics.write_summary()
        summary = metrics.summary()
        metrics.close()
        cache.close()
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        if summary_path:
            print(f"Özet: {summary_path}")
        log.info("Kapatıldı")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        cfg = load_config(args.config or None)
    except (FileNotFoundError, ValueError) as exc:
        print(f"Ayar hatası: {exc}", file=sys.stderr)
        return 2
    apply_overrides(cfg, args)
    resolve_backends(cfg)
    setup_logging(cfg)
    if args.list_monitors:
        return list_monitors()
    if args.dump_config:
        print(yaml.safe_dump(cfg.to_dict(), allow_unicode=True, sort_keys=False))
        return 0
    if args.once:
        return run_once(cfg, args)
    return run_app(cfg, args)
