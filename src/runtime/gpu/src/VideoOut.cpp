#include "gpu/VideoOut.hpp"
#include "gpu/VulkanContext.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif

namespace Sailor {

VideoOutManager& VideoOutManager::Instance() {
    static VideoOutManager instance;
    return instance;
}

i32 VideoOutManager::Open(i32 userId, i32 type, i32 index, const void* param) {
    (void)userId;
    (void)type;
    (void)index;
    (void)param;

    void* caller = nullptr;
#ifdef _WIN32
    caller = _ReturnAddress();
#endif
    std::lock_guard lock(m_mutex);
    i32 handle = m_nextHandle++;

    // Ensure Vulkan context is initialized
    VulkanContext::Instance().Initialize();

    SB_LOG_INFO("VideoOut", "Opened VideoOut port handle={} (called by {:p}, Display: {}x{} @ 60Hz)",
        handle, caller, m_currentResolution.width, m_currentResolution.height);
    return handle;
}

i32 VideoOutManager::Close(i32 handle) {
    if (handle <= 0) return SCE_KERNEL_ERROR_EINVAL;
    void* caller = nullptr;
#ifdef _WIN32
    caller = _ReturnAddress();
#endif
    std::lock_guard lock(m_mutex);
    SB_LOG_INFO("VideoOut", "Closed VideoOut port handle={} (called by {:p})", handle, caller);
    return SCE_OK;
}

i32 VideoOutManager::RegisterBuffers(i32 handle, i32 startIndex, void* const* addresses, u32 bufferNum, const VideoOutBufferAttribute* attr) {
    if (handle <= 0 || !addresses || bufferNum == 0 || startIndex < 0 || startIndex > 64 || bufferNum > 64) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    if (startIndex + bufferNum > m_registeredBuffers.size()) {
        m_registeredBuffers.resize(startIndex + bufferNum, nullptr);
    }

    for (u32 i = 0; i < bufferNum; ++i) {
        void* ptr = nullptr;
        void* candidateRaw = ((void* const*)addresses)[i];
        if (candidateRaw != nullptr && (uintptr_t)candidateRaw >= 0x10000) {
            ptr = candidateRaw;
        }
        m_registeredBuffers[startIndex + i] = ptr;

        // Seed initial buffer memory with a clear test pattern (Sailor royal indigo/cyan with border)
        // so Vulkan presentation immediately displays non-black pixels
        if (ptr) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != 0 &&
                (mbi.State == MEM_COMMIT) &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
                uint32_t* pPixels = static_cast<uint32_t*>(ptr);
                uint32_t w = (attr && attr->width) ? attr->width : 1920;
                uint32_t h = (attr && attr->height) ? attr->height : 1080;
                size_t totalPixels = static_cast<size_t>(w) * h;
                size_t totalBytes = totalPixels * sizeof(uint32_t);
                if (mbi.RegionSize >= totalBytes) {
                    for (size_t p = 0; p < totalPixels; ++p) {
                        size_t y = p / w;
                        size_t x = p % w;
                        if (x < 12 || x >= w - 12 || y < 12 || y >= h - 12) {
                            pPixels[p] = 0xFFFFFFFF; // Bright white border
                        } else {
                            uint8_t r = static_cast<uint8_t>(20 + (x * 40 / w));
                            uint8_t g = static_cast<uint8_t>(70 + (y * 60 / h));
                            uint8_t b = static_cast<uint8_t>(140 + ((x + y) * 90 / (w + h)));
                            // Format B8G8R8A8: byte 0=B, byte 1=G, byte 2=R, byte 3=A
                            pPixels[p] = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                        }
                    }
                }
            }
        }

        SB_LOG_INFO("VideoOut", "  Buffer [{}]: ptr={:p} (raw={:p})",
            startIndex + i, ptr, candidateRaw);
    }

    if (attr) {
        m_currentResolution.width = attr->width ? attr->width : 1920;
        m_currentResolution.height = attr->height ? attr->height : 1080;
    }

    if (m_activeFramebuffer == nullptr && !m_registeredBuffers.empty()) {
        m_activeFramebuffer = m_registeredBuffers[startIndex];
    }

    SB_LOG_INFO("VideoOut", "Registered {} framebuffers for handle={} (base={:p}, res={}x{})",
        bufferNum, handle, m_registeredBuffers[startIndex], m_currentResolution.width, m_currentResolution.height);
    return SCE_OK;
}

i32 VideoOutManager::AddFlipEvent(void* eq, i32 handle, void* udata) {
    if (!eq) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard lock(m_mutex);
    m_flipEventQueues.push_back({eq, handle, udata});
    SB_LOG_INFO("VideoOut", "Registered flip event queue: eq={:p}, handle={}, udata={:p}", eq, handle, udata);
    return SCE_OK;
}

i32 VideoOutManager::DeleteFlipEvent(void* eq, i32 handle) {
    if (!eq) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard lock(m_mutex);
    std::erase_if(m_flipEventQueues, [eq, handle](const FlipEventQueueRegistration& reg) {
        return reg.eq == eq && (handle <= 0 || reg.handle == handle);
    });
    return SCE_OK;
}

i32 VideoOutManager::SubmitFlip(i32 handle, i32 bufferIndex, u32 flipMode, i64 flipArg) {
    if (handle < 0) return SCE_KERNEL_ERROR_EINVAL;
    (void)flipMode;

    void* fbPtr = nullptr;
    std::vector<FlipEventQueueRegistration> queuesCopy;
    std::vector<VideoOutFlipListener> listenersCopy;
    u32 width = 1920;
    u32 height = 1080;

    {
        std::lock_guard lock(m_mutex);
        if (bufferIndex >= 0 && static_cast<std::size_t>(bufferIndex) < m_registeredBuffers.size()) {
            m_currentBufferIndex = bufferIndex;
            fbPtr = m_registeredBuffers[bufferIndex];
            m_hasGuestFlips.store(true, std::memory_order_relaxed);
        } else if (!m_registeredBuffers.empty()) {
            m_currentBufferIndex = (m_currentBufferIndex + 1) % m_registeredBuffers.size();
            fbPtr = m_registeredBuffers[m_currentBufferIndex];
        }

        m_activeFramebuffer = fbPtr;
        queuesCopy = m_flipEventQueues;
        listenersCopy = m_flipListeners;
        width = m_currentResolution.width;
        height = m_currentResolution.height;
    }

    // Submit frame and dispatch notifications OUTSIDE m_mutex to avoid lock-order inversion
    VulkanContext::Instance().SubmitFrame(fbPtr, width, height);

#ifdef _WIN32
    using PushEqueueEventFn = i32(SAILOR_SYSV_ABI*)(void*, u64, i16, u16, u32, i64, void*);
    static auto pushEventFn = reinterpret_cast<PushEqueueEventFn>(
        GetProcAddress(GetModuleHandleA("sailor_kernel.dll"), "sceKernelPushEqueueEvent"));
    if (pushEventFn) {
        for (const auto& reg : queuesCopy) {
            pushEventFn(reg.eq, 1, -9, 0, 0, flipArg << 16, reg.udata);
        }
    }
#endif

    for (const auto& listener : listenersCopy) {
        if (listener.callback) {
            listener.callback(handle, flipArg, listener.udata);
        }
    }

    SB_LOG_INFO("VideoOut", "SubmitFlip complete: handle={}, bufferIndex={}, flipArg=0x{:X}",
        handle, bufferIndex, flipArg);
    return SCE_OK;
}

i32 VideoOutManager::GetResolutionStatus(i32 handle, VideoOutResolutionStatus* status) {
    if (handle <= 0 || !status) return SCE_KERNEL_ERROR_EINVAL;

    std::lock_guard lock(m_mutex);
    *status = m_currentResolution;
    return SCE_OK;
}

void* VideoOutManager::GetActiveFramebuffer() {
    std::lock_guard lock(m_mutex);
    if (m_activeFramebuffer) return m_activeFramebuffer;
    if (!m_registeredBuffers.empty() && m_currentBufferIndex < m_registeredBuffers.size()) {
        return m_registeredBuffers[m_currentBufferIndex];
    }
    return nullptr;
}

void VideoOutManager::RegisterFlipCallback(VideoOutFlipCallback cb, void* udata) {
    if (!cb) return;
    std::lock_guard lock(m_mutex);
    m_flipListeners.push_back({cb, udata});
}

void VideoOutManager::UnregisterFlipCallback(VideoOutFlipCallback cb) {
    if (!cb) return;
    std::lock_guard lock(m_mutex);
    std::erase_if(m_flipListeners, [cb](const VideoOutFlipListener& l) {
        return l.callback == cb;
    });
}

} // namespace Sailor
