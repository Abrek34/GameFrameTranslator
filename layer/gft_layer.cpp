// GameFrame Translator - Vulkan capture/composite layer (Faz 2 başlangıcı).
//
// Bu sürüm: minimal, güvenli pass-through layer iskeleti.
//  - Loader negotiation + instance/device dispatch zinciri
//  - vkQueuePresentKHR hook noktası (şu an sadece pass-through + sayaç)
// Sonraki adımlar:
//  - Swapchain image'larını takip et
//  - Sunulmuş kareyi shm'e aktar (OCR/çeviri pipeline'ına)
//  - Çeviri overlay'ini kompozitleyip present et

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

std::mutex g_mu;

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties = nullptr;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkQueuePresentKHR queuePresentKHR = nullptr;
};

std::unordered_map<VkInstance, InstanceData> g_instances;
std::unordered_map<VkDevice, DeviceData> g_devices;
std::unordered_map<VkQueue, VkDevice> g_queue_device;
std::atomic<uint64_t> g_present_count{0};

inline void* key(VkInstance i) { return reinterpret_cast<void*>(i); }
inline void* key(VkDevice d) { return reinterpret_cast<void*>(d); }

VkLayerInstanceCreateInfo* get_instance_chain_info(const VkInstanceCreateInfo* pCreateInfo, VkLayerFunction func) {
    auto* chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext);
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == func)) {
        chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext);
    }
    return const_cast<VkLayerInstanceCreateInfo*>(chain);
}

VkLayerDeviceCreateInfo* get_device_chain_info(const VkDeviceCreateInfo* pCreateInfo, VkLayerFunction func) {
    auto* chain = reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext);
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == func)) {
        chain = reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain->pNext);
    }
    return const_cast<VkLayerDeviceCreateInfo*>(chain);
}

PFN_vkVoidFunction get_instance_proc(VkInstance instance, const char* name) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_instances.find(instance);
    if (it == g_instances.end() || !it->second.nextGetInstanceProcAddr) return nullptr;
    return it->second.nextGetInstanceProcAddr(instance, name);
}

PFN_vkVoidFunction get_device_proc(VkDevice device, const char* name) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_devices.find(device);
    if (it == g_devices.end() || !it->second.nextGetDeviceProcAddr) return nullptr;
    return it->second.nextGetDeviceProcAddr(device, name);
}

// --- Hooked functions ---

VKAPI_ATTR VkResult VKAPI_CALL gft_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {

    auto* chain_info = get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!chain_info || !chain_info->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (!fpGetInstanceProcAddr) return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fpGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!fpCreateInstance) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS || !pInstance || !*pInstance) return result;

    InstanceData data{};
    data.instance = *pInstance;
    data.nextGetInstanceProcAddr = fpGetInstanceProcAddr;
    data.destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance"));

    std::lock_guard<std::mutex> lock(g_mu);
    g_instances[*pInstance] = data;
    return result;
}

VKAPI_ATTR void VKAPI_CALL gft_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyInstance destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) {
            destroy = it->second.destroyInstance;
            g_instances.erase(it);
        }
    }
    if (destroy) destroy(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL gft_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {

    auto* chain_info = get_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!chain_info || !chain_info->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!fpGetInstanceProcAddr || !fpGetDeviceProcAddr) return VK_ERROR_INITIALIZATION_FAILED;

    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    auto fpCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice"));
    // Bazı loader'larda instance üzerinden alınır; fallback:
    // (instance bilgisi burada doğrudan yok; chain üzerinden alınan fp doğru olmalı)
    if (!fpCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS || !pDevice || !*pDevice) return result;

    DeviceData data{};
    data.device = *pDevice;
    data.nextGetDeviceProcAddr = fpGetDeviceProcAddr;
    data.destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(fpGetDeviceProcAddr(*pDevice, "vkDestroyDevice"));
    data.queuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(fpGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR"));

    std::lock_guard<std::mutex> lock(g_mu);
    g_devices[*pDevice] = data;

    // Queue -> device eşlemesi (present hook için)
    if (pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const auto& qi = pCreateInfo->pQueueCreateInfos[i];
            for (uint32_t j = 0; j < qi.queueCount; ++j) {
                VkQueue q = VK_NULL_HANDLE;
                auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(fpGetDeviceProcAddr(*pDevice, "vkGetDeviceQueue"));
                if (getQueue) {
                    getQueue(*pDevice, qi.queueFamilyIndex, j, &q);
                    if (q) g_queue_device[q] = *pDevice;
                }
            }
        }
    }

    return result;
}

VKAPI_ATTR void VKAPI_CALL gft_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyDevice destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        // queue map temizliği
        for (auto it = g_queue_device.begin(); it != g_queue_device.end(); ) {
            if (it->second == device) it = g_queue_device.erase(it); else ++it;
        }
        auto it = g_devices.find(device);
        if (it != g_devices.end()) {
            destroy = it->second.destroyDevice;
            g_devices.erase(it);
        }
    }
    if (destroy) destroy(device, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL gft_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    PFN_vkQueuePresentKHR next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto qit = g_queue_device.find(queue);
        if (qit != g_queue_device.end()) {
            auto dit = g_devices.find(qit->second);
            if (dit != g_devices.end()) next = dit->second.queuePresentKHR;
        }
    }

    // Enjeksiyon noktası (present öncesi):
    // TODO: swapchain image -> readback -> shm (OCR/çeviri)
    // TODO: overlay bitmap/shm -> image üzerine kompozit
    uint64_t n = ++g_present_count;
    if ((n & 0x3FFu) == 0u) {
        std::fprintf(stderr, "[GFT] vkQueuePresentKHR pass-through, count=%llu\n",
                     static_cast<unsigned long long>(n));
    }

    if (next) return next(queue, pPresentInfo);
    return VK_ERROR_DEVICE_LOST;
}

// --- Dispatch ---

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gft_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_GetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_DestroyDevice);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_QueuePresentKHR);
    // Gelecek: vkCreateSwapchainKHR, vkGetDeviceQueue*, vkAcquireNextImageKHR hook'ları
    return get_device_proc(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gft_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_GetInstanceProcAddr);
    if (std::strcmp(pName, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_CreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_DestroyInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_CreateDevice);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gft_GetDeviceProcAddr);
    return get_instance_proc(instance, pName);
}

} // namespace

// Loader negotiation (exported)
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = gft_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = gft_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return gft_GetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return gft_GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) { *pPropertyCount = 1; return VK_SUCCESS; }
    if (*pPropertyCount < 1) return VK_INCOMPLETE;
    std::memset(pProperties, 0, sizeof(VkLayerProperties));
    std::strncpy(pProperties[0].layerName, "VK_LAYER_GFT_capture", VK_MAX_EXTENSION_NAME_SIZE - 1);
    std::strncpy(pProperties[0].description, "GameFrame Translator capture/composite layer", VK_MAX_DESCRIPTION_SIZE - 1);
    pProperties[0].specVersion = VK_API_VERSION_1_3;
    pProperties[0].implementationVersion = 1;
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*, uint32_t* pPropertyCount, VkExtensionProperties*) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t* pPropertyCount, VkExtensionProperties*) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

} // extern "C"
