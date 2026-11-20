# Yol haritası

## Faz 0 — Python prototip (tamamlandı)
- mss/XComposite yakalama, Tesseract OCR, sözlük/Google çeviri, GTK/Tk overlay, metrikler, sentetik benchmark.

## Faz 1 — C++ çekirdek (tamamlandı)
- C++20/CMake, modüller, ThreadPool, latest-frame slot, SQLite cache, X11 overlay, testler.

## Faz 2 — Yakalama (iskelet)
- Windows DXGI Desktop Duplication.
- Linux Vulkan layer (gerçek present hook) + PipeWire/XComposite/Gamescope fallback.
- Wayland layer-shell overlay.

## Faz 3 — Akıllı OCR
- DBNet/EAST metin tespiti, ROI genişletme, SSIM/pHash değişim filtresi, ONNX Runtime (CPU/GPU).
- OCR çağrı sayısı 2–5/s hedefi.

## Faz 4 — Çeviri/overlay cilalama
- Terim sözlüğü, diyalog geçmişi, font ölçekleme, overlay sürükleme/kilitleme, kısayollar.
- HDR farkındalığı, DPI.

## Faz 5 — Dağıtım
- Windows installer, Linux paketleri (AppImage/Flatpak), otomatik güncelleme.
