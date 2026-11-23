#pragma once

#include "common/Types.hpp"
#include <mutex>
#include <atomic>

namespace Sailor {

struct SceAudioOutPortInfo {
    u32 numChannels{2};
    u32 sampleRate{48000};
    u32 format{0}; // 0 = 16-bit PCM, 1 = 32-bit Float
};

class AudioOutManager {
public:
    static AudioOutManager& Instance();

    i32 Open(i32 userId, i32 type, i32 index, u32 len, u32 freq, u32 param);
    i32 Close(i32 handle);
    i32 Output(i32 handle, const void* ptr);
    i32 SetVolume(i32 handle, i32 flag, const i32* vol);
    i32 GetPortInfo(i32 handle, SceAudioOutPortInfo* info);

private:
    AudioOutManager();
    std::mutex m_mutex;
    std::atomic<i32> m_nextHandle{1};
};

} // namespace Sailor
