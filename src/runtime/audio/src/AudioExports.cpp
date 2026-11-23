#include "audio/AudioOut.hpp"
#include "audio/Audio3d.hpp"
#include "common/Types.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

using namespace Sailor;

extern "C" {

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudioOutInit(void) {
    return AudioOutManager::Instance().Init();
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudioOutOpen(i32 userId, i32 type, i32 index, u32 len, u32 freq, u32 param) {
    return AudioOutManager::Instance().Open(userId, type, index, len, freq, param);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudioOutClose(i32 handle) {
    return AudioOutManager::Instance().Close(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudioOutOutput(i32 handle, const void* ptr) {
    return AudioOutManager::Instance().Output(handle, ptr);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudioOutSetVolume(i32 handle, u32 flag, const i32* volumes) {
    return AudioOutManager::Instance().SetVolume(handle, flag, volumes);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudioOutGetPortInfo(i32 handle, SceAudioOutPortInfo* info) {
    return AudioOutManager::Instance().GetPortInfo(handle, info);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudioOutSetPortGain(i32 handle, i32 gain) {
    return AudioOutManager::Instance().SetPortGain(handle, gain);
}

// -----------------------------------------------------------------------------
// libSceAudio3d (Tempest 3D Audio Engine Exports)
// -----------------------------------------------------------------------------

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dInitialize(u64 flags) {
    return Audio3dManager::Instance().Initialize(flags);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dPortCreate(const SceAudio3dPortAttribute* attr, SceAudio3dPortId* outPortId) {
    return Audio3dManager::Instance().PortCreate(attr, outPortId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dPortDestroy(SceAudio3dPortId portId) {
    return Audio3dManager::Instance().PortDestroy(portId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dPortAdvance(SceAudio3dPortId portId) {
    return Audio3dManager::Instance().PortAdvance(portId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dPortPush(SceAudio3dPortId portId, const SceAudio3dBuffer* pBuffer) {
    return Audio3dManager::Instance().PortPush(portId, pBuffer);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dPortFlush(SceAudio3dPortId portId) {
    return Audio3dManager::Instance().PortFlush(portId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dObjectReserve(SceAudio3dPortId portId, SceAudio3dObjectId* outObjectId) {
    return Audio3dManager::Instance().ObjectReserve(portId, outObjectId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dObjectUnreserve(SceAudio3dPortId portId, SceAudio3dObjectId objectId) {
    return Audio3dManager::Instance().ObjectUnreserve(portId, objectId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dObjectSetPosition(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const SceAudio3dPosition* pos) {
    return Audio3dManager::Instance().ObjectSetPosition(portId, objectId, pos);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dObjectSetVelocity(SceAudio3dPortId portId, SceAudio3dObjectId objectId, const SceAudio3dVelocity* vel) {
    return Audio3dManager::Instance().ObjectSetVelocity(portId, objectId, vel);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dObjectSetVolume(SceAudio3dPortId portId, SceAudio3dObjectId objectId, float volume) {
    return Audio3dManager::Instance().ObjectSetVolume(portId, objectId, volume);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dObjectSetSpread(SceAudio3dPortId portId, SceAudio3dObjectId objectId, float spread) {
    return Audio3dManager::Instance().ObjectSetSpread(portId, objectId, spread);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dBedWrite(SceAudio3dPortId portId, const void* pData, u32 sizeInBytes) {
    return Audio3dManager::Instance().BedWrite(portId, pData, sizeInBytes);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dGetDefaultAttributes(SceAudio3dPortAttribute* attr) {
    return Audio3dManager::Instance().GetDefaultAttributes(attr);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dCreateContext(const void* options, i32* outContext) {
    (void)options;
    if (outContext) *outContext = 1;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAudio3dDestroyContext(i32 context) {
    (void)context;
    return SCE_OK;
}

} // extern "C"
