#pragma once

#include "common/Types.hpp"
#include <mutex>
#include <unordered_map>

namespace Sailor {

constexpr u32 SCE_PAD_BUTTON_L3       = 0x00000002;
constexpr u32 SCE_PAD_BUTTON_R3       = 0x00000004;
constexpr u32 SCE_PAD_BUTTON_OPTIONS  = 0x00000008;
constexpr u32 SCE_PAD_BUTTON_UP       = 0x00000010;
constexpr u32 SCE_PAD_BUTTON_RIGHT    = 0x00000020;
constexpr u32 SCE_PAD_BUTTON_DOWN     = 0x00000040;
constexpr u32 SCE_PAD_BUTTON_LEFT     = 0x00000080;
constexpr u32 SCE_PAD_BUTTON_L2       = 0x00000100;
constexpr u32 SCE_PAD_BUTTON_R2       = 0x00000200;
constexpr u32 SCE_PAD_BUTTON_L1       = 0x00000400;
constexpr u32 SCE_PAD_BUTTON_R1       = 0x00000800;
constexpr u32 SCE_PAD_BUTTON_TRIANGLE = 0x00001000;
constexpr u32 SCE_PAD_BUTTON_CIRCLE   = 0x00002000;
constexpr u32 SCE_PAD_BUTTON_CROSS    = 0x00004000;
constexpr u32 SCE_PAD_BUTTON_SQUARE   = 0x00008000;
constexpr u32 SCE_PAD_BUTTON_TOUCHPAD = 0x00100000;

#pragma pack(push, 1)

struct SceFVector3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct SceFQuaternion {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};
};

struct ScePadTouch {
    u16 x{0};
    u16 y{0};
    u8  id{0};
    u8  reserved[3]{0};
};

struct ScePadTouchData {
    u8 touchNum{0};
    u8 reserved[7]{0};
    ScePadTouch touch[2];
};

struct ScePadData {
    u32 buttons{0};
    u8  leftStickX{128};
    u8  leftStickY{128};
    u8  rightStickX{128};
    u8  rightStickY{128};
    u8  analogButtonsL2{0};
    u8  analogButtonsR2{0};
    u8  padding[2]{0};
    SceFQuaternion orientation{0.0f, 0.0f, 0.0f, 1.0f};
    SceFVector3 acceleration{0.0f, 0.0f, 1.0f};
    SceFVector3 angularVelocity{0.0f, 0.0f, 0.0f};
    ScePadTouchData touchData{};
    u8  connected{1};
    u8  reserved[7]{0};
    u64 timestamp{0};
};

struct ScePadVibrationParam {
    u8 largeMotor{0};
    u8 smallMotor{0};
};

struct ScePadColor {
    u8 r{0};
    u8 g{0};
    u8 b{0};
    u8 a{255};
};

#pragma pack(pop)

struct PadDevice {
    i32 handle{0};
    i32 userId{0};
    bool connected{true};
    bool motionEnabled{false};
    ScePadData currentState{};
    ScePadVibrationParam currentVibration{};
    ScePadColor currentColor{0, 0, 255, 255}; // Default PlayStation Blue
    bool hasSimulatedInput{false};
    ScePadData simulatedState{};
};

class SAILOR_EXPORT PadManager {
public:
    static PadManager& Instance();

    i32 Init();
    i32 Open(i32 userId, i32 type, i32 index, const void* param);
    i32 Close(i32 handle);
    i32 ReadState(i32 handle, ScePadData* data);
    i32 Read(i32 handle, ScePadData* data, i32 count);
    i32 SetVibration(i32 handle, const ScePadVibrationParam* param);
    i32 SetLightBar(i32 handle, const ScePadColor* color);
    i32 ResetOrientation(i32 handle);
    i32 SetMotionSensorState(i32 handle, bool enable);

    // Testing and simulation hooks
    void SetSimulatedInput(i32 handle, const ScePadData& simulated);
    void ClearSimulatedInput(i32 handle);
    bool GetCurrentVibration(i32 handle, ScePadVibrationParam& outVib);
    bool GetCurrentColor(i32 handle, ScePadColor& outColor);

private:
    PadManager();
    ~PadManager() = default;

    void PollHardware(PadDevice& dev);

    std::mutex m_mutex;
    std::unordered_map<i32, PadDevice> m_devices;
    i32 m_nextHandle{1};
    bool m_initialized{false};
    void* m_xinputDll{nullptr};
};

} // namespace Sailor
