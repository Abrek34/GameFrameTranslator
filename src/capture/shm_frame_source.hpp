#pragma once
// Faz 2: Vulkan layer -> shared memory frame transport.
// Oyunun swapchain'inden gerçek sunulmuş kareyi yakalar (üretim yok).
//
// Ring buffer protokolü (tek-yazıcı, tek-okuyucu, POSIX shm).
// Header + N slot; her slot RGB888 + metadata. Slot boyutu ve sayısı
// sabitlenmiştir; farklı çözünürlükler için en yakın slot kullanılır.

#include <cstdint>
#include <cstddef>

namespace gft {

constexpr const char* SHM_NAME = "/gft_frame_ring";
constexpr uint32_t SHM_MAGIC = 0x47465430;  // "GFT0"
constexpr uint32_t SHM_VERSION = 1;
constexpr uint32_t SHM_SLOT_COUNT = 3;
constexpr uint32_t SHM_MAX_WIDTH = 3840;
constexpr uint32_t SHM_MAX_HEIGHT = 2160;

struct ShmSlotHeader {
    uint64_t sequence;
    uint32_t width;
    uint32_t height;
    uint64_t capturedAtNs;
    uint32_t ready;  // 1 = yeni kare hazır
};

struct ShmRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slotCount;
    uint32_t maxW;
    uint32_t maxH;
    uint64_t writeIndex;
};

// Slot verisi header'dan sonra gelir; ofset hesaplaması:
//   slotStride = sizeof(ShmSlotHeader) + maxW*maxH*3
//   slotOffset(i) = sizeof(ShmRingHeader) + i*slotStride

}  // namespace gft
