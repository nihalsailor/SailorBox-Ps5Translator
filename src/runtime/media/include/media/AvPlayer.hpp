#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace Sailor {

enum class AvPlayerState : u32 {
    Idle = 0,
    Playing = 1,
    Paused = 2,
    Stopped = 3
};

#pragma pack(push, 1)

struct SceAvPlayerInitData {
    u32 memorySize{0};
    void* memory{nullptr};
    u32 threadPriority{100};
    u8  debug{0};
    u8  reserved[7]{0};
};

struct SceAvPlayerVideoData {
    u64 timestamp{0};
    u32 width{1920};
    u32 height{1080};
    u32 pitch{1920 * 4};
    u32 aspectWidth{16};
    u32 aspectHeight{9};
    u8* pData{nullptr};
};

struct SceAvPlayerAudioData {
    u64 timestamp{0};
    u32 channelNum{2};
    u32 sampleRate{48000};
    u32 size{0};
    u8* pData{nullptr};
};

#pragma pack(pop)

struct AvPlayerStream {
    i32 handle{0};
    AvPlayerState state{AvPlayerState::Idle};
    std::string sourceUri;
    bool loop{false};
    u32 width{1920};
    u32 height{1080};
    float fps{60.0f};
    u64 currentTimeUs{0};
    u64 durationUs{10000000}; // 10s default
    u64 frameIndex{0};
    std::vector<u8> currentFrameRgba;
    std::vector<u8> currentAudioPcm;

    // Real Hardware Video Demuxer (Windows Media Foundation)
    void* sourceReader{nullptr};
    bool hasRealDemuxer{false};
};

class SAILOR_EXPORT AvPlayerManager {
public:
    static AvPlayerManager& Instance();

    i32 Init(const SceAvPlayerInitData* initData);
    i32 AddSource(i32 handle, const char* uri);
    i32 Start(i32 handle);
    i32 Stop(i32 handle);
    i32 Pause(i32 handle);
    bool IsActive(i32 handle);
    bool GetVideoData(i32 handle, SceAvPlayerVideoData* videoData);
    bool GetAudioData(i32 handle, SceAvPlayerAudioData* audioData);
    i32 SetLooping(i32 handle, bool loop);
    i32 Close(i32 handle);

    // Diagnostics & testing
    size_t GetActivePlayersCount();
    AvPlayerState GetPlayerState(i32 handle);

private:
    AvPlayerManager();
    ~AvPlayerManager();

    void InitMediaFoundation();
    void ShutdownMediaFoundation();
    bool TryOpenSourceReader(AvPlayerStream& player, const std::string& uri);
    void SynthesizeFrame(AvPlayerStream& player);

    std::mutex m_mutex;
    std::unordered_map<i32, AvPlayerStream> m_players;
    i32 m_nextHandle{1};
    bool m_initialized{false};
    bool m_mfStarted{false};
};

} // namespace Sailor
