#include "input/Pad.hpp"
#include "common/Types.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

using namespace Sailor;

extern "C" {

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadInit(void) {
    return PadManager::Instance().Init();
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadOpen(i32 userId, i32 type, i32 index, const void* param) {
    return PadManager::Instance().Open(userId, type, index, param);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadClose(i32 handle) {
    return PadManager::Instance().Close(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadRead(i32 handle, ScePadData* data, i32 num) {
    return PadManager::Instance().Read(handle, data, num);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadReadState(i32 handle, ScePadData* data) {
    return PadManager::Instance().ReadState(handle, data);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadSetVibration(i32 handle, const ScePadVibrationParam* param) {
    return PadManager::Instance().SetVibration(handle, param);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadSetLightBar(i32 handle, const ScePadColor* color) {
    return PadManager::Instance().SetLightBar(handle, color);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadResetOrientation(i32 handle) {
    return PadManager::Instance().ResetOrientation(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadSetMotionSensorState(i32 handle, bool enable) {
    return PadManager::Instance().SetMotionSensorState(handle, enable);
}

} // extern "C"
