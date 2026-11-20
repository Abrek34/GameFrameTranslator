#!/usr/bin/env python3
"""Sentetik sahne + GFT ile uçtan uca benchmark.

Ölçülen: sahnenin metni göstermesi -> GFT'nin overlay modelini yayınlaması.
OCR/çeviri gecikmesi, OCR çağrı sayısı, CPU/RSS özeti raporlanır.

Kullanım:
  DISPLAY=:0 python tools/benchmark.py --duration 30
  DISPLAY=:0 python tools/benchmark.py --duration 30 --with-overlay
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--interval", type=float, default=2.5)
    ap.add_argument("--with-overlay", action="store_true", help="GTK overlay'i de çalıştır")
    ap.add_argument("--keep", action="store_true", help="Geçici dosyaları silme")
    ap.add_argument("--scene-log", default="")
    ap.add_argument("--workdir", default="")
    return ap.parse_args()


def percentile(vals: list[float], p: float) -> float:
    if not vals:
        return 0.0
    s = sorted(vals)
    k = max(0, min(len(s) - 1, int((p / 100.0) * len(s) + 0.999999) - 1))
    return s[k]


def main() -> int:
    args = parse_args()
    work = Path(args.workdir).expanduser() if args.workdir else Path(tempfile.mkdtemp(prefix="gft-bench-"))
    work.mkdir(parents=True, exist_ok=True)
    scene_log = Path(args.scene_log).expanduser() if args.scene_log else work / "scene.jsonl"
    log_dir = work / "logs"
    cache_path = work / "cache.sqlite3"
    config_path = work / "config.yaml"
    scene_log.unlink(missing_ok=True)

    config = f"""
capture:
  backend: xcomposite
  window_title: "GFT Synthetic Scene"
  fps: 30
analyzer:
  discovery_ms: 600
  heartbeat_ms: 2500
ocr:
  backend: tesseract
  lang: eng
  max_calls_per_sec: 5
  burst: 2
translate:
  provider: dictionary
  fallback: original
overlay:
  backend: {"gtk" if args.with_overlay else "none"}
cache:
  path: "{cache_path}"
runtime:
  log_dir: "{log_dir}"
  metrics_jsonl: true
"""
    config_path.write_text(config, encoding="utf-8")
    env = dict(os.environ)
    scene_cmd = [
        PY,
        str(ROOT / "tools" / "synthetic_scene.py"),
        "--duration",
        str(args.duration + 6),
        "--interval",
        str(args.interval),
        "--log",
        str(scene_log),
    ]
    scene = subprocess.Popen(scene_cmd, cwd=str(ROOT), env=env)
    time.sleep(2.0)
    app_cmd = [
        PY,
        "-m",
        "gameframe",
        "--config",
        str(config_path),
        "--source",
        "xcomposite",
        "--window-title",
        "GFT Synthetic Scene",
        "--duration",
        str(args.duration),
    ]
    if not args.with_overlay:
        app_cmd.append("--no-overlay")
    print("Çalıştırılıyor:", " ".join(app_cmd))
    app = subprocess.run(app_cmd, cwd=str(ROOT), env=env, timeout=args.duration + 30, capture_output=True, text=True)
    try:
        scene.wait(timeout=8)
    except subprocess.TimeoutExpired:
        scene.kill()
    if app.returncode != 0:
        print(app.stdout)
        print(app.stderr, file=sys.stderr)
        print("GFT başarısız", file=sys.stderr)
        return app.returncode

    events: list[dict] = []
    metrics_file = max(log_dir.glob("metrics-*.jsonl"), key=lambda p: p.stat().st_mtime)
    for line in metrics_file.read_text(encoding="utf-8").splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    app_start = next((e["t_ns"] for e in events if e.get("kind") == "app_start"), 0)
    scene_events = []
    for line in scene_log.read_text(encoding="utf-8").splitlines():
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue
        if ev.get("kind") == "line_shown" and ev.get("t_ns", 0) >= app_start:
            scene_events.append(ev)

    latencies: list[tuple[float, str]] = []
    for sev in scene_events:
        source = " ".join(str(sev.get("body", "")).split())
        candidates = [
            e
            for e in events
            if e.get("kind") == "overlay"
            and any(i.get("source") == source for i in e.get("items", []))
            and e.get("t_ns", 0) >= sev["t_ns"]
        ]
        if candidates:
            first = min(candidates, key=lambda e: e["t_ns"])
            latencies.append(((first["t_ns"] - sev["t_ns"]) / 1e6, source))
    vals = [x[0] for x in latencies]
    summary_path = max(log_dir.glob("summary-*.json"), key=lambda p: p.stat().st_mtime, default=None)
    summary = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path else {}
    counters = summary.get("counters", {})
    print("\n=== GameFrame Translator benchmark ===")
    print(f"Süre: {args.duration:.0f}s  overlay: {'açık' if args.with_overlay else 'kapalı'}")
    print(f"Sahne olayı: {len(scene_events)}  eşleşen çeviri: {len(latencies)}")
    print(f"Uçtan uca gecikme: p50={percentile(vals, 50):.0f} ms  p95={percentile(vals, 95):.0f} ms  max={max(vals) if vals else 0:.0f} ms")
    print(f"OCR çağrısı: {counters.get('ocr_calls', 0)} (tam={counters.get('full_ocr_calls', 0)}, bölge={counters.get('region_ocr_calls', 0)})")
    print(f"Çeviri: {counters.get('translate_calls', 0)}  cache hit: {counters.get('cache_hits', 0)}")
    print(f"CPU: ort {summary.get('system', {}).get('cpu_avg_pct', 0):.1f}%  max {summary.get('system', {}).get('cpu_max_pct', 0):.1f}%")
    print(f"RSS: ort {summary.get('system', {}).get('rss_avg_mb', 0):.0f} MB  max {summary.get('system', {}).get('rss_max_mb', 0):.0f} MB")
    print(f"Özet: {summary_path}")
    print(f"Çalışma dizini: {work}")
    for ms, text in latencies:
        print(f"  {ms:7.0f} ms  {text}")
    if not args.keep:
        print("(Geçici dosyalar silinmedi; --keep olmadan da rapor için tutuluyor.)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
