from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from gameframe.config import load_config


class ConfigTest(unittest.TestCase):
    def test_load_and_region(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "cfg.yaml"
            p.write_text(
                "capture:\n  backend: synthetic\n  region: [10, 20, 300, 200]\n"
                "analyzer:\n  change_trigger: 0.5\n"
                "ocr:\n  lang: deu\n",
                encoding="utf-8",
            )
            cfg = load_config(p)
            self.assertEqual(cfg.capture.backend, "synthetic")
            self.assertEqual(cfg.capture.region, (10, 20, 300, 200))
            self.assertAlmostEqual(cfg.analyzer.change_trigger, 0.5)
            self.assertEqual(cfg.ocr.lang, "deu")

    def test_unknown_key_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "cfg.yaml"
            p.write_text("capture:\n  nope: 1\n", encoding="utf-8")
            cfg = load_config(p)
            self.assertEqual(cfg.capture.backend, "auto")


if __name__ == "__main__":
    unittest.main()
