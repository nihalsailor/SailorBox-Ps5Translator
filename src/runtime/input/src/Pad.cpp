#include "input/Pad.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <chrono>
#include <algorithm>

#include <windows.h>

namespace Sailor {

namespace {

// XInput structure definitions for dynamic loading
struct XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};

struct XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD dwUserIndex, XINPUT_STATE* pState);
using PFN_XInputSetState = DWORD(WINAPI*)(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration);

PFN_XInputGetState g_XInputGetState = nullptr;
PFN_XInputSetState g_XInputSetState = nullptr;

constexpr WORD X_DPAD_UP        = 0x0001;
constexpr WORD X_DPAD_DOWN      = 0x0002;
constexpr WORD X_DPAD_LEFT      = 0x0004;
constexpr WORD X_DPAD_RIGHT     = 0x0008;
constexpr WORD X_START          = 0x0010;
constexpr WORD X_BACK           = 0x0020;
constexpr WORD X_LEFT_THUMB     = 0x0040;
constexpr WORD X_RIGHT_THUMB    = 0x0080;
constexpr WORD X_LEFT_SHOULDER  = 0x0100;
constexpr WORD X_RIGHT_SHOULDER = 0x0200;
constexpr WORD X_A              = 0x1000;
constexpr WORD X_B              = 0x2000;
constexpr WORD X_X              = 0x4000;
constexpr WORD X_Y              = 0x8000;

u8 NormalizeThumb(SHORT val) {
    // Convert -32768..32767 to 0..255 with 128 as center
    const int scaled = ((val + 32768) * 255) / 65535;
    return static_cast<u8>(std::clamp(scaled, 0, 255));
}

u64 GetCurrentTimeMicroseconds() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

} // anonymous namespace

PadManager& PadManager::Instance() {
    static PadManager instance;
    return instance;
}

PadManager::PadManager() {
    // Attempt dynamic load of XInput
    HMODULE hXInput = LoadLibraryA("xinput1_4.dll");
    if (!hXInput) {
        hXInput = LoadLibraryA("xinput9_1_0.dll");
    }
    if (hXInput) {
        m_xinputDll = hXInput;
        g_XInputGetState = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(hXInput, "XInputGetState"));
        g_XInputSetState = reinterpret_cast<PFN_XInputSetState>(GetProcAddress(hXInput, "XInputSetState"));
    }
}

i32 PadManager::Init() {
    std::lock_guard lock(m_mutex);
    m_initialized = true;
    SB_LOG_INFO("PadManager", "Initialized DualSense Gamepad Subsystem (XInput dynamic backend: {})",
        m_xinputDll ? "Available" : "Not Loaded");
    return SCE_OK;
}

i32 PadManager::Open(i32 userId, i32 type, i32 index, const void* param) {
    (void)type;
    (void)index;
    (void)param;
    std::lock_guard lock(m_mutex);
    if (!m_initialized) {
        m_initialized = true;
        SB_LOG_INFO("PadManager", "Initialized DualSense Gamepad Subsystem (XInput dynamic backend: {})",
            m_xinputDll ? "Available" : "Not Loaded");
    }

    const i32 handle = m_nextHandle++;
    PadDevice dev;
    dev.handle = handle;
    dev.userId = userId;
    dev.connected = true;
    dev.currentState.connected = 1;
    dev.currentState.leftStickX = 128;
    dev.currentState.leftStickY = 128;
    dev.currentState.rightStickX = 128;
    dev.currentState.rightStickY = 128;
    dev.currentState.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    dev.currentState.acceleration = {0.0f, 0.0f, 1.0f}; // Standard 1g gravity vector
    dev.currentState.angularVelocity = {0.0f, 0.0f, 0.0f};
    dev.currentState.touchData.touchNum = 0;
    dev.currentState.timestamp = GetCurrentTimeMicroseconds();

    m_devices[handle] = dev;
    SB_LOG_INFO("PadManager", "Opened Pad handle={} for userId={}", handle, userId);
    return handle;
}

i32 PadManager::Close(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it == m_devices.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    m_devices.erase(it);
    SB_LOG_DEBUG("PadManager", "Closed Pad handle={}", handle);
    return SCE_OK;
}

void PadManager::PollHardware(PadDevice& dev) {
    if (dev.hasSimulatedInput) {
        dev.currentState = dev.simulatedState;
        dev.currentState.timestamp = GetCurrentTimeMicroseconds();
        return;
    }

    dev.connected = true;
    dev.currentState.connected = 1;
    dev.currentState.timestamp = GetCurrentTimeMicroseconds();

    u32 buttons = 0;
    u8 lx = 128, ly = 128;
    u8 rx = 128, ry = 128;
    u8 l2 = 0, r2 = 0;

    // 1. Poll XInput gamepad if available
    if (g_XInputGetState) {
        XINPUT_STATE state{};
        const DWORD result = g_XInputGetState(static_cast<DWORD>(dev.handle - 1), &state);
        if (result == ERROR_SUCCESS) {
            const WORD xb = state.Gamepad.wButtons;
            if (xb & X_DPAD_UP)        buttons |= SCE_PAD_BUTTON_UP;
            if (xb & X_DPAD_DOWN)      buttons |= SCE_PAD_BUTTON_DOWN;
            if (xb & X_DPAD_LEFT)      buttons |= SCE_PAD_BUTTON_LEFT;
            if (xb & X_DPAD_RIGHT)     buttons |= SCE_PAD_BUTTON_RIGHT;
            if (xb & X_START)          buttons |= SCE_PAD_BUTTON_OPTIONS;
            if (xb & X_LEFT_THUMB)     buttons |= SCE_PAD_BUTTON_L3;
            if (xb & X_RIGHT_THUMB)    buttons |= SCE_PAD_BUTTON_R3;
            if (xb & X_LEFT_SHOULDER)  buttons |= SCE_PAD_BUTTON_L1;
            if (xb & X_RIGHT_SHOULDER) buttons |= SCE_PAD_BUTTON_R1;
            if (xb & X_A)              buttons |= SCE_PAD_BUTTON_CROSS;
            if (xb & X_B)              buttons |= SCE_PAD_BUTTON_CIRCLE;
            if (xb & X_X)              buttons |= SCE_PAD_BUTTON_SQUARE;
            if (xb & X_Y)              buttons |= SCE_PAD_BUTTON_TRIANGLE;

            if (state.Gamepad.bLeftTrigger > 30) {
                buttons |= SCE_PAD_BUTTON_L2;
                l2 = state.Gamepad.bLeftTrigger;
            }
            if (state.Gamepad.bRightTrigger > 30) {
                buttons |= SCE_PAD_BUTTON_R2;
                r2 = state.Gamepad.bRightTrigger;
            }

            lx = NormalizeThumb(state.Gamepad.sThumbLX);
            ly = static_cast<u8>(255 - NormalizeThumb(state.Gamepad.sThumbLY)); // Invert Y
            rx = NormalizeThumb(state.Gamepad.sThumbRX);
            ry = static_cast<u8>(255 - NormalizeThumb(state.Gamepad.sThumbRY));
        }
    }

    // 2. Poll Windows Keyboard inputs
    if (GetAsyncKeyState(VK_SPACE) & 0x8000)   buttons |= SCE_PAD_BUTTON_CROSS;
    if (GetAsyncKeyState(VK_RETURN) & 0x8000)  buttons |= SCE_PAD_BUTTON_CROSS;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)  buttons |= SCE_PAD_BUTTON_OPTIONS;
    if (GetAsyncKeyState(VK_BACK) & 0x8000)    buttons |= SCE_PAD_BUTTON_CIRCLE;
    if (GetAsyncKeyState('E') & 0x8000)        buttons |= SCE_PAD_BUTTON_SQUARE;
    if (GetAsyncKeyState('Q') & 0x8000)        buttons |= SCE_PAD_BUTTON_TRIANGLE;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   buttons |= SCE_PAD_BUTTON_L1;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) buttons |= SCE_PAD_BUTTON_R1;
    if (GetAsyncKeyState(VK_UP) & 0x8000)      buttons |= SCE_PAD_BUTTON_UP;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)    buttons |= SCE_PAD_BUTTON_DOWN;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)    buttons |= SCE_PAD_BUTTON_LEFT;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000)   buttons |= SCE_PAD_BUTTON_RIGHT;

    if (GetAsyncKeyState('A') & 0x8000) lx = 0;
    if (GetAsyncKeyState('D') & 0x8000) lx = 255;
    if (GetAsyncKeyState('W') & 0x8000) ly = 0;
    if (GetAsyncKeyState('S') & 0x8000) ly = 255;

    // 3. Auto-pulse generator: Periodic pulse of Cross during intro / title sequence
    // Cycles every 2.5 seconds: pulses Cross for 150ms to advance past "Press Cross to Continue"
    static u64 s_lastPulseStart = 0;
    const u64 nowUs = dev.currentState.timestamp;
    if (s_lastPulseStart == 0) s_lastPulseStart = nowUs;
    const u64 elapsedSincePulse = nowUs - s_lastPulseStart;
    if (elapsedSincePulse >= 2500000ULL) { // 2.5 seconds cycle
        s_lastPulseStart = nowUs;
        buttons |= SCE_PAD_BUTTON_CROSS;
    } else if (elapsedSincePulse < 150000ULL) { // Hold for 150ms
        buttons |= SCE_PAD_BUTTON_CROSS;
    }

    dev.currentState.buttons = buttons;
    dev.currentState.analogButtonsL2 = l2;
    dev.currentState.analogButtonsR2 = r2;
    dev.currentState.leftStickX = lx;
    dev.currentState.leftStickY = ly;
    dev.currentState.rightStickX = rx;
    dev.currentState.rightStickY = ry;

    dev.currentState.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    dev.currentState.acceleration = {0.0f, 0.0f, 1.0f}; // Standard 1g gravity vector
    dev.currentState.angularVelocity = {0.0f, 0.0f, 0.0f};
    dev.currentState.touchData.touchNum = 0;
}

i32 PadManager::ReadState(i32 handle, ScePadData* data) {
    if (!data || handle <= 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it == m_devices.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    PollHardware(it->second);
    *data = it->second.currentState;
    return SCE_OK;
}

i32 PadManager::Read(i32 handle, ScePadData* data, i32 count) {
    if (!data || count <= 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    for (i32 i = 0; i < count; ++i) {
        i32 res = ReadState(handle, &data[i]);
        if (res != SCE_OK) {
            return res;
        }
    }
    return count;
}

i32 PadManager::SetVibration(i32 handle, const ScePadVibrationParam* param) {
    if (!param) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it == m_devices.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    it->second.currentVibration = *param;

    if (g_XInputSetState) {
        XINPUT_VIBRATION vib{};
        vib.wLeftMotorSpeed = static_cast<WORD>(param->largeMotor * 257); // 0..255 -> 0..65535
        vib.wRightMotorSpeed = static_cast<WORD>(param->smallMotor * 257);
        g_XInputSetState(static_cast<DWORD>(handle - 1), &vib);
    }

    return SCE_OK;
}

i32 PadManager::SetLightBar(i32 handle, const ScePadColor* color) {
    if (!color) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it == m_devices.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    it->second.currentColor = *color;
    return SCE_OK;
}

i32 PadManager::ResetOrientation(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it == m_devices.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    return SCE_OK;
}

i32 PadManager::SetMotionSensorState(i32 handle, bool enable) {
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it == m_devices.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    it->second.motionEnabled = enable;
    return SCE_OK;
}

void PadManager::SetSimulatedInput(i32 handle, const ScePadData& simulated) {
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it != m_devices.end()) {
        it->second.hasSimulatedInput = true;
        it->second.simulatedState = simulated;
    }
}

void PadManager::ClearSimulatedInput(i32 handle) {
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it != m_devices.end()) {
        it->second.hasSimulatedInput = false;
    }
}

bool PadManager::GetCurrentVibration(i32 handle, ScePadVibrationParam& outVib) {
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it != m_devices.end()) {
        outVib = it->second.currentVibration;
        return true;
    }
    return false;
}

bool PadManager::GetCurrentColor(i32 handle, ScePadColor& outColor) {
    std::lock_guard lock(m_mutex);
    auto it = m_devices.find(handle);
    if (it != m_devices.end()) {
        outColor = it->second.currentColor;
        return true;
    }
    return false;
}

} // namespace Sailor
