#include "media/AvPlayer.hpp"
#include "common/Types.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

using namespace Sailor;

extern "C" {

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAvPlayerInit(const SceAvPlayerInitData* initData) {
    return AvPlayerManager::Instance().Init(initData);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAvPlayerAddSource(i32 handle, const char* uri) {
    return AvPlayerManager::Instance().AddSource(handle, uri);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAvPlayerStart(i32 handle) {
    return AvPlayerManager::Instance().Start(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAvPlayerStop(i32 handle) {
    return AvPlayerManager::Instance().Stop(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAvPlayerPause(i32 handle) {
    return AvPlayerManager::Instance().Pause(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI bool sceAvPlayerIsActive(i32 handle) {
    return AvPlayerManager::Instance().IsActive(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI bool sceAvPlayerGetVideoData(i32 handle, SceAvPlayerVideoData* videoData) {
    return AvPlayerManager::Instance().GetVideoData(handle, videoData);
}

SAILOR_EXPORT SAILOR_SYSV_ABI bool sceAvPlayerGetAudioData(i32 handle, SceAvPlayerAudioData* audioData) {
    return AvPlayerManager::Instance().GetAudioData(handle, audioData);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAvPlayerSetLooping(i32 handle, bool loop) {
    return AvPlayerManager::Instance().SetLooping(handle, loop);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceAvPlayerClose(i32 handle) {
    return AvPlayerManager::Instance().Close(handle);
}

} // extern "C"
