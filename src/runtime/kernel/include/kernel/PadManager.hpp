#pragma once

#include "common/Types.hpp"
#include <mutex>
#include <atomic>

namespace Sailor {

// PS5 DualSense button bits
constexpr u32 SCE_PAD_BUTTON_L3        = 0x00000002;
constexpr u32 SCE_PAD_BUTTON_R3        = 0x00000004;
constexpr u32 SCE_PAD_BUTTON_OPTIONS   = 0x00000008;
constexpr u32 SCE_PAD_BUTTON_UP        = 0x00000010;
constexpr u32 SCE_PAD_BUTTON_RIGHT     = 0x00000020;
constexpr u32 SCE_PAD_BUTTON_DOWN      = 0x00000040;
constexpr u32 SCE_PAD_BUTTON_LEFT      = 0x00000080;
constexpr u32 SCE_PAD_BUTTON_L2        = 0x00000100;
constexpr u32 SCE_PAD_BUTTON_R2        = 0x00000200;
constexpr u32 SCE_PAD_BUTTON_L1        = 0x00000400;
constexpr u32 SCE_PAD_BUTTON_R1        = 0x00000800;
constexpr u32 SCE_PAD_BUTTON_TRIANGLE  = 0x00001000;
constexpr u32 SCE_PAD_BUTTON_CIRCLE    = 0x00002000;
constexpr u32 SCE_PAD_BUTTON_CROSS     = 0x00004000;
constexpr u32 SCE_PAD_BUTTON_SQUARE    = 0x00008000;
constexpr u32 SCE_PAD_BUTTON_TOUCH_PAD = 0x00100000;

struct ScePadData {
    u32 buttons{0};
    u8 leftStickX{128};
    u8 leftStickY{128};
    u8 rightStickX{128};
    u8 rightStickY{128};
    u8 analogButtons[8]{0}; // L2, R2 pressure, etc.
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f}; // Quaternion (x, y, z, w)
    float acceleration[3]{0.0f, 0.0f, -9.8f};
    float angularVelocity[3]{0.0f, 0.0f, 0.0f};
    u8 touchCount{0};
    u8 reserved[7]{0};
    u64 timestamp{0};
    bool connected{true};
};

struct ScePadVibrationParam {
    u8 largeMotor{0};
    u8 smallMotor{0};
};

struct ScePadColor {
    u8 r{0};
    u8 g{0};
    u8 b{255};
    u8 a{255};
};

class PadManager {
public:
    static PadManager& Instance();

    i32 Initialize();
    i32 Open(i32 userId, i32 type, i32 index, const void* param);
    i32 Close(i32 handle);
    i32 Read(i32 handle, ScePadData* data, i32 num);
    i32 ReadState(i32 handle, ScePadData* data);
    i32 SetVibration(i32 handle, const ScePadVibrationParam* param);
    i32 SetLightBar(i32 handle, const ScePadColor* color);
    i32 ResetOrientation(i32 handle);

private:
    PadManager();
    void PollInputs();

    std::mutex m_mutex;
    ScePadData m_padState{};
    bool m_initialized{false};
    std::atomic<i32> m_nextHandle{1};
};

} // namespace Sailor
