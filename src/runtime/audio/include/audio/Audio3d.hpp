#pragma once

#include "common/Types.hpp"
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>

namespace Sailor {

using SceAudio3dPortId = i32;
using SceAudio3dObjectId = u32;

constexpr u32 SCE_AUDIO3D_MAX_OBJECTS = 64;
constexpr u32 SCE_AUDIO3D_DEFAULT_GRANULARITY = 256;
constexpr u32 SCE_AUDIO3D_DEFAULT_SAMPLERATE = 48000;

#pragma pack(push, 1)

struct SceAudio3dPosition {
    float x{0.0f};
    float y{0.0f};
    float z{1.0f};
};

struct SceAudio3dVelocity {
    float vx{0.0f};
    float vy{0.0f};
    float vz{0.0f};
};

struct SceAudio3dPortAttribute {
    u32 maxObjects{32};
    u32 sampleRate{48000};
    u32 granularity{256};
    u32 queueDepth{4};
    u32 reserved[4]{0};
};

struct SceAudio3dBuffer {
    void* pData{nullptr};
    u32 size{0};
    u64 timestamp{0};
    u32 reserved[2]{0};
};

#pragma pack(pop)

struct Audio3dObject {
    SceAudio3dObjectId id{0};
    bool reserved{false};
    SceAudio3dPosition position{0.0f, 0.0f, 1.0f};
    SceAudio3dVelocity velocity{0.0f, 0.0f, 0.0f};
    float volume{1.0f};
    float spread{0.0f};
    std::vector<float> pcmBuffer; // Mono float audio stream
};

struct Audio3dPort {
    SceAudio3dPortId id{0};
    SceAudio3dPortAttribute attributes;
    std::unordered_map<SceAudio3dObjectId, Audio3dObject> objects;
    SceAudio3dObjectId nextObjectId{1};
    std::vector<float> bedPcm; // 7.1 surround sound bed
    u32 bedChannels{8};
    std::vector<float> lastBinauralStereo; // Rendered binaural output (L, R interleaved)
};

class SAILOR_EXPORT Audio3dManager {
public:
    static Audio3dManager& Instance();

    i32 Initialize(u64 flags);
    i32 Terminate();
    i32 PortCreate(const SceAudio3dPortAttribute* attr, SceAudio3dPortId* outPortId);
    i32 PortDestroy(SceAudio3dPortId portId);
    i32 PortAdvance(SceAudio3dPortId portId);
    i32 PortPush(SceAudio3dPortId portId, const SceAudio3dBuffer* pBuffer);
    i32 PortFlush(SceAudio3dPortId portId);

    i32 ObjectReserve(SceAudio3dPortId portId, SceAudio3dObjectId* outObjectId);
    i32 ObjectUnreserve(SceAudio3dPortId portId, SceAudio3dObjectId objectId);
    i32 ObjectSetPosition(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const SceAudio3dPosition* pos);
    i32 ObjectSetVelocity(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const SceAudio3dVelocity* vel);
    i32 ObjectSetVolume(SceAudio3dPortId portId, SceAudio3dObjectId objectId, float volume);
    i32 ObjectSetSpread(SceAudio3dPortId portId, SceAudio3dObjectId objectId, float spread);
    i32 ObjectPushAudio(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const float* pcm, u32 sampleCount);

    i32 BedWrite(SceAudio3dPortId portId, const void* pData, u32 sizeInBytes);
    i32 GetDefaultAttributes(SceAudio3dPortAttribute* attr);

    // Tempest 3D Binaural Downmix DSP Pipeline
    void DownmixBinaural(Audio3dPort& port, float* outStereo, size_t sampleCount);

    // Diagnostics & testing
    bool GetLastBinauralOutput(SceAudio3dPortId portId, std::vector<float>& outPcm);
    size_t GetActivePortCount();

private:
    Audio3dManager() = default;
    ~Audio3dManager() = default;

    std::mutex m_mutex;
    std::unordered_map<SceAudio3dPortId, Audio3dPort> m_ports;
    SceAudio3dPortId m_nextPortId{1};
    bool m_initialized{false};
};

} // namespace Sailor
