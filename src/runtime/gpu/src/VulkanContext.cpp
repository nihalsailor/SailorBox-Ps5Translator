#include "gpu/VulkanContext.hpp"
#include "common/Logger.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sailor {

namespace {

using PFN_vkVoidFunction = void(*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(*)(void* instance, const char* pName);

// Minimal Vulkan structure definitions for dynamic loading
struct VkApplicationInfo {
    u32 sType{1}; // VK_STRUCTURE_TYPE_APPLICATION_INFO
    const void* pNext{nullptr};
    const char* pApplicationName{nullptr};
    u32 applicationVersion{0};
    const char* pEngineName{nullptr};
    u32 engineVersion{0};
    u32 apiVersion{0x00403000}; // Vulkan 1.3
};

struct VkInstanceCreateInfo {
    u32 sType{2}; // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    const void* pNext{nullptr};
    u32 flags{0};
    const VkApplicationInfo* pApplicationInfo{nullptr};
    u32 enabledLayerCount{0};
    const char* const* ppEnabledLayerNames{nullptr};
    u32 enabledExtensionCount{0};
    const char* const* ppEnabledExtensionNames{nullptr};
};

struct VkDeviceQueueCreateInfo {
    u32 sType{3}; // VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO
    const void* pNext{nullptr};
    u32 flags{0};
    u32 queueFamilyIndex{0};
    u32 queueCount{1};
    const float* pQueuePriorities{nullptr};
};

struct VkDeviceCreateInfo {
    u32 sType{4}; // VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
    const void* pNext{nullptr};
    u32 flags{0};
    u32 queueCreateInfoCount{1};
    const VkDeviceQueueCreateInfo* pQueueCreateInfos{nullptr};
    u32 enabledLayerCount{0};
    const char* const* ppEnabledLayerNames{nullptr};
    u32 enabledExtensionCount{0};
    const char* const* ppEnabledExtensionNames{nullptr};
    const void* pEnabledFeatures{nullptr};
};

struct VkQueueFamilyProperties {
    u32 queueFlags;
    u32 queueCount;
    u32 timestampValidBits;
    u32 minImageTransferGranularity[3];
};

using PFN_vkCreateInstance = int(*)(const VkInstanceCreateInfo*, const void*, void**);
using PFN_vkDestroyInstance = void(*)(void*, const void*);
using PFN_vkEnumeratePhysicalDevices = int(*)(void*, u32*, void**);
using PFN_vkGetPhysicalDeviceProperties = void(*)(void*, void*);
using PFN_vkGetPhysicalDeviceQueueFamilyProperties = void(*)(void*, u32*, VkQueueFamilyProperties*);
using PFN_vkCreateDevice = int(*)(void*, const VkDeviceCreateInfo*, const void*, void**);
using PFN_vkDestroyDevice = void(*)(void*, const void*);
using PFN_vkGetDeviceQueue = void(*)(void*, u32, u32, void**);

#ifdef _WIN32
struct VkWin32SurfaceCreateInfoKHR {
    u32 sType{1000009000};
    const void* pNext{nullptr};
    u32 flags{0};
    HINSTANCE hinstance{nullptr};
    HWND hwnd{nullptr};
};
using PFN_vkCreateWin32SurfaceKHR = int(*)(void*, const VkWin32SurfaceCreateInfoKHR*, const void*, void**);
using PFN_vkDestroySurfaceKHR = void(*)(void*, void*, const void*);

LRESULT CALLBACK SailorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CLOSE) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    if (uMsg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        (void)hdc;
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (uMsg == WM_ERASEBKGND) {
        return 1;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
#endif

struct VkSwapchainCreateInfoKHR {
    u32 sType{1000001000};
    const void* pNext{nullptr};
    u32 flags{0};
    void* surface{nullptr};
    u32 minImageCount{2};
    u32 imageFormat{44};
    u32 imageColorSpace{0};
    struct { u32 width; u32 height; } imageExtent{1920, 1080};
    u32 imageArrayLayers{1};
    u32 imageUsage{0x00000010 | 0x00000008 | 0x00000002 | 0x00000001};
    u32 imageSharingMode{0};
    u32 queueFamilyIndexCount{0};
    const u32* pQueueFamilyIndices{nullptr};
    u32 preTransform{1};
    u32 compositeAlpha{1};
    u32 presentMode{0};
    u32 clipped{1};
    void* oldSwapchain{nullptr};
};

struct VkPresentInfoKHR {
    u32 sType{1000001001};
    const void* pNext{nullptr};
    u32 waitSemaphoreCount{0};
    const void* const* pWaitSemaphores{nullptr};
    u32 swapchainCount{1};
    const void* const* pSwapchains{nullptr};
    const u32* pImageIndices{nullptr};
    int* pResults{nullptr};
};

struct VkCommandPoolCreateInfo {
    u32 sType{39};
    const void* pNext{nullptr};
    u32 flags{2};
    u32 queueFamilyIndex{0};
};

struct VkCommandBufferAllocateInfo {
    u32 sType{40};
    const void* pNext{nullptr};
    void* commandPool{nullptr};
    u32 level{0};
    u32 commandBufferCount{1};
};

struct VkSemaphoreCreateInfo {
    u32 sType{9};
    const void* pNext{nullptr};
    u32 flags{0};
};

struct VkSurfaceCapabilitiesKHR {
    u32 minImageCount;
    u32 maxImageCount;
    struct { u32 width; u32 height; } currentExtent;
    struct { u32 width; u32 height; } minImageExtent;
    struct { u32 width; u32 height; } maxImageExtent;
    u32 maxImageArrayLayers;
    u32 supportedTransforms;
    u32 currentTransform;
    u32 supportedCompositeAlpha;
    u32 supportedUsageFlags;
};

using PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = int(*)(void*, void*, VkSurfaceCapabilitiesKHR*);
using PFN_vkCreateSwapchainKHR = int(*)(void*, const VkSwapchainCreateInfoKHR*, const void*, void**);
using PFN_vkDestroySwapchainKHR = void(*)(void*, void*, const void*);
using PFN_vkGetSwapchainImagesKHR = int(*)(void*, void*, u32*, void**);
using PFN_vkAcquireNextImageKHR = int(*)(void*, void*, u64, void*, void*, u32*);
using PFN_vkQueuePresentKHR = int(*)(void*, const VkPresentInfoKHR*);
using PFN_vkCreateCommandPool = int(*)(void*, const VkCommandPoolCreateInfo*, const void*, void**);
using PFN_vkDestroyCommandPool = void(*)(void*, void*, const void*);
using PFN_vkAllocateCommandBuffers = int(*)(void*, const VkCommandBufferAllocateInfo*, void**);
using PFN_vkCreateSemaphore = int(*)(void*, const VkSemaphoreCreateInfo*, const void*, void**);
using PFN_vkDestroySemaphore = void(*)(void*, void*, const void*);

struct VkCommandBufferBeginInfo {
    u32 sType{42};
    const void* pNext{nullptr};
    u32 flags{1}; // VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    const void* pInheritanceInfo{nullptr};
};

struct VkImageSubresourceRange {
    u32 aspectMask{1}; // VK_IMAGE_ASPECT_COLOR_BIT
    u32 baseMipLevel{0};
    u32 levelCount{1};
    u32 baseArrayLayer{0};
    u32 layerCount{1};
};

struct VkImageMemoryBarrier {
    u32 sType{45}; // VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
    const void* pNext{nullptr};
    u32 srcAccessMask{0};
    u32 dstAccessMask{0};
    u32 oldLayout{0}; // VK_IMAGE_LAYOUT_UNDEFINED
    u32 newLayout{1000001002}; // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    u32 srcQueueFamilyIndex{0xFFFFFFFF}; // VK_QUEUE_FAMILY_IGNORED
    u32 dstQueueFamilyIndex{0xFFFFFFFF}; // VK_QUEUE_FAMILY_IGNORED
    void* image{nullptr};
    VkImageSubresourceRange subresourceRange{};
};

struct VkSubmitInfo {
    u32 sType{4}; // VK_STRUCTURE_TYPE_SUBMIT_INFO
    const void* pNext{nullptr};
    u32 waitSemaphoreCount{0};
    const void* const* pWaitSemaphores{nullptr};
    const u32* pWaitDstStageMask{nullptr};
    u32 commandBufferCount{0};
    const void* const* pCommandBuffers{nullptr};
    u32 signalSemaphoreCount{0};
    const void* const* pSignalSemaphores{nullptr};
};

using PFN_vkBeginCommandBuffer = int(*)(void*, const VkCommandBufferBeginInfo*);
using PFN_vkEndCommandBuffer = int(*)(void*);
using PFN_vkCmdPipelineBarrier = void(*)(void*, u32, u32, u32, u32, const void*, u32, const void*, u32, const VkImageMemoryBarrier*);
using PFN_vkQueueSubmit = int(*)(void*, u32, const VkSubmitInfo*, void*);
using PFN_vkQueueWaitIdle = int(*)(void*);

struct VkOffset3D {
    i32 x{0};
    i32 y{0};
    i32 z{0};
};

struct VkExtent3D {
    u32 width{0};
    u32 height{0};
    u32 depth{1};
};

struct VkImageSubresourceLayers {
    u32 aspectMask{1}; // VK_IMAGE_ASPECT_COLOR_BIT
    u32 mipLevel{0};
    u32 baseArrayLayer{0};
    u32 layerCount{1};
};

struct VkBufferImageCopy {
    u64 bufferOffset{0};
    u32 bufferRowLength{0};
    u32 bufferImageHeight{0};
    VkImageSubresourceLayers imageSubresource{};
    VkOffset3D imageOffset{};
    VkExtent3D imageExtent{};
};

struct VkBufferCreateInfo {
    u32 sType{12}; // VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    const void* pNext{nullptr};
    u32 flags{0};
    u64 size{0};
    u32 usage{0x00000001}; // VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    u32 sharingMode{0};
    u32 queueFamilyIndexCount{0};
    const u32* pQueueFamilyIndices{nullptr};
};

struct VkMemoryRequirements {
    u64 size{0};
    u64 alignment{0};
    u32 memoryTypeBits{0};
};

struct VkMemoryAllocateInfo {
    u32 sType{5}; // VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    const void* pNext{nullptr};
    u64 allocationSize{0};
    u32 memoryTypeIndex{0};
};

struct VkMemoryType {
    u32 propertyFlags{0};
    u32 heapIndex{0};
};

struct VkMemoryHeap {
    u64 size{0};
    u32 flags{0};
};

struct VkPhysicalDeviceMemoryProperties {
    u32 memoryTypeCount{0};
    VkMemoryType memoryTypes[32]{};
    u32 memoryHeapCount{0};
    VkMemoryHeap memoryHeaps[16]{};
};

using PFN_vkCreateBuffer = int(*)(void*, const VkBufferCreateInfo*, const void*, void**);
using PFN_vkDestroyBuffer = void(*)(void*, void*, const void*);
using PFN_vkGetBufferMemoryRequirements = void(*)(void*, void*, VkMemoryRequirements*);
using PFN_vkAllocateMemory = int(*)(void*, const VkMemoryAllocateInfo*, const void*, void**);
using PFN_vkFreeMemory = void(*)(void*, void*, const void*);
using PFN_vkBindBufferMemory = int(*)(void*, void*, void*, u64);
using PFN_vkMapMemory = int(*)(void*, void*, u64, u64, u32, void**);
using PFN_vkUnmapMemory = void(*)(void*, void*);
using PFN_vkGetPhysicalDeviceMemoryProperties = void(*)(void*, VkPhysicalDeviceMemoryProperties*);
using PFN_vkCmdCopyBufferToImage = void(*)(void*, void*, void*, u32, u32, const VkBufferImageCopy*);

static bool SafeCopyMemory(void* dst, const void* src, size_t size) {
#ifdef _WIN32
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, src, size);
    return true;
#endif
}

PFN_vkGetInstanceProcAddr g_vkGetInstanceProcAddr = nullptr;

} // namespace

VulkanContext& VulkanContext::Instance() {
    static VulkanContext instance;
    return instance;
}

bool VulkanContext::Initialize(bool enableValidation) {
    if (m_available) return true;

    SB_LOG_INFO("VulkanContext", "Initializing SailorBox Vulkan 1.3 Subsystem...");

    if (!LoadVulkanLibrary()) {
        SB_LOG_WARN("VulkanContext", "Vulkan library (vulkan-1.dll) not loaded; running in headless mode.");
        return false;
    }

    if (!CreateInstance(enableValidation)) {
        SB_LOG_WARN("VulkanContext", "Failed to create Vulkan instance.");
        return false;
    }

    if (!PickPhysicalDevice()) {
        SB_LOG_WARN("VulkanContext", "Failed to find suitable physical device.");
        return false;
    }

    if (!CreateLogicalDevice()) {
        SB_LOG_WARN("VulkanContext", "Failed to create logical device.");
        return false;
    }

    m_available = true;
    SB_LOG_INFO("VulkanContext", "Vulkan initialized successfully! Target GPU: {}", m_deviceInfo.deviceName);

    CreateDisplayWindow(1920, 1080);
    CreateSurface();
    CreateSwapchain(1920, 1080);
    return true;
}

void VulkanContext::Shutdown() {
    DestroySwapchain();

    if (g_vkGetInstanceProcAddr) {
        if (m_device) {
            auto vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
                g_vkGetInstanceProcAddr(m_instance, "vkDestroyDevice"));
            if (vkDestroyDevice) {
                vkDestroyDevice(m_device, nullptr);
            }
            m_device = nullptr;
        }

        if (m_instance) {
            auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
                g_vkGetInstanceProcAddr(m_instance, "vkDestroyInstance"));
            if (vkDestroyInstance) {
                vkDestroyInstance(m_instance, nullptr);
            }
            m_instance = nullptr;
        }
    }

    if (m_vulkanModule) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_vulkanModule));
#endif
        m_vulkanModule = nullptr;
    }
    m_available = false;
}

bool VulkanContext::LoadVulkanLibrary() {
#ifdef _WIN32
    m_vulkanModule = LoadLibraryA("vulkan-1.dll");
    if (!m_vulkanModule) return false;

    g_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(static_cast<HMODULE>(m_vulkanModule), "vkGetInstanceProcAddr"));
    return g_vkGetInstanceProcAddr != nullptr;
#else
    return false;
#endif
}

bool VulkanContext::CreateInstance(bool enableValidation) {
    (void)enableValidation;
    if (!g_vkGetInstanceProcAddr) return false;

    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        g_vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!vkCreateInstance) return false;

    VkApplicationInfo appInfo{};
    appInfo.pApplicationName = "SailorBox Runtime";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "SailorGPU";
    appInfo.engineVersion = 1;
    appInfo.apiVersion = 0x00403000; // Vulkan 1.3

    VkInstanceCreateInfo createInfo{};
    createInfo.pApplicationInfo = &appInfo;

    const char* instanceExts[] = {
        "VK_KHR_surface",
        "VK_KHR_win32_surface"
    };
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = instanceExts;

    int res = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (res != 0 || !m_instance) {
        // Fallback without extensions if surface extension is unavailable
        createInfo.enabledExtensionCount = 0;
        createInfo.ppEnabledExtensionNames = nullptr;
        res = vkCreateInstance(&createInfo, nullptr, &m_instance);
    }
    if (res != 0 || !m_instance) {
        // Fallback to Vulkan 1.1 if driver does not support 1.3 instance
        appInfo.apiVersion = 0x00401000;
        res = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (res != 0 || !m_instance) {
            return false;
        }
    }
    return true;
}

bool VulkanContext::PickPhysicalDevice() {
    if (!m_instance || !g_vkGetInstanceProcAddr) return false;

    auto vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        g_vkGetInstanceProcAddr(m_instance, "vkEnumeratePhysicalDevices"));
    auto vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        g_vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceProperties"));

    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties) return false;

    u32 deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) return false;

    std::vector<void*> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    void* chosenDevice = nullptr;
    VulkanDeviceInfo chosenInfo;

    for (void* dev : devices) {
        alignas(8) u8 propsBuffer[2048] = {0};
        vkGetPhysicalDeviceProperties(dev, propsBuffer);

        u32 apiVersion = *reinterpret_cast<const u32*>(propsBuffer + 0);
        u32 driverVersion = *reinterpret_cast<const u32*>(propsBuffer + 4);
        u32 deviceType = *reinterpret_cast<const u32*>(propsBuffer + 16);
        const char* devName = reinterpret_cast<const char*>(propsBuffer + 20);

        VulkanDeviceInfo info;
        info.deviceName = devName;
        info.apiVersion = apiVersion;
        info.driverVersion = driverVersion;
        info.isDiscreteGpu = (deviceType == 2); // VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU

        // Prefer discrete GPU
        if (!chosenDevice || info.isDiscreteGpu) {
            chosenDevice = dev;
            chosenInfo = info;
            if (info.isDiscreteGpu) break;
        }
    }

    if (!chosenDevice) return false;

    m_physicalDevice = chosenDevice;
    m_deviceInfo = chosenInfo;
    return true;
}

bool VulkanContext::CreateLogicalDevice() {
    if (!m_physicalDevice || !g_vkGetInstanceProcAddr) return false;

    auto vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        g_vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        g_vkGetInstanceProcAddr(m_instance, "vkCreateDevice"));
    auto vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        g_vkGetInstanceProcAddr(m_instance, "vkGetDeviceQueue"));

    if (!vkCreateDevice || !vkGetDeviceQueue) return false;

    u32 queueFamilyCount = 0;
    if (vkGetPhysicalDeviceQueueFamilyProperties) {
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    }

    m_graphicsQueueFamily = 0;
    m_computeQueueFamily = 0;
    u32 computeQueueIndex = 0;
    bool separateComputeFamily = false;

    if (queueFamilyCount > 0 && vkGetPhysicalDeviceQueueFamilyProperties) {
        std::vector<VkQueueFamilyProperties> queueProps(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueProps.data());

        // Find primary graphics queue family
        for (u32 i = 0; i < queueFamilyCount; ++i) {
            // VK_QUEUE_GRAPHICS_BIT = 0x1, VK_QUEUE_COMPUTE_BIT = 0x2
            if (queueProps[i].queueFlags & 0x1) {
                m_graphicsQueueFamily = i;
                break;
            }
        }

        // Prefer a dedicated async compute queue family (compute bit set, graphics bit unset)
        bool foundDedicated = false;
        for (u32 i = 0; i < queueFamilyCount; ++i) {
            if ((queueProps[i].queueFlags & 0x2) && !(queueProps[i].queueFlags & 0x1)) {
                m_computeQueueFamily = i;
                foundDedicated = true;
                separateComputeFamily = true;
                break;
            }
        }

        // If no dedicated compute queue exists, look for any compute-capable family
        if (!foundDedicated) {
            for (u32 i = 0; i < queueFamilyCount; ++i) {
                if (queueProps[i].queueFlags & 0x2) {
                    m_computeQueueFamily = i;
                    if (i != m_graphicsQueueFamily) separateComputeFamily = true;
                    break;
                }
            }
        }

        // If compute shares the graphics family, check if multiple queues are available
        if (!separateComputeFamily) {
            if (queueProps[m_graphicsQueueFamily].queueCount > 1) {
                computeQueueIndex = 1;
            }
        }
    }

    float queuePriorities[2] = {1.0f, 1.0f};
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

    if (separateComputeFamily) {
        VkDeviceQueueCreateInfo gq{};
        gq.queueFamilyIndex = m_graphicsQueueFamily;
        gq.queueCount = 1;
        gq.pQueuePriorities = queuePriorities;
        queueCreateInfos.push_back(gq);

        VkDeviceQueueCreateInfo cq{};
        cq.queueFamilyIndex = m_computeQueueFamily;
        cq.queueCount = 1;
        cq.pQueuePriorities = queuePriorities;
        queueCreateInfos.push_back(cq);
    } else {
        VkDeviceQueueCreateInfo gq{};
        gq.queueFamilyIndex = m_graphicsQueueFamily;
        gq.queueCount = (computeQueueIndex == 1) ? 2 : 1;
        gq.pQueuePriorities = queuePriorities;
        queueCreateInfos.push_back(gq);
    }

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size());
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();

    const char* deviceExts[] = {
        "VK_KHR_swapchain",
        "VK_EXT_external_memory_host"
    };
    deviceCreateInfo.enabledExtensionCount = 2;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExts;

    int res = vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device);
    if (res != 0 || !m_device) {
        // Fallback without extensions
        deviceCreateInfo.enabledExtensionCount = 0;
        deviceCreateInfo.ppEnabledExtensionNames = nullptr;
        res = vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device);
    }
    if (res != 0 || !m_device) {
        SB_LOG_WARN("VulkanContext", "vkCreateDevice failed with code {}", res);
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    if (separateComputeFamily) {
        vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);
        SB_LOG_INFO("VulkanContext", "Vulkan async compute enabled on dedicated queue family {}", m_computeQueueFamily);
    } else if (computeQueueIndex == 1) {
        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 1, &m_computeQueue);
        SB_LOG_INFO("VulkanContext", "Vulkan async compute enabled on secondary queue index 1 (family {})", m_graphicsQueueFamily);
    } else {
        m_computeQueue = m_graphicsQueue;
        SB_LOG_INFO("VulkanContext", "Vulkan async compute multiplexed on primary graphics queue (family {})", m_graphicsQueueFamily);
    }

    CreateComputeResources();
    return true;
}

void* VulkanContext::ImportHostMemory(void* hostPtr, u64 size) {
    if (!m_device || !hostPtr || size == 0) return nullptr;

    struct VkImportMemoryHostPointerInfoEXT {
        u32 sType{1000178000}; // VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT
        const void* pNext{nullptr};
        u32 handleType{0x00000080}; // VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
        void* pHostPointer{nullptr};
    };

    auto vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(
        g_vkGetInstanceProcAddr(m_instance, "vkAllocateMemory"));

    if (!vkAllocateMemory) return hostPtr;

    VkImportMemoryHostPointerInfoEXT hostImportInfo{};
    hostImportInfo.pHostPointer = hostPtr;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.pNext = &hostImportInfo;
    allocInfo.allocationSize = size;
    allocInfo.memoryTypeIndex = 0;

    void* importedDeviceMemory = nullptr;
    int res = vkAllocateMemory(m_device, &allocInfo, nullptr, &importedDeviceMemory);
    if (res != 0) {
        SB_LOG_DEBUG("VulkanContext", "ImportHostMemory returned code {} (using direct host pointer tracking)", res);
        return hostPtr;
    }

    SB_LOG_INFO("VulkanContext", "Imported DirectMemory host pointer {:p} (0x{:X} bytes) -> VkDeviceMemory {:p} (Zero-Copy)",
        hostPtr, size, importedDeviceMemory);
    return importedDeviceMemory;
}

bool VulkanContext::CreateDisplayWindow(u32 width, u32 height) {
#ifdef _WIN32
    if (m_hwnd) return true;

    // Attach calling thread to interactive desktop so window appears on the user's monitor
    HWINSTA hWinSta = OpenWindowStationW(L"WinSta0", FALSE, MAXIMUM_ALLOWED);
    if (hWinSta) {
        SetProcessWindowStation(hWinSta);
    }
    HDESK hDesk = OpenDesktopW(L"default", 0, FALSE, MAXIMUM_ALLOWED);
    if (hDesk) {
        SetThreadDesktop(hDesk);
    }

    // Enable Per-Monitor V2 DPI Awareness to prevent DWM bitmap scaling/clamping
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
        auto pSetDpi = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (pSetDpi) {
            pSetDpi((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
        }

        typedef void (WINAPI *PFN_DisableProcessWindowsGhosting)();
        auto pDisableGhosting = reinterpret_cast<PFN_DisableProcessWindowsGhosting>(
            GetProcAddress(hUser32, "DisableProcessWindowsGhosting"));
        if (pDisableGhosting) {
            pDisableGhosting();
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SailorWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"SailorBoxWindowClass";
    RegisterClassExW(&wc);

    RECT rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
    int winW = rect.right - rect.left;
    int winH = rect.bottom - rect.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW > winW) ? (screenW - winW) / 2 : 0;
    int posY = (screenH > winH) ? (screenH - winH) / 2 : 0;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"SailorBoxWindowClass",
        L"SailorBox - PlayStation 5 Native Runtime",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        posX, posY,
        winW, winH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );

    if (!hwnd) {
        SB_LOG_WARN("VulkanContext", "Failed to create Win32 display window (running headless)");
        return false;
    }

    m_hwnd = static_cast<void*>(hwnd);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SB_LOG_INFO("VulkanContext", "Created Win32 display window: {}x{} (HWND={:p})", width, height, m_hwnd);
    return true;
#else
    (void)width; (void)height;
    return false;
#endif
}

bool VulkanContext::CreateSurface() {
#ifdef _WIN32
    if (m_surface) return true;
    if (!m_instance || !m_hwnd || !g_vkGetInstanceProcAddr) return false;

    auto vkCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        g_vkGetInstanceProcAddr(m_instance, "vkCreateWin32SurfaceKHR"));
    if (!vkCreateWin32SurfaceKHR) return false;

    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.hinstance = GetModuleHandleW(nullptr);
    createInfo.hwnd = static_cast<HWND>(m_hwnd);

    int res = vkCreateWin32SurfaceKHR(m_instance, &createInfo, nullptr, &m_surface);
    if (res != 0 || !m_surface) {
        SB_LOG_WARN("VulkanContext", "vkCreateWin32SurfaceKHR returned code {}", res);
        return false;
    }

    SB_LOG_INFO("VulkanContext", "Created Win32 VkSurfaceKHR {:p} for HWND {:p}", m_surface, m_hwnd);
    return true;
#else
    return false;
#endif
}

bool VulkanContext::CreateSwapchain(u32 width, u32 height) {
    if (m_swapchain) return true;
    if (!m_device || !m_surface || !g_vkGetInstanceProcAddr) return false;

    auto vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        GetProcAddr("vkCreateSwapchainKHR"));
    auto vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        GetProcAddr("vkGetSwapchainImagesKHR"));

    if (!vkCreateSwapchainKHR || !vkGetSwapchainImagesKHR) return false;

    auto vkGetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        g_vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));

    VkSurfaceCapabilitiesKHR caps{};
    u32 swapchainWidth = width ? width : 1920;
    u32 swapchainHeight = height ? height : 1080;
    u32 minImageCount = 3;
    u32 preTransform = 1;
    u32 compositeAlpha = 1;

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR &&
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps) == 0) {
        
        if (caps.currentExtent.width != 0xFFFFFFFF && caps.currentExtent.width > 0) {
            swapchainWidth = caps.currentExtent.width;
            swapchainHeight = caps.currentExtent.height;
        } else {
            if (swapchainWidth < caps.minImageExtent.width) swapchainWidth = caps.minImageExtent.width;
            if (swapchainWidth > caps.maxImageExtent.width) swapchainWidth = caps.maxImageExtent.width;
            if (swapchainHeight < caps.minImageExtent.height) swapchainHeight = caps.minImageExtent.height;
            if (swapchainHeight > caps.maxImageExtent.height) swapchainHeight = caps.maxImageExtent.height;
        }

        if (minImageCount < caps.minImageCount) minImageCount = caps.minImageCount;
        if (caps.maxImageCount > 0 && minImageCount > caps.maxImageCount) minImageCount = caps.maxImageCount;

        if (caps.supportedTransforms & 1) preTransform = 1;
        else preTransform = caps.currentTransform;

        if (caps.supportedCompositeAlpha & 1) compositeAlpha = 1;
        else if (caps.supportedCompositeAlpha & 2) compositeAlpha = 2;
        else if (caps.supportedCompositeAlpha & 4) compositeAlpha = 4;
        else if (caps.supportedCompositeAlpha & 8) compositeAlpha = 8;
    }

    m_swapchainWidth = swapchainWidth;
    m_swapchainHeight = swapchainHeight;

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.surface = m_surface;
    createInfo.minImageCount = minImageCount;
    createInfo.imageFormat = 44; // VK_FORMAT_B8G8R8A8_UNORM
    createInfo.imageColorSpace = 0; // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
    createInfo.imageExtent.width = swapchainWidth;
    createInfo.imageExtent.height = swapchainHeight;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = 0x00000010 | 0x00000008 | 0x00000002 | 0x00000001; // COLOR_ATTACHMENT | STORAGE | TRANSFER_DST | TRANSFER_SRC
    createInfo.imageSharingMode = 0;
    createInfo.preTransform = preTransform;
    createInfo.compositeAlpha = compositeAlpha;
    createInfo.presentMode = 2; // VK_PRESENT_MODE_FIFO_KHR (guaranteed supported)
    createInfo.clipped = 1;

    int res = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain);
    if (res != 0 || !m_swapchain) {
        SB_LOG_WARN("VulkanContext", "vkCreateSwapchainKHR returned code {}", res);
        return false;
    }

    u32 imageCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    if (imageCount > 0) {
        m_swapchainImages.resize(imageCount, nullptr);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());
    }

    auto vkCreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
        GetProcAddr("vkCreateSemaphore"));
    auto vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
        GetProcAddr("vkCreateCommandPool"));
    auto vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        GetProcAddr("vkAllocateCommandBuffers"));

    if (vkCreateSemaphore) {
        VkSemaphoreCreateInfo semInfo{};
        if (!m_acquireSemaphore) vkCreateSemaphore(m_device, &semInfo, nullptr, &m_acquireSemaphore);
        if (!m_renderSemaphore) vkCreateSemaphore(m_device, &semInfo, nullptr, &m_renderSemaphore);
    }

    if (vkCreateCommandPool && vkAllocateCommandBuffers) {
        if (!m_commandPool) {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.flags = 0x00000002; // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
            poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
            if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) == 0 && m_commandPool) {
                VkCommandBufferAllocateInfo allocInfo{};
                allocInfo.commandPool = m_commandPool;
                allocInfo.commandBufferCount = 1;
                vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer);
            }
        }
    }

    SB_LOG_INFO("VulkanContext", "Created VkSwapchainKHR {:p} with {} images ({}x{})",
        m_swapchain, m_swapchainImages.size(), swapchainWidth, swapchainHeight);
    return true;
}

void VulkanContext::RecreateSwapchain(u32 width, u32 height) {
    if (!m_device || !m_surface || !g_vkGetInstanceProcAddr) return;

    std::lock_guard<std::recursive_mutex> lock(m_submitMutex);

    auto vkDeviceWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(
        GetProcAddr("vkDeviceWaitIdle"));
    if (vkDeviceWaitIdle) {
        vkDeviceWaitIdle(m_device);
    }

    if (m_swapchain) {
        auto vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            GetProcAddr("vkDestroySwapchainKHR"));
        if (vkDestroySwapchainKHR) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        }
        m_swapchain = nullptr;
        m_swapchainImages.clear();
    }

    CreateSwapchain(width, height);
}

bool VulkanContext::EnsureStagingBuffer(u64 requiredSize) {
    if (m_stagingBuffer && m_stagingMemory && m_stagingBufferSize >= requiredSize) {
        return true;
    }

    if (!m_device) return false;

    auto vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(GetProcAddr("vkCreateBuffer"));
    auto vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(GetProcAddr("vkDestroyBuffer"));
    auto vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(GetProcAddr("vkGetBufferMemoryRequirements"));
    auto vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(GetProcAddr("vkAllocateMemory"));
    auto vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(GetProcAddr("vkFreeMemory"));
    auto vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(GetProcAddr("vkBindBufferMemory"));
    auto vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(GetProcAddr("vkGetPhysicalDeviceMemoryProperties"));

    if (!vkCreateBuffer || !vkGetBufferMemoryRequirements || !vkAllocateMemory || !vkBindBufferMemory || !vkGetPhysicalDeviceMemoryProperties) {
        SB_LOG_ERROR("VulkanContext", "EnsureStagingBuffer: failed to resolve required Vulkan function pointers!");
        return false;
    }

    if (m_stagingBuffer) {
        if (vkDestroyBuffer) vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        if (vkFreeMemory && m_stagingMemory) vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingBuffer = nullptr;
        m_stagingMemory = nullptr;
        m_stagingBufferSize = 0;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.size = requiredSize;
    bufferInfo.usage = 0x00000001; // VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    int bRes = vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_stagingBuffer);
    if (bRes != 0 || !m_stagingBuffer) {
        SB_LOG_ERROR("VulkanContext", "EnsureStagingBuffer: vkCreateBuffer failed (code {})", bRes);
        return false;
    }

    VkMemoryRequirements memReqs{};
    vkGetBufferMemoryRequirements(m_device, m_stagingBuffer, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

    u32 memoryTypeIndex = 0;
    bool found = false;
    // Look for HOST_VISIBLE (0x02) and HOST_COHERENT (0x04)
    u32 desiredFlags = 0x00000002 | 0x00000004;
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & desiredFlags) == desiredFlags) {
            memoryTypeIndex = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReqs.memoryTypeBits & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & 0x00000002)) {
                memoryTypeIndex = i;
                found = true;
                break;
            }
        }
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    int aRes = vkAllocateMemory(m_device, &allocInfo, nullptr, &m_stagingMemory);
    if (aRes != 0 || !m_stagingMemory) {
        SB_LOG_ERROR("VulkanContext", "EnsureStagingBuffer: vkAllocateMemory failed (code {})", aRes);
        if (vkDestroyBuffer) vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = nullptr;
        return false;
    }

    int bindRes = vkBindBufferMemory(m_device, m_stagingBuffer, m_stagingMemory, 0);
    if (bindRes != 0) {
        SB_LOG_ERROR("VulkanContext", "EnsureStagingBuffer: vkBindBufferMemory failed (code {})", bindRes);
        if (vkDestroyBuffer) vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        if (vkFreeMemory) vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingBuffer = nullptr;
        m_stagingMemory = nullptr;
        return false;
    }

    m_stagingBufferSize = requiredSize;
    SB_LOG_INFO("VulkanContext", "EnsureStagingBuffer: successfully created staging buffer size {} (memType={})", requiredSize, memoryTypeIndex);
    return true;
}

bool VulkanContext::CreateComputeResources() {
    if (!m_device || !m_instance || !g_vkGetInstanceProcAddr) return false;
    if (m_computeCommandPool && m_computeCommandBuffer) return true;

    auto vkCreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
        GetProcAddr("vkCreateSemaphore"));
    auto vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
        GetProcAddr("vkCreateCommandPool"));
    auto vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        GetProcAddr("vkAllocateCommandBuffers"));

    if (vkCreateSemaphore && !m_computeSemaphore) {
        VkSemaphoreCreateInfo semInfo{};
        vkCreateSemaphore(m_device, &semInfo, nullptr, &m_computeSemaphore);
    }

    if (vkCreateCommandPool && vkAllocateCommandBuffers && !m_computeCommandPool) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.flags = 0x00000002; // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
        poolInfo.queueFamilyIndex = m_computeQueueFamily;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_computeCommandPool) == 0 && m_computeCommandPool) {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = m_computeCommandPool;
            allocInfo.commandBufferCount = 1;
            vkAllocateCommandBuffers(m_device, &allocInfo, &m_computeCommandBuffer);
        }
    }

    SB_LOG_INFO("VulkanContext", "Initialized Vulkan compute resources: pool={:p}, cmdbuf={:p}, sem={:p}",
        m_computeCommandPool, m_computeCommandBuffer, m_computeSemaphore);
    return true;
}

bool VulkanContext::SubmitComputeCommandBuffer(void* cmdBuffer, bool signalSemaphore) {
    if (!m_device || !m_computeQueue || !g_vkGetInstanceProcAddr) return false;

    void* cb = cmdBuffer ? cmdBuffer : m_computeCommandBuffer;
    if (!cb) return false;

    auto vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
        g_vkGetInstanceProcAddr(m_instance, "vkQueueSubmit"));
    if (!vkQueueSubmit) return false;

    VkSubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cb;

    if (signalSemaphore && m_computeSemaphore) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &m_computeSemaphore;
        m_computeWorkPending.store(true, std::memory_order_release);
    }

    int res = vkQueueSubmit(m_computeQueue, 1, &submitInfo, nullptr);
    if (res != 0) {
        SB_LOG_WARN("VulkanContext", "vkQueueSubmit on compute queue returned error {}", res);
        return false;
    }
    return true;
}

bool VulkanContext::WaitComputeIdle() {
    if (!m_computeQueue || !g_vkGetInstanceProcAddr) return false;
    auto vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(
        g_vkGetInstanceProcAddr(m_instance, "vkQueueWaitIdle"));
    if (vkQueueWaitIdle) {
        return vkQueueWaitIdle(m_computeQueue) == 0;
    }
    return false;
}

void VulkanContext::DestroySwapchain() {
    if (!g_vkGetInstanceProcAddr) return;

    if (m_computeCommandPool && m_device) {
        auto vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
            g_vkGetInstanceProcAddr(m_instance, "vkDestroyCommandPool"));
        if (vkDestroyCommandPool) vkDestroyCommandPool(m_device, m_computeCommandPool, nullptr);
        m_computeCommandPool = nullptr;
        m_computeCommandBuffer = nullptr;
    }

    if (m_computeSemaphore && m_device) {
        auto vkDestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
            g_vkGetInstanceProcAddr(m_instance, "vkDestroySemaphore"));
        if (vkDestroySemaphore) vkDestroySemaphore(m_device, m_computeSemaphore, nullptr);
        m_computeSemaphore = nullptr;
    }

    if (m_stagingBuffer && m_device) {
        auto vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(
            g_vkGetInstanceProcAddr(m_instance, "vkDestroyBuffer"));
        auto vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(
            g_vkGetInstanceProcAddr(m_instance, "vkFreeMemory"));
        if (vkDestroyBuffer) vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        if (vkFreeMemory && m_stagingMemory) vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingBuffer = nullptr;
        m_stagingMemory = nullptr;
        m_stagingBufferSize = 0;
    }

    if (m_commandPool && m_device) {
        auto vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
            g_vkGetInstanceProcAddr(m_instance, "vkDestroyCommandPool"));
        if (vkDestroyCommandPool) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = nullptr;
        m_commandBuffer = nullptr;
    }

    if (m_acquireSemaphore && m_device) {
        auto vkDestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
            g_vkGetInstanceProcAddr(m_instance, "vkDestroySemaphore"));
        if (vkDestroySemaphore) {
            vkDestroySemaphore(m_device, m_acquireSemaphore, nullptr);
            if (m_renderSemaphore) vkDestroySemaphore(m_device, m_renderSemaphore, nullptr);
        }
        m_acquireSemaphore = nullptr;
        m_renderSemaphore = nullptr;
    }

    if (m_swapchain && m_device) {
        auto vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            g_vkGetInstanceProcAddr(m_instance, "vkDestroySwapchainKHR"));
        if (vkDestroySwapchainKHR) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = nullptr;
        m_swapchainImages.clear();
    }

    if (m_surface && m_instance) {
        auto vkDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            g_vkGetInstanceProcAddr(m_instance, "vkDestroySurfaceKHR"));
        if (vkDestroySurfaceKHR) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = nullptr;
    }

#ifdef _WIN32
    if (m_hwnd) {
        DestroyWindow(static_cast<HWND>(m_hwnd));
        m_hwnd = nullptr;
    }
#endif
}

void VulkanContext::PumpWindowMessages() {
#ifdef _WIN32
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#endif
}

void VulkanContext::SubmitFrame(void* hostFramebufferPtr, u32 width, u32 height) {
    if (!m_available) return;

    std::lock_guard<std::recursive_mutex> lock(m_submitMutex);

    PumpWindowMessages();

    if (!m_swapchain && m_device) {
        if (CreateDisplayWindow(width, height)) {
            CreateSurface();
            CreateSwapchain(width, height);
        }
    }

    if (m_swapchain && g_vkGetInstanceProcAddr) {
        auto vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
            g_vkGetInstanceProcAddr(m_instance, "vkAcquireNextImageKHR"));
        auto vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
            g_vkGetInstanceProcAddr(m_instance, "vkQueuePresentKHR"));

        if (vkAcquireNextImageKHR && vkQueuePresentKHR) {
            u32 imageIndex = 0;
            int acqRes = vkAcquireNextImageKHR(m_device, m_swapchain, 1000000000ULL, m_acquireSemaphore, nullptr, &imageIndex);
            
            static int s_loggedFbSamples = 0;
            if (hostFramebufferPtr && s_loggedFbSamples < 10) {
                char dbg[512];
                int dbgLen = snprintf(dbg, sizeof(dbg),
                    "[VULKAN SUBMIT DIAG] acqRes=%d, imgIdx=%u, swapImages=%llu, cmdBuf=%p, fb=%p\n",
                    acqRes, imageIndex, (unsigned long long)m_swapchainImages.size(), m_commandBuffer, hostFramebufferPtr);
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD written = 0;
                WriteFile(hOut, dbg, (DWORD)dbgLen, &written, NULL);
            }

            if (acqRes == -1000001004 /* VK_ERROR_OUT_OF_DATE_KHR */ || acqRes == 1000001003 /* VK_SUBOPTIMAL_KHR */) {
                RecreateSwapchain(width, height);
                return;
            }

            if (acqRes == 0 && imageIndex < m_swapchainImages.size()) {
                auto vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(GetProcAddr("vkBeginCommandBuffer"));
                auto vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(GetProcAddr("vkCmdPipelineBarrier"));
                auto vkCmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(GetProcAddr("vkCmdCopyBufferToImage"));
                auto vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(GetProcAddr("vkEndCommandBuffer"));
                auto vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(GetProcAddr("vkQueueSubmit"));
                auto vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(GetProcAddr("vkQueueWaitIdle"));

                if (m_commandBuffer && vkBeginCommandBuffer && vkCmdPipelineBarrier && vkEndCommandBuffer && vkQueueSubmit) {
                    VkCommandBufferBeginInfo beginInfo{};
                    beginInfo.flags = 1; // VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                    int bRes = vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
                    if (bRes != 0) {
                        SB_LOG_ERROR("VulkanContext", "vkBeginCommandBuffer failed (code {})", bRes);
                    }

                    u64 fbSize = static_cast<u64>(width) * height * 4;
                    bool uploaded = false;
                    if (hostFramebufferPtr && fbSize > 0) {
                        if (EnsureStagingBuffer(fbSize)) {
                            auto vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(GetProcAddr("vkMapMemory"));
                            auto vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(GetProcAddr("vkUnmapMemory"));
                            if (vkMapMemory && vkUnmapMemory) {
                                void* mappedData = nullptr;
                                if (vkMapMemory(m_device, m_stagingMemory, 0, fbSize, 0, &mappedData) == 0 && mappedData) {
                                    uploaded = SafeCopyMemory(mappedData, hostFramebufferPtr, fbSize);
                                    static uint64_t s_frameCounter = 0;
                                    s_frameCounter++;
                                    bool shouldSample = (s_frameCounter <= 5) || ((s_frameCounter % 60) == 0);
                                    if (shouldSample) {
                                        const uint32_t* pWords = reinterpret_cast<const uint32_t*>(mappedData);
                                        uint32_t nonZeroCount = 0;
                                        uint32_t firstNonZero = 0;
                                        for (size_t i = 0; i < (fbSize / 4); i += 16) {
                                            if (pWords[i] != 0) {
                                                nonZeroCount++;
                                                if (firstNonZero == 0) firstNonZero = pWords[i];
                                            }
                                        }
                                        SB_LOG_INFO("VulkanContext",
                                            "[VULKAN FB SAMPLE frame {}] fb={:p}, uploaded={}, nonZeroSamples={}/{} (first=0x{:08X})",
                                            s_frameCounter, hostFramebufferPtr, uploaded, nonZeroCount, ((fbSize / 4) / 16), firstNonZero);
                                    }
                                    vkUnmapMemory(m_device, m_stagingMemory);
                                }
                            }
                        } else {
                            static int s_stgFail = 0;
                            if (s_stgFail++ < 3) {
                                SB_LOG_ERROR("VulkanContext", "[VULKAN FB DIAG] EnsureStagingBuffer FAILED for size {}", fbSize);
                            }
                        }
                    }

                    if (uploaded && vkCmdCopyBufferToImage) {
                        // 1. Transition undefined -> transfer dst optimal
                        VkImageMemoryBarrier barrier1{};
                        barrier1.oldLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED
                        barrier1.newLayout = 7; // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                        barrier1.srcAccessMask = 0;
                        barrier1.dstAccessMask = 0x00000800; // VK_ACCESS_TRANSFER_WRITE_BIT
                        barrier1.srcQueueFamilyIndex = 0xFFFFFFFF;
                        barrier1.dstQueueFamilyIndex = 0xFFFFFFFF;
                        barrier1.image = m_swapchainImages[imageIndex];
                        barrier1.subresourceRange.aspectMask = 1; // VK_IMAGE_ASPECT_COLOR_BIT
                        barrier1.subresourceRange.baseMipLevel = 0;
                        barrier1.subresourceRange.levelCount = 1;
                        barrier1.subresourceRange.baseArrayLayer = 0;
                        barrier1.subresourceRange.layerCount = 1;
                        vkCmdPipelineBarrier(m_commandBuffer, 0x00010000, 0x00001000, 0, 0, nullptr, 0, nullptr, 1, &barrier1);

                        // 2. Copy buffer to image
                        u32 copyWidth = (m_swapchainWidth < width) ? m_swapchainWidth : width;
                        u32 copyHeight = (m_swapchainHeight < height) ? m_swapchainHeight : height;

                        VkBufferImageCopy region{};
                        region.bufferOffset = 0;
                        region.bufferRowLength = width;
                        region.bufferImageHeight = height;
                        region.imageSubresource.aspectMask = 1;
                        region.imageSubresource.mipLevel = 0;
                        region.imageSubresource.baseArrayLayer = 0;
                        region.imageSubresource.layerCount = 1;
                        region.imageOffset = {0, 0, 0};
                        region.imageExtent = {copyWidth, copyHeight, 1};
                        vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, m_swapchainImages[imageIndex], 7, 1, &region);

                        // 3. Transition transfer dst optimal -> present src
                        VkImageMemoryBarrier barrier2{};
                        barrier2.oldLayout = 7; // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                        barrier2.newLayout = 1000001002; // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                        barrier2.srcAccessMask = 0x00000800; // VK_ACCESS_TRANSFER_WRITE_BIT
                        barrier2.dstAccessMask = 0;
                        barrier2.srcQueueFamilyIndex = 0xFFFFFFFF;
                        barrier2.dstQueueFamilyIndex = 0xFFFFFFFF;
                        barrier2.image = m_swapchainImages[imageIndex];
                        barrier2.subresourceRange.aspectMask = 1;
                        barrier2.subresourceRange.baseMipLevel = 0;
                        barrier2.subresourceRange.levelCount = 1;
                        barrier2.subresourceRange.baseArrayLayer = 0;
                        barrier2.subresourceRange.layerCount = 1;
                        vkCmdPipelineBarrier(m_commandBuffer, 0x00001000, 0x00010000, 0, 0, nullptr, 0, nullptr, 1, &barrier2);
                    } else {
                        VkImageMemoryBarrier barrier{};
                        barrier.oldLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED
                        barrier.newLayout = 1000001002; // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                        barrier.srcAccessMask = 0;
                        barrier.dstAccessMask = 0;
                        barrier.srcQueueFamilyIndex = 0xFFFFFFFF;
                        barrier.dstQueueFamilyIndex = 0xFFFFFFFF;
                        barrier.image = m_swapchainImages[imageIndex];
                        barrier.subresourceRange.aspectMask = 1; // VK_IMAGE_ASPECT_COLOR_BIT
                        barrier.subresourceRange.baseMipLevel = 0;
                        barrier.subresourceRange.levelCount = 1;
                        barrier.subresourceRange.baseArrayLayer = 0;
                        barrier.subresourceRange.layerCount = 1;
                        vkCmdPipelineBarrier(m_commandBuffer, 0x00010000, 0x00010000, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                    }

                    int endRes = vkEndCommandBuffer(m_commandBuffer);
                    if (endRes != 0) {
                        SB_LOG_ERROR("VulkanContext", "vkEndCommandBuffer failed (code {})", endRes);
                    }

                    void* waitSemaphores[2] = { nullptr, nullptr };
                    u32 waitDstStages[2] = { 0x00010000, 0x00010000 };
                    u32 waitCount = 0;

                    if (m_acquireSemaphore) {
                        waitSemaphores[waitCount] = m_acquireSemaphore;
                        waitDstStages[waitCount] = 0x00010000; // VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                        waitCount++;
                    }

                    if (m_computeWorkPending.exchange(false, std::memory_order_acq_rel) && m_computeSemaphore) {
                        waitSemaphores[waitCount] = m_computeSemaphore;
                        waitDstStages[waitCount] = 0x00010000;
                        waitCount++;
                    }

                    VkSubmitInfo submitInfo{};
                    if (waitCount > 0) {
                        submitInfo.waitSemaphoreCount = waitCount;
                        submitInfo.pWaitSemaphores = waitSemaphores;
                        submitInfo.pWaitDstStageMask = waitDstStages;
                    }
                    submitInfo.commandBufferCount = 1;
                    submitInfo.pCommandBuffers = &m_commandBuffer;
                    if (m_renderSemaphore) {
                        submitInfo.signalSemaphoreCount = 1;
                        submitInfo.pSignalSemaphores = &m_renderSemaphore;
                    }

                    int qRes = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, nullptr);
                    if (qRes != 0) {
                        SB_LOG_ERROR("VulkanContext", "vkQueueSubmit failed (code {})", qRes);
                    }

                    VkPresentInfoKHR presentInfo{};
                    presentInfo.swapchainCount = 1;
                    presentInfo.pSwapchains = &m_swapchain;
                    presentInfo.pImageIndices = &imageIndex;
                    void* waitSem = m_renderSemaphore ? m_renderSemaphore : m_acquireSemaphore;
                    if (waitSem) {
                        presentInfo.waitSemaphoreCount = 1;
                        presentInfo.pWaitSemaphores = &waitSem;
                    }
                    int presRes = vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
                    if (presRes == -1000001004 /* VK_ERROR_OUT_OF_DATE_KHR */ || presRes == 1000001003 /* VK_SUBOPTIMAL_KHR */) {
                        RecreateSwapchain(width, height);
                    } else if (presRes != 0) {
                        SB_LOG_ERROR("VulkanContext", "vkQueuePresentKHR failed (code {})", presRes);
                    }

                    if (vkQueueWaitIdle) {
                        vkQueueWaitIdle(m_graphicsQueue);
                    }
                } else {
                    VkPresentInfoKHR presentInfo{};
                    presentInfo.swapchainCount = 1;
                    presentInfo.pSwapchains = &m_swapchain;
                    presentInfo.pImageIndices = &imageIndex;
                    if (m_acquireSemaphore) {
                        presentInfo.waitSemaphoreCount = 1;
                        presentInfo.pWaitSemaphores = &m_acquireSemaphore;
                    }
                    int presRes = vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
                    if (presRes == -1000001004 /* VK_ERROR_OUT_OF_DATE_KHR */ || presRes == 1000001003 /* VK_SUBOPTIMAL_KHR */) {
                        RecreateSwapchain(width, height);
                    }
                }
            }
        }
    }

    SB_LOG_INFO("VulkanContext", "SubmitFrame: Presented guest framebuffer {:p} ({}x{}) to Vulkan swapchain",
        hostFramebufferPtr, width, height);
}

void* VulkanContext::GetProcAddr(const char* name) const {
    if (!g_vkGetInstanceProcAddr || !name) return nullptr;
    if (m_device) {
        auto vkGetDeviceProcAddr = reinterpret_cast<PFN_vkVoidFunction(*)(void*, const char*)>(
            g_vkGetInstanceProcAddr(m_instance, "vkGetDeviceProcAddr"));
        if (vkGetDeviceProcAddr) {
            void* p = reinterpret_cast<void*>(vkGetDeviceProcAddr(m_device, name));
            if (p) return p;
        }
    }
    return reinterpret_cast<void*>(g_vkGetInstanceProcAddr(m_instance, name));
}

} // namespace Sailor
