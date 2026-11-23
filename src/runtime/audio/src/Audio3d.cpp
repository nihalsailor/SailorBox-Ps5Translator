#include "audio/Audio3d.hpp"
#include "audio/AudioOut.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace Sailor {

Audio3dManager& Audio3dManager::Instance() {
    static Audio3dManager instance;
    return instance;
}

i32 Audio3dManager::Initialize(u64 flags) {
    (void)flags;
    std::lock_guard lock(m_mutex);
    m_initialized = true;
    SB_LOG_INFO("Audio3d", "Initialized Sony Tempest 3D Audio Engine (Binaural HRTF Downmixer)");
    return SCE_OK;
}

i32 Audio3dManager::Terminate() {
    std::lock_guard lock(m_mutex);
    m_ports.clear();
    m_initialized = false;
    SB_LOG_INFO("Audio3d", "Terminated Tempest 3D Audio Engine");
    return SCE_OK;
}

i32 Audio3dManager::GetDefaultAttributes(SceAudio3dPortAttribute* attr) {
    if (!attr) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    attr->maxObjects = 32;
    attr->sampleRate = 48000;
    attr->granularity = 256;
    attr->queueDepth = 4;
    std::memset(attr->reserved, 0, sizeof(attr->reserved));
    return SCE_OK;
}

i32 Audio3dManager::PortCreate(const SceAudio3dPortAttribute* attr, SceAudio3dPortId* outPortId) {
    if (!outPortId) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    if (!m_initialized) {
        m_initialized = true;
        SB_LOG_INFO("Audio3d", "Auto-initialized Tempest 3D Audio Engine");
    }

    const SceAudio3dPortId portId = m_nextPortId++;
    Audio3dPort port;
    port.id = portId;
    if (attr) {
        port.attributes = *attr;
    } else {
        GetDefaultAttributes(&port.attributes);
    }

    m_ports[portId] = std::move(port);
    *outPortId = portId;

    SB_LOG_INFO("Audio3d", "Created Audio3d Port handle={} (MaxObjects={}, Granularity={}, SampleRate={}Hz)",
        portId, port.attributes.maxObjects, port.attributes.granularity, port.attributes.sampleRate);
    return SCE_OK;
}

i32 Audio3dManager::PortDestroy(SceAudio3dPortId portId) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    m_ports.erase(it);
    SB_LOG_DEBUG("Audio3d", "Destroyed Audio3d Port handle={}", portId);
    return SCE_OK;
}

i32 Audio3dManager::ObjectReserve(SceAudio3dPortId portId, SceAudio3dObjectId* outObjectId) {
    if (!outObjectId) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    Audio3dPort& port = it->second;
    if (port.objects.size() >= port.attributes.maxObjects) {
        return SCE_KERNEL_ERROR_ENOMEM;
    }

    const SceAudio3dObjectId objId = port.nextObjectId++;
    Audio3dObject obj;
    obj.id = objId;
    obj.reserved = true;
    obj.position = SceAudio3dPosition{0.0f, 0.0f, 1.0f};
    obj.volume = 1.0f;

    port.objects[objId] = std::move(obj);
    *outObjectId = objId;

    SB_LOG_DEBUG("Audio3d", "Port {} reserved 3D audio object {}", portId, objId);
    return SCE_OK;
}

i32 Audio3dManager::ObjectUnreserve(SceAudio3dPortId portId, SceAudio3dObjectId objectId) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    auto objIt = it->second.objects.find(objectId);
    if (objIt == it->second.objects.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    it->second.objects.erase(objIt);
    SB_LOG_DEBUG("Audio3d", "Port {} unreserved 3D audio object {}", portId, objectId);
    return SCE_OK;
}

i32 Audio3dManager::ObjectSetPosition(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const SceAudio3dPosition* pos) {
    if (!pos) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    auto objIt = it->second.objects.find(objectId);
    if (objIt == it->second.objects.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    objIt->second.position = *pos;
    return SCE_OK;
}

i32 Audio3dManager::ObjectSetVelocity(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const SceAudio3dVelocity* vel) {
    if (!vel) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    auto objIt = it->second.objects.find(objectId);
    if (objIt == it->second.objects.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    objIt->second.velocity = *vel;
    return SCE_OK;
}

i32 Audio3dManager::ObjectSetVolume(SceAudio3dPortId portId, SceAudio3dObjectId objectId, float volume) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    auto objIt = it->second.objects.find(objectId);
    if (objIt == it->second.objects.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    objIt->second.volume = std::clamp(volume, 0.0f, 4.0f);
    return SCE_OK;
}

i32 Audio3dManager::ObjectSetSpread(SceAudio3dPortId portId, SceAudio3dObjectId objectId, float spread) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    auto objIt = it->second.objects.find(objectId);
    if (objIt == it->second.objects.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    objIt->second.spread = std::clamp(spread, 0.0f, 1.0f);
    return SCE_OK;
}

i32 Audio3dManager::ObjectPushAudio(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const float* pcm, u32 sampleCount) {
    if (!pcm || sampleCount == 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    auto objIt = it->second.objects.find(objectId);
    if (objIt == it->second.objects.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    objIt->second.pcmBuffer.assign(pcm, pcm + sampleCount);
    return SCE_OK;
}

i32 Audio3dManager::BedWrite(SceAudio3dPortId portId, const void* pData, u32 sizeInBytes) {
    if (!pData || sizeInBytes == 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    const size_t floatCount = sizeInBytes / sizeof(float);
    const float* src = reinterpret_cast<const float*>(pData);
    it->second.bedPcm.assign(src, src + floatCount);
    return SCE_OK;
}

void Audio3dManager::DownmixBinaural(Audio3dPort& port, float* outStereo, size_t sampleCount) {
    // Zero initialize stereo output
    std::fill_n(outStereo, sampleCount * 2, 0.0f);

    // 1. Process 3D Positional Audio Objects
    for (auto& [objId, obj] : port.objects) {
        if (obj.pcmBuffer.empty()) continue;

        const float x = obj.position.x;
        const float y = obj.position.y;
        const float z = obj.position.z;

        // Euclidean distance from listener at (0, 0, 0)
        const float dist = std::sqrt(x * x + y * y + z * z);
        const float attenuation = 1.0f / std::max(1.0f, dist);

        // Azimuth angle in horizontal plane
        const float azimuth = std::atan2(x, (std::abs(z) > 0.001f) ? z : 0.001f);
        const float sinAzimuth = std::sin(azimuth);

        // Interaural Level Difference (ILD)
        const float gainL = attenuation * std::clamp(0.5f * (1.0f - sinAzimuth), 0.1f, 1.0f) * obj.volume;
        const float gainR = attenuation * std::clamp(0.5f * (1.0f + sinAzimuth), 0.1f, 1.0f) * obj.volume;

        // Interaural Time Difference (ITD delay in samples, max ~16 samples at 48kHz)
        const int delayDelta = static_cast<int>(16.0f * sinAzimuth);
        const int delayL = std::max(0, delayDelta);
        const int delayR = std::max(0, -delayDelta);

        const size_t count = std::min(sampleCount, obj.pcmBuffer.size());
        for (size_t i = 0; i < count; ++i) {
            const float sampleL = (i >= static_cast<size_t>(delayL)) ? obj.pcmBuffer[i - delayL] : 0.0f;
            const float sampleR = (i >= static_cast<size_t>(delayR)) ? obj.pcmBuffer[i - delayR] : 0.0f;

            outStereo[i * 2 + 0] += sampleL * gainL;
            outStereo[i * 2 + 1] += sampleR * gainR;
        }
    }

    // 2. Downmix 7.1 Bed Channels if present
    if (!port.bedPcm.empty() && port.bedChannels >= 8) {
        const size_t bedFrames = port.bedPcm.size() / port.bedChannels;
        const size_t count = std::min(sampleCount, bedFrames);
        for (size_t i = 0; i < count; ++i) {
            const float L  = port.bedPcm[i * 8 + 0];
            const float R  = port.bedPcm[i * 8 + 1];
            const float C  = port.bedPcm[i * 8 + 2];
            const float LFE = port.bedPcm[i * 8 + 3];
            const float Ls = port.bedPcm[i * 8 + 4];
            const float Rs = port.bedPcm[i * 8 + 5];
            const float Lb = port.bedPcm[i * 8 + 6];
            const float Rb = port.bedPcm[i * 8 + 7];

            // 7.1 to Binaural Stereo Matrix
            outStereo[i * 2 + 0] += (L + 0.7071f * C + 0.5f * LFE + 0.7071f * Ls + 0.5f * Lb) * 0.5f;
            outStereo[i * 2 + 1] += (R + 0.7071f * C + 0.5f * LFE + 0.7071f * Rs + 0.5f * Rb) * 0.5f;
        }
    }

    // Soft limiter / clamping to prevent clipping [-1.0, 1.0]
    for (size_t i = 0; i < sampleCount * 2; ++i) {
        outStereo[i] = std::clamp(outStereo[i], -1.0f, 1.0f);
    }
}

i32 Audio3dManager::PortAdvance(SceAudio3dPortId portId) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    Audio3dPort& port = it->second;
    const size_t granularity = port.attributes.granularity ? port.attributes.granularity : 256;
    port.lastBinauralStereo.resize(granularity * 2);

    DownmixBinaural(port, port.lastBinauralStereo.data(), granularity);

    AudioOutManager::Instance().WriteToWasapi(
        port.lastBinauralStereo.data(),
        static_cast<u32>(granularity),
        2,
        SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO
    );
    return SCE_OK;
}

i32 Audio3dManager::PortPush(SceAudio3dPortId portId, const SceAudio3dBuffer* pBuffer) {
    if (!pBuffer || !pBuffer->pData || pBuffer->size == 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    return BedWrite(portId, pBuffer->pData, pBuffer->size);
}

i32 Audio3dManager::PortFlush(SceAudio3dPortId portId) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    for (auto& [_, obj] : it->second.objects) {
        obj.pcmBuffer.clear();
    }
    it->second.bedPcm.clear();
    return SCE_OK;
}

bool Audio3dManager::GetLastBinauralOutput(SceAudio3dPortId portId, std::vector<float>& outPcm) {
    std::lock_guard lock(m_mutex);
    auto it = m_ports.find(portId);
    if (it == m_ports.end() || it->second.lastBinauralStereo.empty()) {
        return false;
    }
    outPcm = it->second.lastBinauralStereo;
    return true;
}

size_t Audio3dManager::GetActivePortCount() {
    std::lock_guard lock(m_mutex);
    return m_ports.size();
}

} // namespace Sailor
