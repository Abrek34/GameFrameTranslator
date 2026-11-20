from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from gameframe.cache import TranslationCache


class CacheTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "cache.sqlite3"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_memory_and_disk_roundtrip(self) -> None:
        c = TranslationCache(self.path, memory_size=8)
        self.assertIsNone(c.get("en", "tr", "Continue"))
        c.put("en", "tr", "Continue", "Devam", "dictionary")
        self.assertEqual(c.get("en", "tr", "Continue"), ("Devam", "dictionary"))
        self.assertEqual(c.get("en", "tr", "  Continue  "), ("Devam", "dictionary"))
        c.close()

        c2 = TranslationCache(self.path, memory_size=8)
        self.assertEqual(c2.get("en", "tr", "Continue"), ("Devam", "dictionary"))
        self.assertGreaterEqual(c2.disk_hits, 1)
        c2.close()

    def test_normalized_key(self) -> None:
        c = TranslationCache(self.path)
        c.put("en", "tr", "Press   E\nto interact", "Etkileşim için E'ye bas", "x")
        self.assertIsNotNone(c.get("en", "tr", "Press E to interact"))
        self.assertIsNone(c.get("de", "tr", "Press E to interact"))
        c.close()


if __name__ == "__main__":
    unittest.main()
