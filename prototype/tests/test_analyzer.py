from __future__ import annotations

import unittest

import numpy as np

from gameframe.analyzer import FrameDiffer, RegionTracker, signature_from_rgb
from gameframe.config import AnalyzerConfig, TranslateConfig
from gameframe.types import Frame, OcrLine, Rect


def make_frame(w=320, h=180, color=(10, 10, 10)) -> Frame:
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:, :] = color
    return Frame(image=img, origin_x=0, origin_y=0)


class AnalyzerTest(unittest.TestCase):
    def test_diff_detects_change(self) -> None:
        cfg = AnalyzerConfig(analysis_width=160, pixel_threshold=10)
        d = FrameDiffer(cfg)
        f1 = make_frame()
        r1 = d.compute(f1)
        self.assertTrue(r1.first_frame)
        r2 = d.compute(f1)
        self.assertAlmostEqual(r2.changed_ratio, 0.0, places=4)
        f2 = f1
        f2.image = f1.image.copy()
        f2.image[20:60, 20:100] = 240
        r3 = d.compute(f2)
        self.assertGreater(r3.changed_ratio, 0.0)
        self.assertIsNotNone(r3.changed_bbox)
        assert r3.changed_bbox is not None
        self.assertGreaterEqual(r3.changed_bbox.w, 70)
        self.assertLessEqual(r3.changed_bbox.x, 25)

    def test_signature(self) -> None:
        a = np.zeros((30, 100, 3), dtype=np.uint8)
        a[5:25, 5:90] = 255
        s1 = signature_from_rgb(a)
        s2 = signature_from_rgb(a.copy())
        self.assertLess(s1.delta(s2), 0.001)
        b = a.copy()
        b[5:25, 5:45] = 0
        self.assertGreater(s1.delta(signature_from_rgb(b)), 0.01)

    def test_region_lifecycle(self) -> None:
        cfg = AnalyzerConfig(expire_ms=1000, region_change=0.01)
        tr = RegionTracker(cfg, TranslateConfig(retry_cooldown_ms=0))
        frame = make_frame()
        frame.image[30:50, 20:150] = 240
        differ = FrameDiffer(cfg)
        differ.compute(frame)
        line = OcrLine(rect=Rect(20, 30, 130, 20), text="Continue", confidence=90)
        now = 1_000_000_000
        tr.ingest_full(frame, [line], now)
        self.assertEqual(len(tr), 1)
        self.assertEqual(len(tr.pending_translations(now, 4)), 1)
        r = tr.regions[0]
        self.assertTrue(tr.set_translation(r.id, "Devam", "dictionary", now + 1_000_000))
        self.assertEqual(len(tr.overlay_items(now + 2_000_000, 5000)), 1)
        self.assertEqual(len(tr.pending_translations(now + 2_000_000, 4)), 0)

        # Aynı kare: dirty olmamalı
        diff_same = differ.compute(frame)
        dirty = tr.observe(frame, differ, diff_same, now + 2_000_000)
        self.assertEqual(dirty, [])
        # Metin bölgesi değişti: dirty olmalı
        frame.image[30:50, 20:150] = 30
        diff_changed = differ.compute(frame)
        dirty = tr.observe(frame, differ, diff_changed, now + 3_000_000)
        self.assertEqual(len(dirty), 1)
        # Aynı metin OCR'ı dirty'yi temizler
        tr.ingest_region(frame, r.id, [line], now + 3_100_000)
        self.assertEqual(tr.regions[0].dirty, False)
        # Uzun süre görülmezse düşer
        tr.expire(now + 10_000_000_000)
        self.assertEqual(len(tr), 0)


if __name__ == "__main__":
    unittest.main()
