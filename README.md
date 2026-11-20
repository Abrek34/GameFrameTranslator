# GameFrame Translator

Düşük gecikmeli, gerçek sunulmuş kareleri yakalayan, metin bölgelerini yalnızca
değiştiğinde OCR'dan geçiren ve Türkçe overlay çizen çeviri sistemi.

- Linux/Proton öncelikli (Vulkan layer + XComposite fallback), Windows DXGI.
- Çerçeve üretimi/interpolasyon yok; yalnızca gerçekten sunulmuş kareler.
- Render döngüsü asla bloke edilmez; OCR/çeviri ayrı iş parçacıklarında.

## Yapı
- `prototype/` — Faz 0 Python prototipi (mss/XComposite + Tesseract + sözlük/Google + GTK/Tk overlay)
- `src/` — Faz 1 C++20 çekirdek (CMake)
- `layer/` — Faz 2 Vulkan layer iskeleti
- `docs/` — mimari, modül, yakalama, benchmark, yol haritası
- `tools/` — benchmark yardımcıları

## Hızlı Başlangıç (Python prototipi)
```bash
cd prototype
python -m venv .venv --system-site-packages
. .venv/bin/activate
pip install -r requirements.txt
# Tek kare test (ekransız)
python -m gameframe --source synthetic --once --provider dictionary
# Oyun penceresi ile (overlay açıksa XComposite kullanın)
python -m gameframe --source xcomposite --window-title "Oyun Başlığı" --overlay gtk
```

## Hızlı Başlangıç (C++ çekirdek)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gft --source synthetic --once --provider dictionary
ctest --test-dir build
```

## Anti-cheat notu
Yalnızca pasif yakalama (Vulkan present hook / DXGI AcquireNextFrame / XComposite).
Bellek okuma, injection veya process hollowing yok. Detay: `docs/anti_cheat.md`.
