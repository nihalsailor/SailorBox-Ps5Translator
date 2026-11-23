#include "kernel/AudioOutManager.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

namespace Sailor {

AudioOutManager& AudioOutManager::Instance() {
    static AudioOutManager instance;
    return instance;
}

AudioOutManager::AudioOutManager() = default;

i32 AudioOutManager::Open(i32 userId, i32 type, i32 index, u32 len, u32 freq, u32 param) {
    (void)userId;
    (void)type;
    (void)index;
    (void)len;
    (void)param;
    i32 handle = m_nextHandle.fetch_add(1, std::memory_order_relaxed);
    SB_LOG_INFO("AudioOut", "Opened audio out port handle {} (freq={}, len={})", handle, freq, len);
    return handle;
}

i32 AudioOutManager::Close(i32 handle) {
    SB_LOG_INFO("AudioOut", "Closed audio out port handle {}", handle);
    return SCE_OK;
}

i32 AudioOutManager::Output(i32 handle, const void* ptr) {
    (void)handle;
    (void)ptr;
    // Consumed audio buffer without blocking the audio thread
    return SCE_OK;
}

i32 AudioOutManager::SetVolume(i32 handle, i32 flag, const i32* vol) {
    (void)handle;
    (void)flag;
    (void)vol;
    return SCE_OK;
}

i32 AudioOutManager::GetPortInfo(i32 handle, SceAudioOutPortInfo* info) {
    (void)handle;
    if (!info) return SCE_KERNEL_ERROR_EINVAL;
    info->numChannels = 2;
    info->sampleRate = 48000;
    info->format = 0;
    return SCE_OK;
}

} // namespace Sailor
