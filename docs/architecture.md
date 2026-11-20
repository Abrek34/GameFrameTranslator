# Mimari

## Thread modeli
- **capture thread**: kaynaktan kare alır, `LatestSlot<Frame>`'e yazar. Eski kareler bilinçli olarak düşer (drop-late).
- **analyzer thread**: son kareyi alır; fark (FrameDiffer), bölge izleme (RegionTracker), OCR kararları, çeviri kuyruğuna iş atar.
- **translation worker pool** (2 thread): ağ/önbellek çevirisini render'dan bağımsız yapar; sonuçları tracker'a yazar ve overlay modelini yayınlar.
- **overlay thread (GTK main)**: modeli poll eder, X11/GTK ile çizer; oyun render döngüsünü bloke etmez.

## Veri akışı
1. `FrameSource::grabOnce` → `LatestSlot::publish`
2. `FrameDiffer::compute` → `DiffResult`
3. `RegionTracker::observe` → dirty bölge listesi
4. OCR (tam keşif veya bölge) → `RegionTracker::ingest*`
5. Çeviri havuzu → `RegionTracker::setTranslation` → `LatestModelSlot::publish`
6. Overlay → `LatestModelSlot::get` → çizim

## Gecikme hedefi
- Tam pipeline (metin değişimi → overlay) hedef <800 ms, kabul edilebilir <1500 ms.
- Prototip ölçümü: ~340–470 ms (Tesseract CLI, sentetik sahne).
- C++ çekirdek: ~470 ms (Tesseract CLI).

## Bellek/CPU
- Boşta <300 MB, aktif <800 MB, OCR boşta CPU <%3 hedefi C++/ONNX ile.
- Python prototipi CPU yoğun (Tesseract); Faz 3 ONNX Runtime ile düşürülecek.
