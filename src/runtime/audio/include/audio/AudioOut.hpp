#pragma once

#include "common/Types.hpp"
#include <vector>
#include <unordered_map>
#include <mutex>

namespace Sailor {

constexpr i32 SCE_AUDIO_OUT_PORT_TYPE_MAIN   = 0;
constexpr i32 SCE_AUDIO_OUT_PORT_TYPE_BGM    = 1;
constexpr i32 SCE_AUDIO_OUT_PORT_TYPE_VOICE  = 2;
constexpr i32 SCE_AUDIO_OUT_PORT_TYPE_PADSPK = 4;

constexpr u32 SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO     = 0;
constexpr u32 SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO   = 1;
constexpr u32 SCE_AUDIO_OUT_PARAM_FORMAT_S16_5DOT1    = 3;
constexpr u32 SCE_AUDIO_OUT_PARAM_FORMAT_S16_7DOT1    = 4;
constexpr u32 SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO = 5;

#pragma pack(push, 1)

struct SceAudioOutPortInfo {
    u32 channels{2};
    u32 sampleRate{48000};
    u32 format{1};
    u32 sampleCount{256};
    u64 samplesPlayed{0};
    u32 reserved[4]{0};
};

#pragma pack(pop)

struct AudioPort {
    i32 handle{0};
    i32 userId{0};
    i32 type{0};
    u32 sampleCount{256};
    u32 sampleRate{48000};
    u32 format{SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO};
    u32 channels{2};
    float channelVolumes[8]{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float masterGain{1.0f};
    u64 totalSamplesOutput{0};
    std::vector<u8> lastBufferData;
};

class SAILOR_EXPORT AudioOutManager {
public:
    static AudioOutManager& Instance();

    i32 Init();
    i32 Open(i32 userId, i32 type, i32 index, u32 len, u32 freq, u32 param);
    i32 Close(i32 handle);
    i32 Output(i32 handle, const void* ptr);
    i32 SetVolume(i32 handle, u32 flag, const i32* volumes);
    i32 GetPortInfo(i32 handle, SceAudioOutPortInfo* info);
    i32 SetPortGain(i32 handle, i32 gain);

    // Testing and diagnostics
    size_t GetOpenPortCount();
    bool GetLastOutputData(i32 handle, std::vector<u8>& outData);
    void SetPacingEnabled(bool enabled) { m_pacingEnabled = enabled; }
    bool IsWasapiActive() const { return m_wasapiInitialized; }

    void WriteToWasapi(const void* ptr, u32 sampleCount, u32 channels, u32 format, const float* channelVolumes = nullptr, float masterGain = 1.0f);

private:
    AudioOutManager();
    ~AudioOutManager();

    void InitWasapi();
    void ShutdownWasapi();

    std::mutex m_mutex;
    std::unordered_map<i32, AudioPort> m_ports;
    i32 m_nextHandle{1};
    bool m_initialized{false};
    bool m_pacingEnabled{true};

    // Windows WASAPI Shared-Mode Audio Engine
    bool m_wasapiInitialized{false};
    void* m_audioClient{nullptr};
    void* m_renderClient{nullptr};
    void* m_waveFormat{nullptr};
    u32 m_bufferFrameCount{0};
};

} // namespace Sailor
