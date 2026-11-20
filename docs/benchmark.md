# Steam/Proton benchmark prosedürü

## Amaç
Gerçek bir Proton oyununda gecikme, FPS etkisi, CPU/RSS ve OCR çağrı sayısını ölçmek.

## Hazırlık
1. Oyunu Proton ile çalıştırın (örn. Steam → "Force the use of a specific Steam Play compatibility tool").
2. Oyun pencere başlığını alın (`xprop _NET_WM_NAME` veya `wmctrl -l`).
3. GFT'yi XComposite ile çalıştırın:
   ```bash
   ./build/gft --source xcomposite --window-title "Oyun Başlığı" --overlay gtk --provider auto --debug
   ```
   veya Python prototipi:
   ```bash
   python -m gameframe --source xcomposite --window-title "Oyun Başlığı" --overlay gtk
   ```
4. Oyun içi FPS sayacını açın (Steam overlay: Shift+Tab → FPS sayacı).

## Ölçülecekler
- **Gecikme**: oyun metni değişti → overlay'de Türkçe görünmesi. Sentetik sahne ile otomatik; gerçek oyunda telefonla/kronometre ile manuel veya oyun içi diyalog geçişlerini video kaydıyla.
- **OCR çağrı sayısı**: saniyede 2–5 arası hedef; metrik JSONL'den (`ocr_calls`).
- **FPS etkisi**: GFT açık/kapalı FPS farkı <%5.
- **Kaynak**: CPU <%3 OCR boşta, RSS <800 MB aktif.

## Sentetik benchmark (otomatik)
```bash
DISPLAY=:0 python prototype/tools/benchmark.py --duration 30
DISPLAY=:0 python prototype/tools/benchmark.py --duration 30 --with-overlay
```
Çıktı: uçtan uca gecikme p50/p95, OCR çağrı sayısı, CPU/RSS özeti.

## Raporlama
- `~/.local/state/gameframe/summary-*.json` ve `metrics-*.jsonl` dosyalarını saklayın.
- Oyun adı, Proton sürümü, çözünürlük, ayarları kaydedin.
