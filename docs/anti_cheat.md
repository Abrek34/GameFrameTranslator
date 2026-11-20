# Anti-cheat güvenliği

GameFrame Translator yalnızca **pasif** çalışır:

- Oyun belleğini okumaz, yazmaz.
- Process injection, DLL injection, hook injection yapmaz.
- Yalnızca grafik API present hook'u (Vulkan `vkQueuePresentKHR`) veya OS seviyesi yakalama (DXGI `AcquireNextFrame`, XComposite) kullanır.
- Çeviri ağ isteği oyun sürecinden değil, GFT sürecinden yapılır.

## Oyunlar için öneriler
- Anti-cheat'i tetikleme riskini en aza indirmek için Vulkan layer yerine XComposite/DXGI fallback kullanılabilir.
- Çevrimiçi rekabetçi oyunlarda önce tek oyunculu/modlarda test edin.
- Oyunun kendi anti-cheat dokümantasyonunu kontrol edin; "screen capture" izinli mi?

## Yasal/etik
- Bu araç çeviri amaçlıdır; hile, bot veya otomasyon sağlamaz.
- Oyun geliştiricilerinin kullanım şartlarına uyun.
