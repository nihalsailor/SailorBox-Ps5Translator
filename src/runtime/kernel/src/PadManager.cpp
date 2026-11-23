#include "kernel/PadManager.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sailor {

PadManager& PadManager::Instance() {
    static PadManager instance;
    return instance;
}

PadManager::PadManager() {
    m_padState.connected = true;
    m_padState.leftStickX = 128;
    m_padState.leftStickY = 128;
    m_padState.rightStickX = 128;
    m_padState.rightStickY = 128;
}

i32 PadManager::Initialize() {
    std::lock_guard lock(m_mutex);
    m_initialized = true;
    SB_LOG_INFO("Pad", "SailorBox DualSense Pad Manager initialized");
    return SCE_OK;
}

i32 PadManager::Open(i32 userId, i32 type, i32 index, const void* param) {
    (void)userId;
    (void)type;
    (void)index;
    (void)param;
    i32 handle = m_nextHandle.fetch_add(1, std::memory_order_relaxed);
    SB_LOG_INFO("Pad", "Opened DualSense pad handle {}", handle);
    return handle;
}

i32 PadManager::Close(i32 handle) {
    SB_LOG_INFO("Pad", "Closed DualSense pad handle {}", handle);
    return SCE_OK;
}

void PadManager::PollInputs() {
    using namespace std::chrono;
    auto now = steady_clock::now().time_since_epoch();
    m_padState.timestamp = duration_cast<microseconds>(now).count();

    u32 buttons = 0;
    u8 lx = 128, ly = 128;
    u8 rx = 128, ry = 128;

#ifdef _WIN32
    // Keyboard input emulation
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)  buttons |= SCE_PAD_BUTTON_LEFT;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) buttons |= SCE_PAD_BUTTON_RIGHT;
    if (GetAsyncKeyState(VK_UP) & 0x8000)    buttons |= SCE_PAD_BUTTON_UP;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)  buttons |= SCE_PAD_BUTTON_DOWN;

    if (GetAsyncKeyState('A') & 0x8000) lx = 0;
    if (GetAsyncKeyState('D') & 0x8000) lx = 255;
    if (GetAsyncKeyState('W') & 0x8000) ly = 0;
    if (GetAsyncKeyState('S') & 0x8000) ly = 255;

    if (GetAsyncKeyState(VK_SPACE) & 0x8000)  buttons |= SCE_PAD_BUTTON_CROSS;
    if (GetAsyncKeyState(VK_RETURN) & 0x8000) buttons |= SCE_PAD_BUTTON_CIRCLE;
    if (GetAsyncKeyState('E') & 0x8000)        buttons |= SCE_PAD_BUTTON_SQUARE;
    if (GetAsyncKeyState('Q') & 0x8000)        buttons |= SCE_PAD_BUTTON_TRIANGLE;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)  buttons |= SCE_PAD_BUTTON_L1;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)buttons |= SCE_PAD_BUTTON_R1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) buttons |= SCE_PAD_BUTTON_OPTIONS;
#endif

    m_padState.buttons = buttons;
    m_padState.leftStickX = lx;
    m_padState.leftStickY = ly;
    m_padState.rightStickX = rx;
    m_padState.rightStickY = ry;
}

i32 PadManager::Read(i32 handle, ScePadData* data, i32 num) {
    if (!data || num <= 0) return SCE_KERNEL_ERROR_EINVAL;
    (void)handle;

    std::lock_guard lock(m_mutex);
    PollInputs();
    for (i32 i = 0; i < num; ++i) {
        data[i] = m_padState;
    }
    return num;
}

i32 PadManager::ReadState(i32 handle, ScePadData* data) {
    if (!data) return SCE_KERNEL_ERROR_EINVAL;
    (void)handle;

    std::lock_guard lock(m_mutex);
    PollInputs();
    *data = m_padState;
    return SCE_OK;
}

i32 PadManager::SetVibration(i32 handle, const ScePadVibrationParam* param) {
    if (!param) return SCE_KERNEL_ERROR_EINVAL;
    SB_LOG_DEBUG("Pad", "SetVibration handle {}: large={}, small={}", handle, param->largeMotor, param->smallMotor);
    return SCE_OK;
}

i32 PadManager::SetLightBar(i32 handle, const ScePadColor* color) {
    if (!color) return SCE_KERNEL_ERROR_EINVAL;
    SB_LOG_DEBUG("Pad", "SetLightBar handle {}: R={}, G={}, B={}", handle, color->r, color->g, color->b);
    return SCE_OK;
}

i32 PadManager::ResetOrientation(i32 handle) {
    (void)handle;
    std::lock_guard lock(m_mutex);
    m_padState.orientation[0] = 0.0f;
    m_padState.orientation[1] = 0.0f;
    m_padState.orientation[2] = 0.0f;
    m_padState.orientation[3] = 1.0f;
    return SCE_OK;
}

} // namespace Sailor
