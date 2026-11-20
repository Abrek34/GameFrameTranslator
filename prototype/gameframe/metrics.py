"""Ölçüm toplama: JSONL olayları, gecikme yüzdelikleri, CPU/RSS örnekleri.

Benchmark için olay zaman damgaları `time.monotonic_ns()` kullanır; aynı
makinedeki sentetik sahne de aynı saati kullandığı için iki süreç arasında
gecikme ölçülebilir.
"""
from __future__ import annotations

import json
import logging
import math
import os
import threading
import time
from pathlib import Path
from typing import Any

from .types import OverlayModel

log = logging.getLogger(__name__)


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    vals = sorted(values)
    k = max(0, min(len(vals) - 1, int(math.ceil((p / 100.0) * len(vals))) - 1))
    return float(vals[k])


class Metrics:
    def __init__(self, log_dir: str | Path, enabled: bool = True, stats_interval_s: float = 1.0):
        self.log_dir = Path(log_dir)
        self.enabled = enabled
        self.stats_interval_s = max(0.25, float(stats_interval_s))
        self.session = time.strftime("%Y%m%d-%H%M%S")
        self.started_ns = time.monotonic_ns()
        self.counters: dict[str, int] = {}
        self.latencies_ms: list[float] = []
        self.timings: dict[str, list[float]] = {}
        self._lock = threading.Lock()
        self._fh = None
        self._stop = threading.Event()
        self._sampler: threading.Thread | None = None
        self.cpu_samples: list[float] = []
        self.rss_samples_mb: list[float] = []
        self._last_cpu: tuple[int, int, float] | None = None
        self._log_path: Path | None = None
        if self.enabled:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            self._log_path = self.log_dir / f"metrics-{self.session}.jsonl"
            self._fh = self._log_path.open("a", encoding="utf-8", buffering=1)
        self._start_sampler()

    # ------------------------------------------------------------------
    def event(self, kind: str, **fields: Any) -> None:
        if not self._fh:
            return
        payload = {
            "t_ns": time.monotonic_ns(),
            "wall": time.time(),
            "kind": kind,
            **fields,
        }
        try:
            line = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
            with self._lock:
                self._fh.write(line + "\n")
        except Exception:
            pass

    def inc(self, name: str, n: int = 1) -> None:
        with self._lock:
            self.counters[name] = self.counters.get(name, 0) + int(n)

    def timing(self, name: str, ms: float) -> None:
        with self._lock:
            self.timings.setdefault(name, []).append(float(ms))

    def latency(self, ms: float, **fields: Any) -> None:
        with self._lock:
            self.latencies_ms.append(float(ms))
        self.event("latency", ms=float(ms), **fields)

    def overlay(self, model: OverlayModel, latency_ms: float | None = None) -> None:
        self.inc("overlay_updates")
        items = [
            {
                "id": i.region_id,
                "rect": [i.rect.x, i.rect.y, i.rect.w, i.rect.h],
                "text": i.text,
                "source": i.source_text,
                "stale": i.stale,
            }
            for i in model.items
        ]
        fields: dict[str, Any] = {"items": items}
        if latency_ms is not None:
            fields["latency_ms"] = latency_ms
        self.event("overlay", **fields)

    # ------------------------------------------------------------------
    def _start_sampler(self) -> None:
        if not self.enabled:
            return
        self._sampler = threading.Thread(target=self._sample_loop, name="metrics-sampler", daemon=True)
        self._sampler.start()

    def _read_proc(self) -> tuple[float, float] | None:
        try:
            stat = Path("/proc/self/stat").read_text(encoding="utf-8").split()
            utime, stime = int(stat[13]), int(stat[14])
            ticks = float(os.sysconf("SC_CLK_TCK"))
            rss_kb = 0
            for line in Path("/proc/self/status").read_text(encoding="utf-8").splitlines():
                if line.startswith("VmRSS:"):
                    rss_kb = int(line.split()[1])
                    break
            return (utime + stime) / ticks, rss_kb / 1024.0
        except Exception:
            return None

    def _sample_loop(self) -> None:
        while not self._stop.wait(self.stats_interval_s):
            data = self._read_proc()
            if data is None:
                continue
            cpu_seconds, rss_mb = data
            now = time.monotonic()
            if self._last_cpu is not None:
                last_cpu, _, last_t = self._last_cpu
                elapsed = max(1e-3, now - last_t)
                pct = max(0.0, (cpu_seconds - last_cpu) / elapsed * 100.0)
                with self._lock:
                    self.cpu_samples.append(pct)
                    self.rss_samples_mb.append(rss_mb)
                self.event("system", cpu_pct=round(pct, 2), rss_mb=round(rss_mb, 2))
            self._last_cpu = (cpu_seconds, rss_mb, now)

    # ------------------------------------------------------------------
    def summary(self) -> dict[str, Any]:
        with self._lock:
            counters = dict(self.counters)
            lat = list(self.latencies_ms)
            timings = {k: list(v) for k, v in self.timings.items()}
            cpu = list(self.cpu_samples)
            rss = list(self.rss_samples_mb)
        out: dict[str, Any] = {
            "session": self.session,
            "duration_s": (time.monotonic_ns() - self.started_ns) / 1e9,
            "counters": counters,
            "latency_ms": {
                "count": len(lat),
                "p50": round(percentile(lat, 50), 2),
                "p95": round(percentile(lat, 95), 2),
                "max": round(max(lat), 2) if lat else 0.0,
            },
            "timings_ms": {
                k: {
                    "count": len(v),
                    "avg": round(sum(v) / len(v), 2),
                    "p95": round(percentile(v, 95), 2),
                }
                for k, v in timings.items()
                if v
            },
            "system": {
                "cpu_avg_pct": round(sum(cpu) / len(cpu), 2) if cpu else 0.0,
                "cpu_max_pct": round(max(cpu), 2) if cpu else 0.0,
                "rss_avg_mb": round(sum(rss) / len(rss), 1) if rss else 0.0,
                "rss_max_mb": round(max(rss), 1) if rss else 0.0,
            },
            "metrics_path": str(self._log_path) if self._log_path else "",
        }
        return out

    def write_summary(self) -> Path | None:
        if not self.enabled:
            return None
        summary = self.summary()
        path = self.log_dir / f"summary-{self.session}.json"
        try:
            path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        except Exception as exc:
            log.warning("Özet yazılamadı: %s", exc)
            return None
        return path

    def close(self) -> None:
        self._stop.set()
        if self._sampler:
            self._sampler.join(timeout=2.0)
            self._sampler = None
        if self._fh:
            try:
                self._fh.flush()
                self._fh.close()
            except Exception:
                pass
            self._fh = None
