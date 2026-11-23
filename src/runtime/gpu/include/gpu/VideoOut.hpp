#pragma once

#include "common/Types.hpp"
#include <vector>
#include <atomic>
#include <mutex>

namespace Sailor {

using VideoOutFlipCallback = void(*)(i32 handle, i64 flipArg, void* udata);

struct VideoOutFlipListener {
    VideoOutFlipCallback callback{nullptr};
    void* udata{nullptr};
};

struct VideoOutResolutionStatus {
    u32 width{1920};
    u32 height{1080};
    f32 refreshRate{60.0f};
    u32 pixelFormat{0}; // 0 = ARGB8888
};

struct VideoOutBufferAttribute {
    u32 pixelFormat{0};
    u32 tilingMode{0};
    u32 aspectRatio{0};
    u32 width{1920};
    u32 height{1080};
    u32 pitchInPixels{1920};
};

class VideoOutManager {
public:
    static VideoOutManager& Instance();

    i32 Open(i32 userId, i32 type, i32 index, const void* param);
    i32 Close(i32 handle);

    i32 RegisterBuffers(i32 handle, i32 startIndex, void* const* addresses, u32 bufferNum, const VideoOutBufferAttribute* attr);
    i32 SubmitFlip(i32 handle, i32 bufferIndex, u32 flipMode, i64 flipArg);

    i32 AddFlipEvent(void* eq, i32 handle, void* udata);
    i32 DeleteFlipEvent(void* eq, i32 handle);

    i32 GetResolutionStatus(i32 handle, VideoOutResolutionStatus* status);

    void* GetActiveFramebuffer();
    bool HasGuestFlips() const { return m_hasGuestFlips.load(std::memory_order_relaxed); }

    void RegisterFlipCallback(VideoOutFlipCallback cb, void* udata = nullptr);
    void UnregisterFlipCallback(VideoOutFlipCallback cb);

private:
    VideoOutManager() = default;

    struct FlipEventQueueRegistration {
        void* eq{nullptr};
        i32 handle{-1};
        void* udata{nullptr};
    };

    std::mutex m_mutex;
    std::atomic<bool> m_hasGuestFlips{false};
    i32 m_nextHandle{1};
    i32 m_currentBufferIndex{0};
    void* m_activeFramebuffer{nullptr};
    std::vector<void*> m_registeredBuffers;
    VideoOutResolutionStatus m_currentResolution;
    std::vector<VideoOutFlipListener> m_flipListeners;
    std::vector<FlipEventQueueRegistration> m_flipEventQueues;
};

} // namespace Sailor
