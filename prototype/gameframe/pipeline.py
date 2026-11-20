"""Pipeline: yakalama -> fark -> OCR -> çeviri -> overlay.

Thread modeli:
  capture thread  : kaynaktan kare alır, LatestFrameSlot'a yazar (eskisini ezer)
  analyzer thread : son kareyi alır; fark/OCR/çeviri yapar; OverlayModel yayınlar
  overlay thread  : (GTK/Tk) modeli poll eder ve çizer

OCR/çeviri asla yakalama thread'inde çalışmaz; oyun render döngüsü bloke
edilmez. Analyzer yetişemezse eski kareler atılır (drop-late policy).
"""
from __future__ import annotations

import logging
import threading
import time
from typing import Optional

from .analyzer import FrameDiffer, RegionTracker
from .capture import FrameSource
from .config import AppConfig
from .metrics import Metrics
from .ocr import OcrEngine, OcrError
from .translate import TranslatorChain
from .types import DiffResult, Frame, OverlayModel, Rect

log = logging.getLogger(__name__)


class LatestFrameSlot:
    def __init__(self):
        self._cond = threading.Condition()
        self._frame: Optional[Frame] = None
        self._version = 0
        self._closed = False
        self.overwritten = 0

    def publish(self, frame: Frame) -> None:
        with self._cond:
            if self._closed:
                return
            if self._frame is not None:
                self.overwritten += 1
            self._frame = frame
            self._version += 1
            self._cond.notify_all()

    def take_new(self, last_version: int, timeout: float = 0.2) -> tuple[Optional[Frame], int]:
        with self._cond:
            if self._version <= last_version and not self._closed:
                self._cond.wait(timeout)
            if self._version <= last_version:
                return None, last_version
            frame = self._frame
            # Tüketilen kare slot'ta kalırsa her yeni kare "drop" sayılır.
            # Analyzer'ın elindeki kareyi temizle; drop metriği gerçekten
            # yetişilemeyen kareleri göstersin.
            self._frame = None
            return frame, self._version

    def close(self) -> None:
        with self._cond:
            self._closed = True
            self._cond.notify_all()


class LatestModelSlot:
    def __init__(self):
        self._lock = threading.Lock()
        self._model: Optional[OverlayModel] = None
        self.version = 0

    def publish(self, model: OverlayModel) -> None:
        with self._lock:
            self._model = model
            self.version += 1

    def get(self) -> tuple[Optional[OverlayModel], int]:
        with self._lock:
            return self._model, self.version


class RateLimiter:
    """Token bucket; OCR'ı saniyede max N çağrıya sınırlar."""

    def __init__(self, rate: float, burst: int = 2):
        self.rate = max(0.1, float(rate))
        self.capacity = max(1, int(burst))
        self.tokens = float(self.capacity)
        self.last = time.monotonic()

    def allow(self) -> bool:
        now = time.monotonic()
        self.tokens = min(self.capacity, self.tokens + (now - self.last) * self.rate)
        self.last = now
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False


class Pipeline:
    def __init__(
        self,
        cfg: AppConfig,
        source: FrameSource,
        ocr: OcrEngine,
        translator: TranslatorChain,
        metrics: Metrics,
        models: LatestModelSlot,
    ):
        self.cfg = cfg
        self.source = source
        self.ocr = ocr
        self.translator = translator
        self.metrics = metrics
        self.models = models
        self.differ = FrameDiffer(cfg.analyzer)
        self.tracker = RegionTracker(cfg.analyzer, cfg.translate)
        self.frames = LatestFrameSlot()
        self.limiter = RateLimiter(cfg.ocr.max_calls_per_sec, cfg.ocr.burst)
        self._stop = threading.Event()
        self._analyzer: Optional[threading.Thread] = None
        self._last_discovery_ns = 0
        self._last_analysis_ns = 0
        self._last_publish_sig: tuple = ()
        self._last_publish_ns = 0
        self._last_latency_ms: Optional[float] = None
        self._last_stats_ns = 0
        self._ocr_errors = 0
        self._overwritten_seen = 0

    # ------------------------------------------------------------------
    def start(self) -> None:
        self._stop.clear()
        self.source.start(self._on_frame)
        self._analyzer = threading.Thread(target=self._analyze_loop, name="analyzer", daemon=True)
        self._analyzer.start()
        log.info("Pipeline başladı (capture=%s, ocr=%s)", self.source.name, self.ocr.name)

    def stop(self) -> None:
        self._stop.set()
        self.frames.close()
        try:
            self.source.stop()
        except Exception:
            pass
        if self._analyzer:
            self._analyzer.join(timeout=5.0)
            self._analyzer = None
        log.info("Pipeline durdu")

    # ------------------------------------------------------------------
    def _on_frame(self, frame: Frame) -> None:
        self.metrics.inc("frames_captured")
        self.frames.publish(frame)

    def _analyze_loop(self) -> None:
        last_version = 0
        while not self._stop.is_set():
            frame, version = self.frames.take_new(last_version, timeout=0.2)
            if frame is None:
                if self._stop.is_set():
                    break
                # 2 sn kare gelmezse overlay'i temizle (oyun kapandı).
                if time.monotonic_ns() - getattr(self, "_last_frame_ns", 0) > 2_000_000_000 and self.models.get()[0] is not None:
                    self.models.publish(OverlayModel(items=[], updated_at_ns=time.monotonic_ns(), debug="no frames"))
                    self.metrics.inc("overlay_updates")
                continue
            last_version = version
            try:
                self._process(frame)
            except Exception as exc:
                self.metrics.inc("errors")
                log.exception("Analiz hatası: %s", exc)
                self._stop.wait(0.05)
        self.metrics.event("analyzer_stopped")

    # ------------------------------------------------------------------
    def _process(self, frame: Frame) -> None:
        now = time.monotonic_ns()
        self._last_frame_ns = now
        max_fps = float(getattr(self.cfg.analyzer, "max_analysis_fps", 0.0) or 0.0)
        if max_fps > 0:
            min_interval_ns = int(1_000_000_000.0 / max_fps)
            if now - self._last_analysis_ns < min_interval_ns:
                return
            self._last_analysis_ns = now
        t0 = time.perf_counter()
        diff = self.differ.compute(frame)
        dirty = self.tracker.observe(frame, self.differ, diff, now)
        self.metrics.inc("frames_analyzed")
        self.metrics.timing("diff_ms", (time.perf_counter() - t0) * 1000)

        dropped = self.frames.overwritten - self._overwritten_seen
        if dropped > 0:
            self.metrics.inc("frames_dropped", dropped)
            self._overwritten_seen = self.frames.overwritten

        did_ocr = False
        discovery_interval_ns = int(self.cfg.analyzer.discovery_ms) * 1_000_000
        heartbeat_ns = int(self.cfg.analyzer.heartbeat_ms) * 1_000_000
        due = now - self._last_discovery_ns >= discovery_interval_ns

        # Büyük sahne değişimlerinde bölge-bölge OCR yavaş kalır; keşif OCR
        # tüm metinleri tek seferde günceller.
        big_change = diff.changed_ratio >= 0.08 or len(dirty) >= 6

        if len(self.tracker) == 0:
            did_ocr = self._discover(frame, now)
        elif big_change and due:
            did_ocr = self._discover(frame, now)
        elif dirty:
            did_ocr = self._ocr_dirty(frame, dirty, now)
            # Takip edilmeyen değişen piksel varsa veya çok bölge kirliyse keşif yap.
            if due and (len(dirty) >= 3 or diff.changed_ratio >= 0.04):
                did_ocr = self._discover(frame, now) or did_ocr
            elif diff.changed_bbox is not None:
                covered = any(r.rect.iou(diff.changed_bbox) > 0.1 for r in self.tracker.regions)
                if not covered and self.limiter.allow():
                    did_ocr = self._discover(frame, now) or did_ocr
        elif due and diff.changed_ratio >= self.cfg.analyzer.change_trigger:
            did_ocr = self._discover(frame, now)
        elif now - self._last_discovery_ns >= heartbeat_ns:
            # Statik ekranda kaçan metinleri bulmak için periyodik tam keşif.
            # Çok bölge varsa limiter'ı zorlamamak için daha seyrek çalış.
            if len(self.tracker) < 20 or diff.changed_ratio > 0.0005:
                did_ocr = self._discover(frame, now)
            elif now - self._last_discovery_ns >= heartbeat_ns * 3:
                did_ocr = self._discover(frame, now)

        # OCR sırasında yeni metinler oluştu; çeviri kuyruğunu boşalt.
        translated = self._translate_pending(now)
        self.tracker.expire(now)
        self._publish(now, diff, force=did_ocr or translated)
        self.metrics.timing("frame_ms", (time.perf_counter() - t0) * 1000)

        if now - self._last_stats_ns >= 1_000_000_000:
            self._last_stats_ns = now
            regs, trans, stale = self.tracker.debug_counts()
            self.metrics.event(
                "pipeline",
                diff_ratio=round(diff.changed_ratio, 5),
                regions=regs,
                translated=trans,
                stale=stale,
                dirty=len(dirty),
                ocr_calls=self.metrics.counters.get("ocr_calls", 0),
                translate_calls=self.metrics.counters.get("translate_calls", 0),
                capture_fps=round(self.source.fps, 1),
            )

    # ------------------------------------------------------------------
    def _ocr_full(self, frame: Frame, now: int) -> bool:
        t0 = time.perf_counter()
        try:
            lines = self.ocr.recognize(frame, None, self.cfg.ocr.psm)
        except OcrError as exc:
            self._ocr_errors += 1
            self.metrics.inc("ocr_errors")
            if self._ocr_errors <= 3 or self._ocr_errors % 10 == 0:
                log.error("OCR hatası: %s", exc)
            return False
        ms = (time.perf_counter() - t0) * 1000
        self.metrics.inc("ocr_calls")
        self.metrics.inc("full_ocr_calls")
        self.metrics.timing("ocr_ms", ms)
        self.tracker.ingest_full(frame, lines, now)
        log.debug("Keşif OCR: %d satır, %.1f ms", len(lines), ms)
        return True

    def _discover(self, frame: Frame, now: int) -> bool:
        if not self.limiter.allow():
            return False
        self._last_discovery_ns = now
        return self._ocr_full(frame, now)

    def _ocr_dirty(self, frame: Frame, dirty: list, now: int) -> bool:
        did = False
        max_n = max(1, int(self.cfg.analyzer.max_region_ocr_per_cycle))
        for region in dirty[:max_n]:
            if not self.limiter.allow():
                break
            # Metin değişince yeni metin eskisinden uzun olabilir. Sola az,
            # sağa geniş pay ver; aksi halde diyalog kutusunun kenarlığı OCR'a
            # girip "| Inventory" gibi gürültü üretiyor.
            pad_y = max(6, min(12, int(region.rect.h * 0.28)))
            pad_left = max(6, min(14, int(region.rect.w * 0.08)))
            pad_right = max(48, int(region.rect.w * 0.90))
            roi = Rect(
                region.rect.x - pad_left,
                region.rect.y - pad_y,
                region.rect.w + pad_left + pad_right,
                region.rect.h + 2 * pad_y,
            )
            t0 = time.perf_counter()
            try:
                lines = self.ocr.recognize(
                    frame, roi, self.cfg.ocr.region_psm, self.cfg.ocr.region_upscale
                )
            except OcrError as exc:
                self._ocr_errors += 1
                self.metrics.inc("ocr_errors")
                if self._ocr_errors <= 3 or self._ocr_errors % 10 == 0:
                    log.error("Bölge OCR hatası: %s", exc)
                continue
            ms = (time.perf_counter() - t0) * 1000
            self.metrics.inc("ocr_calls")
            self.metrics.inc("region_ocr_calls")
            self.metrics.timing("ocr_ms", ms)
            self.tracker.ingest_region(frame, region.id, lines, now)
            did = True
            log.debug("Bölge OCR [%d]: %d satır, %.1f ms", region.id, len(lines), ms)
        return did

    # ------------------------------------------------------------------
    def _translate_pending(self, now: int) -> bool:
        pending = self.tracker.pending_translations(
            now, max(1, int(self.cfg.translate.max_translations_per_cycle))
        )
        changed = False
        for region in pending:
            if self._stop.is_set():
                break
            revision = region.revision
            self.tracker.mark_translate_attempt(region.id, now)
            t0 = time.perf_counter()
            result = self.translator.translate(region.text)
            ms = (time.perf_counter() - t0) * 1000
            self.metrics.inc("translate_calls")
            self.metrics.timing("translate_ms", ms)
            if result.cached:
                self.metrics.inc("cache_hits")
            if not result.text:
                continue
            ok = self.tracker.set_translation(
                region.id, result.text, result.provider, time.monotonic_ns(), expected_revision=revision
            )
            if ok:
                updated = self.tracker.get(region.id)
                latency = updated.latency_ms if updated else 0.0
                self._last_latency_ms = latency
                self.metrics.latency(
                    latency,
                    region_id=region.id,
                    source=region.text,
                    translated=result.text,
                    provider=result.provider,
                    cached=result.cached,
                )
                self.metrics.event(
                    "translation",
                    region_id=region.id,
                    source=region.text,
                    translated=result.text,
                    provider=result.provider,
                    cached=result.cached,
                    elapsed_ms=round(ms, 2),
                    latency_ms=round(latency, 2),
                )
                changed = True
                log.info(
                    "Çeviri [%d] %r -> %r (%s, %.0f ms, toplam %.0f ms)",
                    region.id,
                    region.text,
                    result.text,
                    result.provider,
                    ms,
                    latency,
                )
        return changed

    # ------------------------------------------------------------------
    def _publish(self, now: int, diff: DiffResult, force: bool = False) -> None:
        items = self.tracker.overlay_items(now, self.cfg.overlay.ttl_ms)
        regs, trans, stale = self.tracker.debug_counts()
        debug = (
            f"regions={regs} translated={trans} stale={stale} "
            f"diff={diff.changed_ratio * 100:.2f}% ocr={self.metrics.counters.get('ocr_calls', 0)} "
            f"cache={self.metrics.counters.get('cache_hits', 0)}"
        )
        model = OverlayModel(items=items, updated_at_ns=now, debug=debug)
        sig = model.signature()
        if not force and sig == self._last_publish_sig and now - self._last_publish_ns < 250_000_000:
            return
        self._last_publish_sig = sig
        self._last_publish_ns = now
        self.models.publish(model)
        if sig or force:
            self.metrics.overlay(model, self._last_latency_ms)
