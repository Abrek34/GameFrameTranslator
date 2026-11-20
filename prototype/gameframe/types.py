"""Temel veri tipleri. Prototip ile C++ çekirdeği arasındaki sözleşmelerin
Python karşılıkları burada tutulur; alan adları C++ tarafıyla uyumludur.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Optional

import numpy as np


# ---------------------------------------------------------------------------
# Geometri
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    w: int
    h: int

    @property
    def right(self) -> int:
        return self.x + self.w

    @property
    def bottom(self) -> int:
        return self.y + self.h

    @property
    def area(self) -> int:
        return max(0, self.w) * max(0, self.h)

    @property
    def empty(self) -> bool:
        return self.w <= 0 or self.h <= 0

    def clipped(self, width: int, height: int) -> "Rect":
        x1 = max(0, min(self.x, width))
        y1 = max(0, min(self.y, height))
        x2 = max(0, min(self.right, width))
        y2 = max(0, min(self.bottom, height))
        return Rect(x1, y1, max(0, x2 - x1), max(0, y2 - y1))

    def expanded(self, pad: int, width: int, height: int) -> "Rect":
        return Rect(self.x - pad, self.y - pad, self.w + 2 * pad, self.h + 2 * pad).clipped(
            width, height
        )

    def union(self, other: "Rect") -> "Rect":
        x1 = min(self.x, other.x)
        y1 = min(self.y, other.y)
        x2 = max(self.right, other.right)
        y2 = max(self.bottom, other.bottom)
        return Rect(x1, y1, x2 - x1, y2 - y1)

    def intersects(self, other: "Rect") -> bool:
        return not (
            self.right <= other.x
            or other.right <= self.x
            or self.bottom <= other.y
            or other.bottom <= self.y
        )

    def intersection(self, other: "Rect") -> "Rect":
        x1 = max(self.x, other.x)
        y1 = max(self.y, other.y)
        x2 = min(self.right, other.right)
        y2 = min(self.bottom, other.bottom)
        return Rect(x1, y1, max(0, x2 - x1), max(0, y2 - y1))

    def iou(self, other: "Rect") -> float:
        inter = self.intersection(other).area
        if inter <= 0:
            return 0.0
        return inter / float(self.area + other.area - inter)

    def center(self) -> tuple[float, float]:
        return (self.x + self.w / 2.0, self.y + self.h / 2.0)

    def contains_point(self, px: float, py: float) -> bool:
        return self.x <= px < self.right and self.y <= py < self.bottom


@dataclass(frozen=True)
class Cell:
    rect: Rect
    changed_ratio: float


# ---------------------------------------------------------------------------
# Kare
# ---------------------------------------------------------------------------
@dataclass
class Frame:
    """Gerçek (sunulmuş) bir kare. image RGB uint8, HxWx3, contiguous."""

    image: np.ndarray
    origin_x: int = 0
    origin_y: int = 0
    captured_at_ns: int = field(default_factory=time.monotonic_ns)
    seq: int = 0
    source: str = "unknown"

    @property
    def width(self) -> int:
        return int(self.image.shape[1])

    @property
    def height(self) -> int:
        return int(self.image.shape[0])

    def crop(self, rect: Rect) -> np.ndarray:
        r = rect.clipped(self.width, self.height)
        if r.empty:
            return np.zeros((0, 0, 3), dtype=np.uint8)
        return self.image[r.y : r.bottom, r.x : r.right]


@dataclass(frozen=True)
class RegionSignature:
    """Bölge parmak izi: dHash + 16x16 gri küçük resim.

    dHash tek başına anti-aliasing'e duyarlıdır; küçük resim farkı ile birlikte
    kullanılır. Eşik altındaki fark "aynı metin" sayılır ve OCR tekrarlanmaz.
    """

    dhash: int
    thumb: np.ndarray

    def delta(self, other: "RegionSignature") -> float:
        if self.thumb.shape != other.thumb.shape:
            return 1.0
        return float(
            np.mean(np.abs(self.thumb.astype(np.int16) - other.thumb.astype(np.int16)))
            / 255.0
        )


# ---------------------------------------------------------------------------
# OCR / bölge / overlay
# ---------------------------------------------------------------------------
@dataclass
class OcrLine:
    rect: Rect  # ekran koordinatı (frame.origin dahil)
    text: str
    confidence: float = 0.0


@dataclass
class TextRegion:
    id: int
    rect: Rect
    text: str = ""
    translation: str = ""
    translation_provider: str = ""
    signature: Optional[RegionSignature] = None
    revision: int = 0
    stale: bool = False
    dirty: bool = False
    dirty_frames: int = 0
    misses: int = 0
    first_seen_ns: int = 0
    last_changed_ns: int = 0
    last_present_ns: int = 0
    last_ocr_ns: int = 0
    translated_at_ns: int = 0
    last_translate_attempt_ns: int = 0
    latency_ms: float = 0.0
    ocr_source: str = ""

    @property
    def display_text(self) -> str:
        return self.translation or self.text


@dataclass
class OverlayItem:
    region_id: int
    rect: Rect
    text: str
    source_text: str = ""
    stale: bool = False
    translated_at_ns: int = 0


@dataclass
class OverlayModel:
    items: list[OverlayItem] = field(default_factory=list)
    updated_at_ns: int = field(default_factory=time.monotonic_ns)
    debug: str = ""

    def signature(self) -> tuple:
        return tuple(
            (i.region_id, i.rect, i.text, i.stale) for i in sorted(self.items, key=lambda x: x.region_id)
        )


# ---------------------------------------------------------------------------
# Fark / istatistik
# ---------------------------------------------------------------------------
@dataclass
class DiffResult:
    changed_ratio: float = 1.0
    changed_bbox: Optional[Rect] = None
    cells: list[Cell] = field(default_factory=list)
    frame_hash: int = 0
    first_frame: bool = False


@dataclass
class CaptureStats:
    captured: int = 0
    dropped: int = 0
    fps: float = 0.0
    last_error: str = ""


@dataclass
class PipelineStats:
    frames_seen: int = 0
    frames_analyzed: int = 0
    frames_dropped: int = 0
    ocr_calls: int = 0
    full_ocr_calls: int = 0
    region_ocr_calls: int = 0
    translate_calls: int = 0
    cache_hits: int = 0
    overlay_updates: int = 0
    errors: int = 0


# ---------------------------------------------------------------------------
# Metin yardımcıları
# ---------------------------------------------------------------------------
def normalize_text(text: str) -> str:
    """OCR/çeviri anahtarı için boşlukları normalize eder."""
    text = (text or "").replace("\u00ad", "").replace("\u200b", "")
    return " ".join(text.split()).strip()


def has_letters(text: str) -> bool:
    return any(ch.isalpha() for ch in text)
