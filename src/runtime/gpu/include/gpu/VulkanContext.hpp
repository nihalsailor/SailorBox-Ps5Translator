#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace Sailor {

struct VulkanDeviceInfo {
    std::string deviceName;
    u32 apiVersion{0};
    u32 driverVersion{0};
    bool isDiscreteGpu{false};
};

class VulkanContext {
public:
    static VulkanContext& Instance();

    bool Initialize(bool enableValidation = false);
    void Shutdown();

    bool IsAvailable() const { return m_available; }
    const VulkanDeviceInfo& GetDeviceInfo() const { return m_deviceInfo; }

    void* GetInstance() const { return m_instance; }
    void* GetPhysicalDevice() const { return m_physicalDevice; }
    void* GetDevice() const { return m_device; }
    void* GetGraphicsQueue() const { return m_graphicsQueue; }
    void* GetComputeQueue() const { return m_computeQueue; }
    u32 GetGraphicsQueueFamily() const { return m_graphicsQueueFamily; }
    u32 GetComputeQueueFamily() const { return m_computeQueueFamily; }
    bool HasDedicatedComputeQueue() const { return m_computeQueueFamily != m_graphicsQueueFamily; }

    void* ImportHostMemory(void* hostPtr, u64 size);
    void SubmitFrame(void* hostFramebufferPtr = nullptr, u32 width = 1920, u32 height = 1080);

    bool CreateDisplayWindow(u32 width = 1920, u32 height = 1080);
    bool CreateSurface();
    bool CreateSwapchain(u32 width = 1920, u32 height = 1080);
    void RecreateSwapchain(u32 width = 1920, u32 height = 1080);
    void DestroySwapchain();
    void PumpWindowMessages();

    void* GetHwnd() const { return m_hwnd; }
    void* GetSurface() const { return m_surface; }
    void* GetSwapchain() const { return m_swapchain; }
    void* GetCommandBuffer() const { return m_commandBuffer; }
    void* GetComputeCommandBuffer() const { return m_computeCommandBuffer; }
    void* GetComputeSemaphore() const { return m_computeSemaphore; }
    void* GetProcAddr(const char* name) const;

    bool SubmitComputeCommandBuffer(void* cmdBuffer = nullptr, bool signalSemaphore = true);
    bool WaitComputeIdle();
    void SignalComputePending() { m_computeWorkPending = true; }

private:
    VulkanContext() = default;

    bool LoadVulkanLibrary();
    bool CreateInstance(bool enableValidation);
    bool PickPhysicalDevice();
    bool CreateLogicalDevice();
    bool EnsureStagingBuffer(u64 requiredSize);
    bool CreateComputeResources();

    bool m_available{false};
    void* m_vulkanModule{nullptr};
    void* m_instance{nullptr};
    void* m_physicalDevice{nullptr};
    void* m_device{nullptr};
    void* m_graphicsQueue{nullptr};
    void* m_computeQueue{nullptr};
    u32 m_graphicsQueueFamily{0};
    u32 m_computeQueueFamily{0};
    VulkanDeviceInfo m_deviceInfo;

    void* m_hwnd{nullptr};
    void* m_surface{nullptr};
    void* m_swapchain{nullptr};
    u32 m_swapchainWidth{1920};
    u32 m_swapchainHeight{1080};
    std::vector<void*> m_swapchainImages;
    void* m_commandPool{nullptr};
    void* m_commandBuffer{nullptr};
    void* m_computeCommandPool{nullptr};
    void* m_computeCommandBuffer{nullptr};
    void* m_acquireSemaphore{nullptr};
    void* m_renderSemaphore{nullptr};
    void* m_computeSemaphore{nullptr};
    std::atomic<bool> m_computeWorkPending{false};
    void* m_stagingBuffer{nullptr};
    void* m_stagingMemory{nullptr};
    u64 m_stagingBufferSize{0};
    mutable std::recursive_mutex m_submitMutex;
};

} // namespace Sailor
