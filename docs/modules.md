# Modül dokümantasyonu

Her modül için: bağımlılıklar, thread modeli, I/O, hata yönetimi.

## `src/common`
- **types.hpp**: Rect, Frame, OcrLine, TextRegion, OverlayModel, DiffResult, normalizeText.
  - Bağımlılık: standart kütüphane.
  - Thread: value type, dış senkronizasyon.
- **config.hpp/cpp**: minimal YAML ayar yükleyici.
  - Bağımlılık: filesystem, cstdlib.
  - I/O: ayar dosyası okuma, log dir/cache path genişletme.
  - Hata: bilinmeyen anahtar uyarı, sayı dönüştürme hatası uyarı.
- **latest_slot.hpp**: `LatestSlot<T>` drop-late kuyruk.
  - Thread: mutex + condition_variable.
- **thread_pool.hpp/cpp**: sabit iş parçacıklı havuz.
  - Thread: kendi worker'ları.
- **metrics.hpp/cpp**: JSONL olayları, gecikme yüzdelikleri, CPU/RSS örnekleyici.
  - I/O: log dir dosyaları.
  - Hata: dosya açma hatası log, devam.
- **model_slot.hpp**: `LatestModelSlot`.
- **log.hpp/cpp**: stderr log.

## `src/capture`
- **frame_source.hpp/cpp**: fabrika (`createFrameSource`).
- **x11_capture.hpp/cpp**: XComposite pencere yakalama + root fallback.
  - Bağımlılık: X11, Xcomposite.
  - Thread: kendi capture thread'i; `XInitThreads`.
  - I/O: X sunucusu round-trip.
  - Hata: pencere bulunamazsa `std::runtime_error`; thread yeniden dener.
- **synthetic_capture.hpp/cpp**: cairo ile sahte sahne (ekransız test).
  - Bağımlılık: cairo.
- **shm_frame_source.hpp**: Vulkan layer ring protokolü (Faz 2).
- **dxgi_capture.hpp**, **wayland_portal.hpp**: iskelet (Faz 2/Windows).

## `src/analyzer`
- **analyzer.hpp/cpp**: `FrameDiffer`, `RegionTracker`.
  - Bağımlılık: types, config.
  - Thread: dış mutex; RegionTracker dahili mutex.
  - Hata: imza hesaplama güvenli; boş kareler atlanır.

## `src/ocr`
- **ocr_engine.hpp/cpp**: `TesseractCliOcr` (CLI), `NullOcr`.
  - Bağımlılık: Tesseract CLI, POSIX (mkstemp, popen, timeout).
  - I/O: geçici PPM, tesseract stdout (TSV).
  - Hata: `OcrError`; zaman aşımı, eksik dil dosyası.

## `src/translate`
- **translator.hpp/cpp**: sözlük, Google web, zincir, önbellek entegrasyonu.
  - Bağımlılık: libcurl, json-c, SQLite cache.
  - Thread: stateless sağlayıcılar; cache thread-safe.
  - Hata: ağ hataları log, fallback.

## `src/cache`
- **sqlite_cache.hpp/cpp**: SQLite + bellek LRU.
  - Bağımlılık: SQLite3.
  - I/O: WAL veritabanı.
  - Hata: açma hatası → yalnızca bellek.

## `src/overlay`
- **overlay.hpp/cpp**: GTK3 şeffaf, tıklama geçirgen overlay.
  - Bağımlılık: GTK3, cairo, pango.
  - Thread: GTK main thread.
  - Hata: display yoksa exception.

## `src/pipeline`
- **pipeline.hpp/cpp**: orkestrasyon, latest frame/model slot, rate limiter.
  - Thread: capture/analyzer/translation/overlay.

## `src/app/main.cpp`
- CLI, bileşen kurulumu, yaşam döngüsü, sinyal yönetimi.
