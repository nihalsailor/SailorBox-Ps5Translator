#include "input/Pad.hpp"
#include "audio/AudioOut.hpp"
#include "media/AvPlayer.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>

using namespace Sailor;

void TestDualSenseInput() {
    std::cout << "[Test 1] Testing DualSense Gamepad Subsystem (sailor_pad)..." << std::endl;
    PadManager& pad = PadManager::Instance();

    assert(pad.Init() == SCE_OK);

    // Open Pad handle #1
    const i32 handle = pad.Open(1, 0, 0, nullptr);
    assert(handle > 0);
    std::cout << "  -> Opened Pad handle=" << handle << std::endl;

    // Test default centered neutral state (allowing standard +/- 8 hardware deadzone if physical gamepad is plugged in)
    ScePadData data{};
    assert(pad.ReadState(handle, &data) == SCE_OK);
    assert(data.connected == 1);
    assert(std::abs(static_cast<int>(data.leftStickX) - 128) <= 8 && std::abs(static_cast<int>(data.leftStickY) - 128) <= 8);
    assert(std::abs(static_cast<int>(data.rightStickX) - 128) <= 8 && std::abs(static_cast<int>(data.rightStickY) - 128) <= 8);
    std::cout << "  -> Default neutral stick position verified (~128, ~128)" << std::endl;

    // Test simulated DualSense input injection (Cross + Triangle + R2 pressed)
    ScePadData simulated{};
    simulated.connected = 1;
    simulated.buttons = SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_TRIANGLE | SCE_PAD_BUTTON_R2;
    simulated.leftStickX = 220;
    simulated.leftStickY = 40;
    simulated.rightStickX = 180;
    simulated.rightStickY = 190;
    simulated.analogButtonsL2 = 0;
    simulated.analogButtonsR2 = 250;

    pad.SetSimulatedInput(handle, simulated);

    ScePadData readData{};
    assert(pad.ReadState(handle, &readData) == SCE_OK);
    assert((readData.buttons & SCE_PAD_BUTTON_CROSS) != 0);
    assert((readData.buttons & SCE_PAD_BUTTON_TRIANGLE) != 0);
    assert((readData.buttons & SCE_PAD_BUTTON_R2) != 0);
    assert(readData.leftStickX == 220 && readData.leftStickY == 40);
    assert(readData.analogButtonsR2 == 250);
    std::cout << "  -> Injected Cross+Triangle+R2 buttons & analog values verified" << std::endl;

    // Test DualSense Haptic Rumble & Vibration
    ScePadVibrationParam vib{180, 90};
    assert(pad.SetVibration(handle, &vib) == SCE_OK);
    ScePadVibrationParam currentVib{};
    assert(pad.GetCurrentVibration(handle, currentVib));
    assert(currentVib.largeMotor == 180 && currentVib.smallMotor == 90);
    std::cout << "  -> DualSense Vibration motors verified (Large: 180, Small: 90)" << std::endl;

    // Test DualSense Lightbar RGB
    ScePadColor color{255, 64, 128, 255};
    assert(pad.SetLightBar(handle, &color) == SCE_OK);
    ScePadColor currentColor{};
    assert(pad.GetCurrentColor(handle, currentColor));
    assert(currentColor.r == 255 && currentColor.g == 64 && currentColor.b == 128);
    std::cout << "  -> DualSense Lightbar color verified (RGB: 255, 64, 128)" << std::endl;

    // Close Pad
    assert(pad.Close(handle) == SCE_OK);
    std::cout << "  [PASS] DualSense Gamepad Subsystem passed!" << std::endl;
}

void TestAudioOutput() {
    std::cout << "[Test 2] Testing Multi-Channel Audio Engine (sailor_audio)..." << std::endl;
    AudioOutManager& audio = AudioOutManager::Instance();

    assert(audio.Init() == SCE_OK);

    // Open stereo 48kHz port
    const i32 stereoHandle = audio.Open(1, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 512, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    assert(stereoHandle > 0);
    std::cout << "  -> Opened Stereo AudioOut handle=" << stereoHandle << std::endl;

    SceAudioOutPortInfo info{};
    assert(audio.GetPortInfo(stereoHandle, &info) == SCE_OK);
    assert(info.channels == 2);
    assert(info.sampleRate == 48000);
    assert(info.sampleCount == 512);

    // Synthesize 512 stereo samples (440Hz concert A tone)
    std::vector<i16> toneBuffer(512 * 2);
    for (size_t i = 0; i < 512; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        const i16 sample = static_cast<i16>(3000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * t));
        toneBuffer[i * 2 + 0] = sample; // Left
        toneBuffer[i * 2 + 1] = sample; // Right
    }

    assert(audio.Output(stereoHandle, toneBuffer.data()) == SCE_OK);

    std::vector<u8> lastData;
    assert(audio.GetLastOutputData(stereoHandle, lastData));
    assert(lastData.size() == 512 * 2 * sizeof(i16));
    assert(std::memcmp(lastData.data(), toneBuffer.data(), lastData.size()) == 0);
    std::cout << "  -> Output 512 stereo samples (2048 bytes) matched bit-for-bit" << std::endl;

    // Test volume control
    const i32 volumes[2] = {32768, 16384}; // 100% Left, 50% Right
    assert(audio.SetVolume(stereoHandle, 0, volumes) == SCE_OK);

    // Test 5.1 Surround Sound Port
    const i32 surroundHandle = audio.Open(1, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_5DOT1);
    assert(surroundHandle > 0);
    SceAudioOutPortInfo surroundInfo{};
    assert(audio.GetPortInfo(surroundHandle, &surroundInfo) == SCE_OK);
    assert(surroundInfo.channels == 6);
    assert(surroundInfo.sampleRate == 48000);
    std::cout << "  -> Opened 5.1 Surround Sound AudioOut handle=" << surroundHandle << " (6 channels)" << std::endl;

    assert(audio.Close(stereoHandle) == SCE_OK);
    assert(audio.Close(surroundHandle) == SCE_OK);
    assert(audio.GetOpenPortCount() == 0);

    std::cout << "  [PASS] Multi-Channel Audio Engine passed!" << std::endl;
}

void TestAvPlayer() {
    std::cout << "[Test 3] Testing Video Cutscene Player (sailor_media / libSceAvPlayer)..." << std::endl;
    AvPlayerManager& av = AvPlayerManager::Instance();

    // Initialize player
    const i32 playerHandle = av.Init(nullptr);
    assert(playerHandle > 0);
    std::cout << "  -> Initialized AvPlayer handle=" << playerHandle << std::endl;

    // Add MP4 cutscene source
    assert(av.AddSource(playerHandle, "/app0/movies/intro_cinematic.mp4") == SCE_OK);
    assert(av.GetPlayerState(playerHandle) == AvPlayerState::Idle);

    // Start playback
    assert(av.Start(playerHandle) == SCE_OK);
    assert(av.IsActive(playerHandle));
    assert(av.GetPlayerState(playerHandle) == AvPlayerState::Playing);
    std::cout << "  -> Started video playback" << std::endl;

    // Retrieve video frames
    SceAvPlayerVideoData videoData{};
    assert(av.GetVideoData(playerHandle, &videoData));
    assert(videoData.width == 1920 && videoData.height == 1080);
    assert(videoData.pitch == 1920 * 4);
    assert(videoData.pData != nullptr);
    assert(videoData.timestamp > 0);
    std::cout << "  -> Decoded 1080p video frame (1920x1080 RGBA8, timestamp=" << videoData.timestamp << "us)" << std::endl;

    // Retrieve audio data
    SceAvPlayerAudioData audioData{};
    assert(av.GetAudioData(playerHandle, &audioData));
    assert(audioData.channelNum == 2 && audioData.sampleRate == 48000);
    assert(audioData.pData != nullptr && audioData.size > 0);
    std::cout << "  -> Decoded synchronized audio stream (" << audioData.sampleRate << "Hz, " << audioData.size << " bytes)" << std::endl;

    // Pause & Resume
    assert(av.Pause(playerHandle) == SCE_OK);
    assert(av.GetPlayerState(playerHandle) == AvPlayerState::Paused);
    assert(av.IsActive(playerHandle));

    assert(av.Start(playerHandle) == SCE_OK);
    assert(av.GetPlayerState(playerHandle) == AvPlayerState::Playing);

    // Stop playback
    assert(av.Stop(playerHandle) == SCE_OK);
    assert(av.GetPlayerState(playerHandle) == AvPlayerState::Stopped);
    assert(!av.IsActive(playerHandle));

    // Close player
    assert(av.Close(playerHandle) == SCE_OK);
    assert(av.GetActivePlayersCount() == 0);

    std::cout << "  [PASS] Video Cutscene Player passed!" << std::endl;
}

int main() {
    std::cout << "=================================================" << std::endl;
    std::cout << " SailorBox Phase 5: Input, Audio & Media Tests" << std::endl;
    std::cout << "=================================================" << std::endl;

    try {
        TestDualSenseInput();
        TestAudioOutput();
        TestAvPlayer();

        std::cout << "\n>>> ALL PHASE 5 TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED]: " << e.what() << std::endl;
        return 1;
    }
}
