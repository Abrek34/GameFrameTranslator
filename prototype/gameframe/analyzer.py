"""Kare farkı, bölge parmak izi ve metin bölgesi yaşam döngüsü.

Temel kural: her karede OCR yok. Önce bölgelerin küçük resim/dHash farkına
bakılır; yalnızca değişen bölge OCR kuyruğuna girer. Yeni metin kutuları,
global fark eşiği aşıldığında seyrek aralıklarla yapılan "keşif" OCR'ı ile
bulunur.
"""
from __future__ import annotations

import logging
import math
import time
import zlib
from typing import Iterable, Optional

import numpy as np
from PIL import Image

from .config import AnalyzerConfig, TranslateConfig
from .types import (
    Cell,
    DiffResult,
    Frame,
    OcrLine,
    OverlayItem,
    Rect,
    RegionSignature,
    TextRegion,
    has_letters,
    normalize_text,
)

log = logging.getLogger(__name__)


def _to_gray(rgb: np.ndarray) -> np.ndarray:
    # Pillow'un C yolu, numpy ağırlıklı toplamdan ~4x hızlı.
    return np.asarray(Image.fromarray(rgb).convert("L"), dtype=np.uint8)


def _resize_gray(gray: np.ndarray, width: int) -> np.ndarray:
    h, w = gray.shape[:2]
    if width <= 0 or w <= 0:
        return gray
    if w <= width:
        return gray
    # Hızlı yol: 2x/3x kutu ortalaması (PIL reduce C'de çalışır).
    factor = max(1, w // max(1, width))
    if factor >= 2 and h >= factor and w >= factor:
        return np.asarray(Image.fromarray(gray).reduce(factor), dtype=np.uint8)
    height = max(1, int(round(h * width / float(w))))
    return np.asarray(Image.fromarray(gray).resize((width, height), Image.BILINEAR), dtype=np.uint8)


def signature_from_rgb(rgb: np.ndarray) -> RegionSignature:
    gray = _to_gray(rgb)
    thumb = np.asarray(Image.fromarray(gray).resize((16, 16), Image.BILINEAR), dtype=np.uint8)
    small = np.asarray(Image.fromarray(gray).resize((9, 8), Image.BILINEAR), dtype=np.uint8)
    bits = (small[:, 1:] > small[:, :-1]).flatten()
    dhash = int.from_bytes(np.packbits(bits).tobytes(), "big")
    return RegionSignature(dhash=dhash, thumb=thumb)


class FrameDiffer:
    def __init__(self, cfg: AnalyzerConfig):
        self.cfg = cfg
        self._prev: Optional[np.ndarray] = None
        self._prev_shape: tuple[int, int] = (0, 0)
        self._scale_x = 1.0
        self._scale_y = 1.0

    def compute(self, frame: Frame) -> DiffResult:
        gray = _to_gray(frame.image)
        small = _resize_gray(gray, max(160, int(self.cfg.analysis_width)))
        h, w = small.shape
        if self._prev is None or self._prev.shape != small.shape:
            self._prev = small
            self._scale_x = frame.width / float(w)
            self._scale_y = frame.height / float(h)
            return DiffResult(
                changed_ratio=1.0,
                changed_bbox=Rect(frame.origin_x, frame.origin_y, frame.width, frame.height),
                frame_hash=zlib.crc32(small.tobytes()),
                first_frame=True,
            )

        delta = np.abs(small.astype(np.int16) - self._prev.astype(np.int16))
        mask = delta > int(self.cfg.pixel_threshold)
        changed = int(mask.sum())
        total = int(mask.size)
        ratio = changed / float(total) if total else 0.0
        bbox = None
        cells: list[Cell] = []
        if changed:
            rows = np.flatnonzero(mask.any(axis=1))
            cols = np.flatnonzero(mask.any(axis=0))
            if rows.size and cols.size:
                x0, x1 = int(cols[0]), int(cols[-1]) + 1
                y0, y1 = int(rows[0]), int(rows[-1]) + 1
                bbox = Rect(
                    frame.origin_x + int(x0 * self._scale_x),
                    frame.origin_y + int(y0 * self._scale_y),
                    max(1, int((x1 - x0) * self._scale_x)),
                    max(1, int((y1 - y0) * self._scale_y)),
                )
            cells = self._changed_cells(mask, frame)
        self._prev = small
        self._scale_x = frame.width / float(w)
        self._scale_y = frame.height / float(h)
        return DiffResult(
            changed_ratio=ratio,
            changed_bbox=bbox,
            cells=cells,
            frame_hash=zlib.crc32(small.tobytes()),
        )

    def _changed_cells(self, mask: np.ndarray, frame: Frame) -> list[Cell]:
        h, w = mask.shape
        g = max(4, int(self.cfg.grid))
        gh, gw = math.ceil(h / g), math.ceil(w / g)
        pad = np.zeros((gh * g, gw * g), dtype=np.uint8)
        pad[:h, :w] = mask
        sums = pad.reshape(gh, g, gw, g).sum(axis=(1, 3))
        cell_area = float(g * g)
        out: list[Cell] = []
        for gy in range(gh):
            for gx in range(gw):
                r = float(sums[gy, gx]) / cell_area
                if r >= 0.08:
                    out.append(
                        Cell(
                            rect=Rect(
                                frame.origin_x + int(gx * g * self._scale_x),
                                frame.origin_y + int(gy * g * self._scale_y),
                                max(1, int(g * self._scale_x)),
                                max(1, int(g * self._scale_y)),
                            ),
                            changed_ratio=r,
                        )
                    )
        out.sort(key=lambda c: c.changed_ratio, reverse=True)
        return out[:200]

    def signature(self, frame: Frame, rect: Rect) -> RegionSignature:
        crop = frame.crop(rect)
        if crop.size == 0:
            return RegionSignature(0, np.zeros((16, 16), dtype=np.uint8))
        return signature_from_rgb(crop)


class RegionTracker:
    def __init__(self, cfg: AnalyzerConfig, tr_cfg: TranslateConfig):
        self.cfg = cfg
        self.tr_cfg = tr_cfg
        self._regions: dict[int, TextRegion] = {}
        self._next_id = 1

    # ------------------------------------------------------------------
    @property
    def regions(self) -> list[TextRegion]:
        return list(self._regions.values())

    def __len__(self) -> int:
        return len(self._regions)

    def get(self, region_id: int) -> Optional[TextRegion]:
        return self._regions.get(region_id)

    def remove(self, region_id: int) -> None:
        self._regions.pop(region_id, None)

    # ------------------------------------------------------------------
    def observe(self, frame: Frame, differ: FrameDiffer, diff, now_ns: int) -> list[TextRegion]:
        """Her karede bölgelerin piksel imzasını günceller; dirty listesi döner."""
        dirty: list[TextRegion] = []
        remove_ids: list[int] = []
        for region in self._regions.values():
            local = Rect(
                region.rect.x - frame.origin_x,
                region.rect.y - frame.origin_y,
                region.rect.w,
                region.rect.h,
            ).clipped(frame.width, frame.height)
            if local.empty:
                region.misses += 1
                if region.misses >= 4:
                    remove_ids.append(region.id)
                continue
            sig = differ.signature(frame, local)
            old = region.signature
            region.signature = sig
            region.last_present_ns = now_ns
            # Kare farkı bölgeyle kesişiyorsa metin değişmiş olabilir. Küçük
            # resim farkı eşiği tek başına ince harf değişimlerini kaçırabilir;
            # global değişim kutusu burada ikinci bir tetikleyicidir.
            pixel_hit = bool(diff.changed_bbox and diff.changed_bbox.intersects(region.rect))
            sig_hit = old is not None and sig.delta(old) > self.cfg.region_change
            if pixel_hit or sig_hit:
                region.dirty_frames += 1
                if region.dirty_frames >= max(1, int(getattr(self.cfg, "min_dirty_frames", 1))):
                    if not region.dirty:
                        region.dirty = True
                        region.last_changed_ns = now_ns
                        # İçerik değişti: eski çeviriyi ekranda tutma.
                        region.translation = ""
                        region.translation_provider = ""
                        region.stale = True
                        region.last_translate_attempt_ns = 0
                    dirty.append(region)
            else:
                region.dirty_frames = 0
        for rid in remove_ids:
            self._regions.pop(rid, None)
        self.expire(now_ns)
        # Rate limit nedeniyle önceki karede OCR'lanamayan dirty bölgeler de
        # kuyrukta kalır; yalnızca "bu karede değişti" listesini döndürmeyiz.
        dirty = [r for r in self._regions.values() if r.dirty]
        dirty.sort(key=lambda r: r.rect.area, reverse=True)
        return dirty

    def expire(self, now_ns: int) -> None:
        ttl = int(self.cfg.expire_ms) * 1_000_000
        dead = [
            rid
            for rid, r in self._regions.items()
            if now_ns - r.last_present_ns > ttl or r.misses >= 5
        ]
        for rid in dead:
            log.debug("Bölge %d süresi doldu", rid)
            self._regions.pop(rid, None)

    # ------------------------------------------------------------------
    def _match(self, line: OcrLine, used: set[int]) -> Optional[TextRegion]:
        best = None
        best_score = 0.0
        lc = line.rect.center()
        for r in self._regions.values():
            if r.id in used:
                continue
            score = r.rect.iou(line.rect)
            if score < 0.20:
                # OCR kutusu bazen bölünür/birleşir: merkez yakınlığına da bak.
                rc = r.rect.center()
                dist = math.hypot(lc[0] - rc[0], lc[1] - rc[1])
                near = max(12.0, 0.6 * min(r.rect.w, r.rect.h))
                if dist <= near:
                    score = 0.15 * (1.0 - min(1.0, dist / near))
            if score > best_score:
                best_score = score
                best = r
        return best if best_score >= 0.15 else None

    @staticmethod
    def _signature_for_screen_rect(frame: Frame, rect: Rect) -> RegionSignature:
        local = Rect(
            rect.x - frame.origin_x, rect.y - frame.origin_y, rect.w, rect.h
        ).clipped(frame.width, frame.height)
        if local.empty:
            return RegionSignature(0, np.zeros((16, 16), dtype=np.uint8))
        return signature_from_rgb(frame.crop(local))

    @staticmethod
    def _merge_ocr_lines(lines: list[OcrLine]) -> list[OcrLine]:
        """Yakın OCR satırlarını birleştirerek daha doğru kutu/konum üret."""
        items = [l for l in lines if normalize_text(l.text)]
        if not items:
            return []
        items.sort(key=lambda l: (l.rect.y, l.rect.x))
        merged: list[OcrLine] = []
        used = [False] * len(items)
        for i, base in enumerate(items):
            if used[i]:
                continue
            used[i] = True
            group = [base]
            rect = base.rect
            confs = [base.confidence]
            changed = True
            while changed:
                changed = False
                for j, other in enumerate(items):
                    if used[j]:
                        continue
                    # Aynı satırda yatay yakın mı?
                    cy1 = rect.center()[1]
                    cy2 = other.rect.center()[1]
                    same_line = abs(cy1 - cy2) <= max(6.0, 0.6 * min(rect.h, other.rect.h))
                    h_gap = min(abs(other.rect.x - rect.right), abs(rect.x - other.rect.right))
                    close_x = h_gap <= max(24.0, 1.2 * max(rect.h, other.rect.h))
                    overlap_x = not (other.rect.right < rect.x - 4 or rect.right < other.rect.x - 4)
                    # Alt satır (çok satırlı blok) mı?
                    v_gap = other.rect.y - rect.bottom
                    close_y = 0 <= v_gap <= max(8.0, 0.8 * rect.h)
                    x_align = abs(other.rect.x - rect.x) <= max(20.0, 0.35 * max(rect.w, other.rect.w))
                    if (same_line and (close_x or overlap_x)) or (close_y and x_align):
                        used[j] = True
                        group.append(other)
                        rect = rect.union(other.rect)
                        confs.append(other.confidence)
                        changed = True
            group.sort(key=lambda l: (l.rect.y, l.rect.x))
            # Satır satır birleştir
            lines_by_y: list[list[OcrLine]] = []
            for g in group:
                placed = False
                for bucket in lines_by_y:
                    if abs(bucket[0].rect.center()[1] - g.rect.center()[1]) <= max(6.0, 0.6 * bucket[0].rect.h):
                        bucket.append(g)
                        placed = True
                        break
                if not placed:
                    lines_by_y.append([g])
            text_lines = []
            for bucket in sorted(lines_by_y, key=lambda b: b[0].rect.y):
                bucket.sort(key=lambda l: l.rect.x)
                text_lines.append(" ".join(normalize_text(x.text) for x in bucket if normalize_text(x.text)))
            text = normalize_text("\n".join(t for t in text_lines if t))
            if not text:
                continue
            merged.append(OcrLine(rect=rect, text=text, confidence=float(sum(confs) / len(confs)) if confs else 0.0))
        merged.sort(key=lambda l: (l.rect.y, l.rect.x))
        return merged

    @staticmethod
    def _smooth_rect(old: Rect, new: Rect) -> Rect:
        # Jitter azalt: kutular çok yakınsa eski konumu koru, küçük farkta yumuşat.
        if old.iou(new) > 0.85:
            return old
        ox, oy = old.center()
        nx, ny = new.center()
        if abs(ox - nx) <= 2 and abs(oy - ny) <= 2 and abs(old.w - new.w) <= 4 and abs(old.h - new.h) <= 4:
            return old
        x = int(round(old.x * 0.35 + new.x * 0.65))
        y = int(round(old.y * 0.35 + new.y * 0.65))
        w = int(round(old.w * 0.35 + new.w * 0.65))
        h = int(round(old.h * 0.35 + new.h * 0.65))
        return Rect(x, y, max(1, w), max(1, h))

    def ingest_full(self, frame: Frame, lines: Iterable[OcrLine], now_ns: int) -> None:
        """Tam kare OCR sonucunu mevcut bölgelerle eşleştirir, yenileri ekler."""
        merged_lines = self._merge_ocr_lines(list(lines))
        ordered = sorted(
            [l for l in merged_lines if normalize_text(l.text)],
            key=lambda l: l.rect.area,
            reverse=True,
        )
        used: set[int] = set()
        for line in ordered:
            text = normalize_text(line.text)
            if not text:
                continue
            match = self._match(line, used)
            if match is not None:
                used.add(match.id)
                smooth_rect = self._smooth_rect(match.rect, line.rect) if match.text == text else line.rect
                match.rect = smooth_rect
                match.last_ocr_ns = now_ns
                match.last_present_ns = now_ns
                match.misses = 0
                match.dirty = False
                match.dirty_frames = 0
                match.signature = self._signature_for_screen_rect(frame, smooth_rect)
                if text != match.text:
                    log.info("Metin değişti [%d]: %r -> %r", match.id, match.text, text)
                    match.text = text
                    match.revision += 1
                    # Eski çeviri artık geçersiz; overlay'de üst üste binmeyi önle.
                    match.translation = ""
                    match.translation_provider = ""
                    match.stale = True
                    match.last_changed_ns = now_ns
                    match.last_translate_attempt_ns = 0
                elif match.translation:
                    match.stale = False
                continue
            if len(self._regions) >= self.cfg.max_regions:
                continue
            rid = self._next_id
            self._next_id += 1
            region = TextRegion(
                id=rid,
                rect=line.rect,
                text=text,
                stale=True,
                dirty=False,
                first_seen_ns=now_ns,
                last_changed_ns=now_ns,
                last_present_ns=now_ns,
                last_ocr_ns=now_ns,
                ocr_source="full",
            )
            region.signature = self._signature_for_screen_rect(frame, line.rect)
            self._regions[rid] = region
            used.add(rid)
            log.info("Yeni metin bölgesi [%d] %r @ %s", rid, text, line.rect)

    def ingest_region(self, frame: Frame, region_id: int, lines: Iterable[OcrLine], now_ns: int) -> None:
        region = self._regions.get(region_id)
        if region is None:
            return
        lines = [l for l in lines if normalize_text(l.text)]
        if len(lines) > 1:
            # Genişletilmiş ROI komşu satırı da kapsayabilir; dikey merkezi
            # bölgeye en yakın satırları tut.
            cy = region.rect.center()[1]
            keep = [
                l
                for l in lines
                if abs(l.rect.center()[1] - cy) <= max(region.rect.h * 0.75, 12.0)
            ]
            if keep:
                lines = keep
        if not lines:
            # Bölge dirty iken OCR metin bulamadıysa içerik kaybolmuş veya
            # değişmiş demektir; eski çeviriyi ekranda bırakma.
            log.info("Bölge [%d] metni bulunamadı, kaldırılıyor", region_id)
            self._regions.pop(region_id, None)
            return
        merged = " ".join(normalize_text(l.text) for l in lines)
        merged = normalize_text(merged)
        if not merged:
            log.info("Bölge [%d] boş metin, kaldırılıyor", region_id)
            self._regions.pop(region_id, None)
            return
        rect = lines[0].rect
        for line in lines[1:]:
            rect = rect.union(line.rect)
        region.rect = rect
        region.last_ocr_ns = now_ns
        region.last_present_ns = now_ns
        region.misses = 0
        region.dirty = False
        region.dirty_frames = 0
        region.signature = self._signature_for_screen_rect(frame, rect)
        if merged == region.text:
            if region.translation:
                region.stale = False
            return
        log.info("Bölge metni değişti [%d]: %r -> %r", region_id, region.text, merged)
        region.text = merged
        region.revision += 1
        region.translation = ""
        region.translation_provider = ""
        region.stale = True
        region.last_changed_ns = now_ns
        region.last_translate_attempt_ns = 0

    # ------------------------------------------------------------------
    def pending_translations(self, now_ns: int, limit: int) -> list[TextRegion]:
        cooldown = int(self.tr_cfg.retry_cooldown_ms) * 1_000_000
        out = [
            r
            for r in self._regions.values()
            if r.text
            and has_letters(r.text)
            and (not r.translation or r.stale)
            and now_ns - r.last_translate_attempt_ns >= cooldown
        ]
        out.sort(key=lambda r: r.last_changed_ns)
        return out[: max(1, limit)]

    def mark_translate_attempt(self, region_id: int, now_ns: int) -> None:
        r = self._regions.get(region_id)
        if r:
            r.last_translate_attempt_ns = now_ns

    def set_translation(
        self,
        region_id: int,
        translation: str,
        provider: str,
        now_ns: int,
        expected_revision: Optional[int] = None,
    ) -> bool:
        r = self._regions.get(region_id)
        if r is None:
            return False
        if expected_revision is not None and r.revision != expected_revision:
            return False
        if not translation:
            return False
        r.translation = translation
        r.translation_provider = provider
        r.translated_at_ns = now_ns
        r.stale = False
        r.latency_ms = max(0.0, (now_ns - r.last_changed_ns) / 1_000_000.0)
        return True

    # ------------------------------------------------------------------
    def overlay_items(self, now_ns: int, ttl_ms: int) -> list[OverlayItem]:
        ttl = int(ttl_ms) * 1_000_000
        items: list[OverlayItem] = []
        for r in self._regions.values():
            if not r.translation:
                continue
            if now_ns - r.last_present_ns > ttl:
                continue
            items.append(
                OverlayItem(
                    region_id=r.id,
                    rect=r.rect,
                    text=r.translation,
                    source_text=r.text,
                    stale=r.stale,
                    translated_at_ns=r.translated_at_ns,
                )
            )
        # Örtüşen kutuları tekilleştir: daha yeni çeviri kazanır.
        items.sort(key=lambda i: i.translated_at_ns, reverse=True)
        kept: list[OverlayItem] = []
        for it in items:
            if any(k.rect.iou(it.rect) > 0.6 for k in kept):
                continue
            kept.append(it)
        kept.sort(key=lambda i: (i.rect.y, i.rect.x))
        return kept

    def debug_counts(self) -> tuple[int, int, int]:
        stale = sum(1 for r in self._regions.values() if r.stale)
        translated = sum(1 for r in self._regions.values() if r.translation)
        return len(self._regions), translated, stale
