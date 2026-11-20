"""OCR motorları.

Faz 0: Tesseract CLI (TSV). Ek Python paketi gerektirmez; tesseract ve dil
dosyaları sistemde olmalı. RapidOCR/ONNX opsiyonel backend olarak hazırdır,
model dosyaları Faz 3'te eklenecek.
"""
from __future__ import annotations

import logging
import os
import shutil
import subprocess
import tempfile
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image

from .config import AppConfig, OcrConfig
from .types import Frame, OcrLine, Rect, normalize_text

log = logging.getLogger(__name__)


class OcrError(RuntimeError):
    pass


class OcrEngine(ABC):
    name = "base"

    @abstractmethod
    def recognize(
        self,
        frame: Frame,
        roi: Optional[Rect] = None,
        psm: Optional[int] = None,
        upscale: Optional[float] = None,
    ) -> list[OcrLine]:
        raise NotImplementedError

    def close(self) -> None:
        pass


class TesseractOcr(OcrEngine):
    name = "tesseract"

    def __init__(self, cfg: OcrConfig):
        self.cfg = cfg
        self._checked = False

    def _check(self) -> None:
        if self._checked:
            return
        self._checked = True
        cmd = self.cfg.tesseract_cmd
        if not shutil.which(cmd) and not Path(cmd).exists():
            raise OcrError(
                "tesseract bulunamadı. Kurulum: sudo pacman -S tesseract tesseract-data-eng "
                "(veya --ocr-backend rapidocr)."
            )

    def recognize(
        self,
        frame: Frame,
        roi: Optional[Rect] = None,
        psm: Optional[int] = None,
        upscale: Optional[float] = None,
    ) -> list[OcrLine]:
        self._check()
        local = (
            Rect(roi.x - frame.origin_x, roi.y - frame.origin_y, roi.w, roi.h).clipped(
                frame.width, frame.height
            )
            if roi is not None
            else Rect(0, 0, frame.width, frame.height)
        )
        if local.empty:
            return []
        crop = frame.crop(local)
        scale = max(0.25, float(upscale if upscale is not None else self.cfg.upscale))
        if crop.shape[0] < self.cfg.auto_upscale_below_px:
            scale = max(scale, 2.0)
        if abs(scale - 1.0) > 0.01:
            crop = np.asarray(
                Image.fromarray(crop).resize(
                    (max(1, int(crop.shape[1] * scale)), max(1, int(crop.shape[0] * scale))),
                    Image.LANCZOS,
                ),
                dtype=np.uint8,
            )

        # Oyun içi/HUD metinlerde kontrastı artır: gri + autocontrast.
        # (Tesseract bazen renkli arka planda metni kaçırabiliyor.)
        try:
            img = Image.fromarray(crop).convert("L")
            from PIL import ImageOps, ImageEnhance
            img = ImageOps.autocontrast(img, cutoff=1)
            img = ImageEnhance.Contrast(img).enhance(1.6)
            img = ImageEnhance.Sharpness(img).enhance(1.3)
        except Exception:
            img = Image.fromarray(crop)

        tmp_path = ""
        try:
            fd, tmp_path = tempfile.mkstemp(prefix="gft-ocr-", suffix=".png")
            os.close(fd)
            img.save(tmp_path, format="PNG")
            cmd = [
                self.cfg.tesseract_cmd,
                tmp_path,
                "stdout",
                "-l",
                self.cfg.lang,
                "--psm",
                str(int(psm if psm is not None else self.cfg.psm)),
                "--oem",
                "1",
                "tsv",
            ]
            env = dict(os.environ)
            env.setdefault("OMP_THREAD_LIMIT", str(self.cfg.omp_thread_limit))
            try:
                proc = subprocess.run(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=float(self.cfg.timeout_s),
                    env=env,
                    check=False,
                )
            except subprocess.TimeoutExpired as exc:
                raise OcrError(f"Tesseract zaman aşımı ({self.cfg.timeout_s}s)") from exc
            if proc.returncode != 0:
                err = proc.stderr.decode("utf-8", "replace").strip().splitlines()
                detail = " | ".join(err[-3:]) if err else f"çıkış kodu {proc.returncode}"
                raise OcrError(f"Tesseract hatası: {detail}")
            text = proc.stdout.decode("utf-8", "replace")
            lines = self._parse_tsv(text, frame, local, scale)
            return lines
        finally:
            if tmp_path:
                try:
                    os.unlink(tmp_path)
                except OSError:
                    pass

    def _parse_tsv(self, tsv: str, frame: Frame, local: Rect, scale: float) -> list[OcrLine]:
        rows = tsv.splitlines()
        if not rows:
            return []
        groups: dict[tuple[int, int, int, int], dict] = {}
        order: list[tuple[int, int, int, int]] = []
        for row in rows[1:]:
            cols = row.split("\t")
            if len(cols) < 12:
                continue
            try:
                level = int(cols[0])
                if level != 5:
                    continue
                page, block, par, line = (int(cols[i]) for i in range(1, 5))
                left, top, width, height = (int(cols[i]) for i in range(6, 10))
                conf = float(cols[10])
            except (TypeError, ValueError):
                continue
            word = cols[11].strip()
            if not word or conf < self.cfg.min_confidence:
                continue
            key = (page, block, par, line)
            if key not in groups:
                groups[key] = {
                    "words": [],
                    "x1": left,
                    "y1": top,
                    "x2": left + width,
                    "y2": top + height,
                    "conf": [],
                }
                order.append(key)
            g = groups[key]
            g["words"].append(word)
            g["x1"] = min(g["x1"], left)
            g["y1"] = min(g["y1"], top)
            g["x2"] = max(g["x2"], left + width)
            g["y2"] = max(g["y2"], top + height)
            g["conf"].append(conf)
        out: list[OcrLine] = []
        for key in order:
            g = groups[key]
            text = normalize_text(" ".join(g["words"]))
            if not text or not any(ch.isalnum() for ch in text):
                continue
            x = local.x + int(round(g["x1"] / scale))
            y = local.y + int(round(g["y1"] / scale))
            w = max(1, int(round((g["x2"] - g["x1"]) / scale)))
            h = max(1, int(round((g["y2"] - g["y1"]) / scale)))
            if h < self.cfg.min_text_height:
                continue
            out.append(
                OcrLine(
                    rect=Rect(frame.origin_x + x, frame.origin_y + y, w, h),
                    text=text,
                    confidence=float(np.mean(g["conf"])) if g["conf"] else 0.0,
                )
            )
        out.sort(key=lambda l: (l.rect.y, l.rect.x))
        return out


class RapidOcrEngine(OcrEngine):
    """Opsiyonel: rapidocr_onnxruntime. Model dosyaları Faz 3'te sabitlenecek."""

    name = "rapidocr"

    def __init__(self, cfg: OcrConfig):
        try:
            from rapidocr_onnxruntime import RapidOCR  # type: ignore
        except Exception as exc:  # pragma: no cover - optional dependency
            raise OcrError(
                "rapidocr_onnxruntime yok. Kurulum: pip install rapidocr_onnxruntime onnxruntime"
            ) from exc
        self.cfg = cfg
        self._engine = RapidOCR()

    def recognize(
        self,
        frame: Frame,
        roi: Optional[Rect] = None,
        psm: Optional[int] = None,
        upscale: Optional[float] = None,
    ) -> list[OcrLine]:
        local = (
            Rect(roi.x - frame.origin_x, roi.y - frame.origin_y, roi.w, roi.h).clipped(
                frame.width, frame.height
            )
            if roi is not None
            else Rect(0, 0, frame.width, frame.height)
        )
        if local.empty:
            return []
        crop = frame.crop(local)
        raw = self._engine(crop)
        result = raw[0] if isinstance(raw, tuple) else raw
        out: list[OcrLine] = []
        for item in result or []:
            try:
                box, text, score = item[0], str(item[1]), float(item[2])
            except Exception:
                continue
            if score < self.cfg.min_confidence / 100.0:
                continue
            xs = [float(p[0]) for p in box]
            ys = [float(p[1]) for p in box]
            x1, y1, x2, y2 = min(xs), min(ys), max(xs), max(ys)
            text = normalize_text(text)
            if not text:
                continue
            out.append(
                OcrLine(
                    rect=Rect(
                        frame.origin_x + local.x + int(x1),
                        frame.origin_y + local.y + int(y1),
                        max(1, int(x2 - x1)),
                        max(1, int(y2 - y1)),
                    ),
                    text=text,
                    confidence=score * 100.0,
                )
            )
        out.sort(key=lambda l: (l.rect.y, l.rect.x))
        return out


class NullOcr(OcrEngine):
    name = "null"

    def recognize(
        self,
        frame: Frame,
        roi: Optional[Rect] = None,
        psm: Optional[int] = None,
        upscale: Optional[float] = None,
    ) -> list[OcrLine]:
        return []


def create_ocr(cfg: AppConfig) -> OcrEngine:
    backend = cfg.ocr.backend
    if backend == "null":
        return NullOcr()
    if backend == "tesseract":
        return TesseractOcr(cfg.ocr)
    if backend == "rapidocr":
        return RapidOcrEngine(cfg.ocr)
    if backend == "auto":
        if shutil.which(cfg.ocr.tesseract_cmd):
            return TesseractOcr(cfg.ocr)
        try:
            return RapidOcrEngine(cfg.ocr)
        except OcrError:
            log.error("OCR motoru bulunamadı; null backend kullanılıyor")
            return NullOcr()
    raise OcrError(f"Bilinmeyen OCR backend'i: {backend}")
