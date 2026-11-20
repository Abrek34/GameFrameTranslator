from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from gameframe.cache import TranslationCache
from gameframe.config import TranslateConfig
from gameframe.translate import DictionaryTranslator, TranslatorChain


class FakeTranslator:
    name = "fake"

    def __init__(self, mapping: dict[str, str]):
        self.mapping = mapping
        self.calls = 0

    def translate(self, text: str, source_lang: str, target_lang: str):
        self.calls += 1
        return self.mapping.get(text)


class TranslateTest(unittest.TestCase):
    def test_dictionary(self) -> None:
        d = DictionaryTranslator()
        self.assertEqual(d.translate("Continue", "en", "tr"), "Devam")
        self.assertEqual(d.translate("  continue ", "en", "tr"), "Devam")
        self.assertIsNone(d.translate("Some unknown sentence", "en", "tr"))

    def test_chain_cache_and_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cache = TranslationCache(Path(tmp) / "c.sqlite3")
            fake = FakeTranslator({"Hello there": "Merhaba"})
            cfg = TranslateConfig(fallback="original")
            chain = TranslatorChain(cfg, cache, [fake])
            r1 = chain.translate("Hello there")
            self.assertEqual(r1.text, "Merhaba")
            self.assertFalse(r1.cached)
            r2 = chain.translate("Hello there")
            self.assertTrue(r2.cached)
            self.assertEqual(fake.calls, 1)
            r3 = chain.translate("Unknown")
            self.assertEqual(r3.text, "Unknown")
            self.assertFalse(r3.ok)
            cache.close()

    def test_no_letters(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cache = TranslationCache(Path(tmp) / "c.sqlite3")
            chain = TranslatorChain(TranslateConfig(), cache, [])
            r = chain.translate("1234")
            self.assertEqual(r.text, "1234")
            self.assertEqual(r.provider, "skip")
            cache.close()


if __name__ == "__main__":
    unittest.main()
