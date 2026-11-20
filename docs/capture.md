# Yakalama

## İlkeler
- Yalnızca gerçekten sunulmuş kareler yakalanır: Vulkan `vkQueuePresentKHR`, DXGI `AcquireNextFrame`, XComposite pixmap.
- Frame generation/interpolasyon yok.
- Pasif yakalama; bellek okuma/injection yok.

## Linux
1. **Vulkan layer (Faz 2, öncelikli)**: oyunun swapchain present'ini yakalar; overlay kareye karışmaz.
2. **XComposite (Faz 0/1)**: hedef pencere pixmap'i. Overlay ayrı pencere olduğu için yakalanmaz (WDA_EXCLUDEFROMCAPTURE'ın Linux karşılığı).
3. **PipeWire/XComposite/Gamescope fallback** (Faz 2).

## Windows
- DXGI Desktop Duplication (`AcquireNextFrame`), dirty rect desteği.
- Overlay: layered window + `WDA_EXCLUDEFROMCAPTURE`.

## Wayland native
- Native Wayland oyunlar X11 overlay ile güvenilir şekilde üst üste çıkmaz; Faz 2'de `wlr-layer-shell` backend'i eklenecek.

## Geri besleme döngüsü önleme
- Root/ekran yakalama overlay'i de yakalar → OCR geri besleme döngüsü.
- Çözüm: XComposite hedef pencere veya Vulkan layer. C++/Python çekirdekleri root+overlay birlikte çalışmayı varsayılan olarak reddeder (`allow_root_with_overlay`).
