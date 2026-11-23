#include "audio/AudioOut.hpp"
#include "audio/Audio3d.hpp"
#include "media/AvPlayer.hpp"
#include "services/UserService.hpp"
#include "services/SaveData.hpp"
#include "services/NpTrophy.hpp"
#include "services/PlayGo.hpp"
#include "kernel/VirtualFileSystem.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <filesystem>
#include <cstring>

using namespace Sailor;

void TestAudioAndTempest3D() {
    std::cout << "[Test 1] Testing WASAPI Audio & Tempest 3D Binaural Subsystems..." << std::endl;

    // 1. AudioOut Engine with WASAPI ring buffer / hardware pacing
    AudioOutManager& audio = AudioOutManager::Instance();
    assert(audio.Init() == SCE_OK);
    audio.SetPacingEnabled(false); // disable sleep pacing for high-speed unit testing

    const i32 handle = audio.Open(1, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    assert(handle > 0);

    std::vector<i16> stereoBuffer(256 * 2, 1000); // 256 stereo samples
    assert(audio.Output(handle, stereoBuffer.data()) == SCE_OK);

    std::vector<u8> outputData;
    assert(audio.GetLastOutputData(handle, outputData));
    assert(outputData.size() == 256 * 2 * sizeof(i16));
    assert(audio.Close(handle) == SCE_OK);
    std::cout << "  -> AudioOutManager verified with WASAPI pipeline (isWasapiActive=" << audio.IsWasapiActive() << ")" << std::endl;

    // 2. Tempest 3D Audio Engine & Binaural HRTF Downmixer
    Audio3dManager& audio3d = Audio3dManager::Instance();
    assert(audio3d.Initialize(0) == SCE_OK);

    SceAudio3dPortAttribute attr{};
    assert(audio3d.GetDefaultAttributes(&attr) == SCE_OK);
    assert(attr.maxObjects == 32);
    assert(attr.sampleRate == 48000);

    SceAudio3dPortId portId = 0;
    assert(audio3d.PortCreate(&attr, &portId) == SCE_OK);
    assert(portId > 0);

    // Reserve 3D object on the right side: X = +10.0, Y = 0.0, Z = 5.0
    SceAudio3dObjectId rightObjId = 0;
    assert(audio3d.ObjectReserve(portId, &rightObjId) == SCE_OK);
    SceAudio3dPosition rightPos{10.0f, 0.0f, 5.0f};
    assert(audio3d.ObjectSetPosition(portId, rightObjId, &rightPos) == SCE_OK);
    assert(audio3d.ObjectSetVolume(portId, rightObjId, 1.0f) == SCE_OK);

    // Push test sine burst to right 3D audio object
    std::vector<float> monoAudio(256, 0.8f);
    assert(audio3d.ObjectPushAudio(portId, rightObjId, monoAudio.data(), 256) == SCE_OK);

    // Advance port to compute Tempest 3D binaural downmix
    assert(audio3d.PortAdvance(portId) == SCE_OK);

    std::vector<float> binauralData;
    assert(audio3d.GetLastBinauralOutput(portId, binauralData));
    assert(binauralData.size() == 256 * 2);

    // Verify spatial HRTF/ILD properties: object on right (+X) must produce stronger Right channel than Left channel
    float leftEnergy = 0.0f;
    float rightEnergy = 0.0f;
    for (size_t i = 0; i < 256; ++i) {
        leftEnergy += std::abs(binauralData[i * 2 + 0]);
        rightEnergy += std::abs(binauralData[i * 2 + 1]);
    }

    assert(rightEnergy > leftEnergy);
    std::cout << "  -> Spatial HRTF ILD verified: Right channel energy (" << rightEnergy 
              << ") > Left channel energy (" << leftEnergy << ")" << std::endl;

    // Test 7.1 Bed Channels Downmixing
    std::vector<float> bedPcm(256 * 8, 0.1f);
    assert(audio3d.BedWrite(portId, bedPcm.data(), static_cast<u32>(bedPcm.size() * sizeof(float))) == SCE_OK);
    assert(audio3d.PortAdvance(portId) == SCE_OK);
    std::cout << "  -> 7.1 surround sound bed downmixed to binaural stereo successfully" << std::endl;

    assert(audio3d.PortDestroy(portId) == SCE_OK);
    assert(audio3d.GetActivePortCount() == 0);
    std::cout << "  [PASS] WASAPI Audio & Tempest 3D Subsystems passed!" << std::endl;
}

void TestAvPlayerDemuxer() {
    std::cout << "[Test 2] Testing Media Foundation Cutscene Demuxer Pipeline..." << std::endl;
    AvPlayerManager& av = AvPlayerManager::Instance();

    const i32 handle = av.Init(nullptr);
    assert(handle > 0);

    // Add video source
    assert(av.AddSource(handle, "/app0/movies/intro_logo.mp4") == SCE_OK);
    assert(av.Start(handle) == SCE_OK);
    assert(av.IsActive(handle));

    // Retrieve video frames
    SceAvPlayerVideoData videoData{};
    assert(av.GetVideoData(handle, &videoData));
    assert(videoData.width > 0 && videoData.height > 0);
    assert(videoData.pData != nullptr);
    assert(videoData.pitch == videoData.width * 4);

    // Retrieve audio data
    SceAvPlayerAudioData audioData{};
    assert(av.GetAudioData(handle, &audioData));
    assert(audioData.channelNum == 2 && audioData.sampleRate == 48000);
    assert(audioData.pData != nullptr && audioData.size > 0);

    assert(av.Stop(handle) == SCE_OK);
    assert(!av.IsActive(handle));
    assert(av.Close(handle) == SCE_OK);
    std::cout << "  [PASS] Media Foundation Cutscene Demuxer Pipeline passed!" << std::endl;
}

void TestUserService() {
    std::cout << "[Test 3] Testing Core SCE Service: libSceUserService..." << std::endl;
    UserServiceManager& user = UserServiceManager::Instance();

    assert(user.Initialize(nullptr) == SCE_OK);
    assert(user.IsInitialized());

    // Verify initial user ID is 1 (standard PlayStation primary logged-in user)
    SceUserServiceUserId userId = 0;
    assert(user.GetInitialUser(&userId) == SCE_OK);
    assert(userId == 1);
    std::cout << "  -> Verified initial logged-in user ID = " << userId << std::endl;

    // Verify login user list
    SceUserServiceLoginUserIdList list{};
    assert(user.GetLoginUserIdList(&list) == SCE_OK);
    assert(list.userId[0] == 1);
    assert(list.userId[1] == -1);

    // Verify username
    char userName[32]{0};
    assert(user.GetUserName(1, userName, sizeof(userName)) == SCE_OK);
    assert(std::strcmp(userName, "SailorUser") == 0);
    std::cout << "  -> Verified user profile username = '" << userName << "'" << std::endl;

    // Verify foreground user and pad mapping
    SceUserServiceUserId fgUser = 0;
    assert(user.GetForegroundUser(&fgUser) == SCE_OK);
    assert(fgUser == 1);

    i32 padId = -1;
    assert(user.GetPadIdByUserId(1, &padId) == SCE_OK);
    assert(padId == 0);

    assert(user.Terminate() == SCE_OK);
    std::cout << "  [PASS] libSceUserService passed!" << std::endl;
}

void TestSaveData() {
    std::cout << "[Test 4] Testing Core SCE Service: libSceSaveData..." << std::endl;
    SaveDataManager& save = SaveDataManager::Instance();

    assert(save.Initialize(nullptr) == SCE_OK);

    // Test mounting savedata directory
    SceSaveDataMount mount{};
    mount.userId = 1;
    mount.dirName = "SaveSlot01";
    mount.blocks = 1024;
    mount.mountMode = 0;

    SceSaveDataMountResult result{};
    assert(save.Mount(&mount, &result) == SCE_OK);

    // Verify mount point routed to /savedata
    assert(std::strcmp(result.mountPoint.data, "/savedata") == 0);
    std::cout << "  -> Verified SaveData mount routed to guest path: '" << result.mountPoint.data << "'" << std::endl;

    // Verify local host directory created on disk
    std::filesystem::path hostSaveDir = save.GetHostSaveDirectory();
    std::filesystem::path expectedDir = hostSaveDir / "SaveSlot01";
    assert(std::filesystem::exists(expectedDir));
    std::cout << "  -> Verified host save directory exists on disk: '" << expectedDir.string() << "'" << std::endl;

    // Unmount
    assert(save.Umount(&result.mountPoint) == SCE_OK);
    assert(save.GetActiveMountCount() == 0);

    assert(save.Terminate() == SCE_OK);
    std::cout << "  [PASS] libSceSaveData passed!" << std::endl;
}

void TestNpTrophy() {
    std::cout << "[Test 5] Testing Core SCE Service: libSceNpTrophy2..." << std::endl;
    NpTrophyManager& trophy = NpTrophyManager::Instance();

    SceNpTrophyContext context = 0;
    assert(trophy.CreateContext(&context, nullptr) == SCE_OK);
    assert(context > 0);

    SceNpTrophyHandle handle = 0;
    assert(trophy.CreateHandle(&handle) == SCE_OK);
    assert(handle > 0);

    // Unlock Bronze Trophy #1
    u64 platId = 0;
    assert(trophy.UnlockTrophy(context, handle, 1, &platId) == SCE_OK);
    assert(trophy.IsTrophyUnlocked(1));

    // Unlock Gold Trophy #7
    assert(trophy.UnlockTrophy(context, handle, 7, &platId) == SCE_OK);
    assert(trophy.IsTrophyUnlocked(7));
    assert(!trophy.IsTrophyUnlocked(2)); // Trophy 2 not yet unlocked
    assert(trophy.GetUnlockedTrophyCount() == 2);

    // Check bitflag status
    SceNpTrophyFlagArray flags{};
    u32 count = 0;
    assert(trophy.GetTrophyUnlockState(context, handle, &flags, &count) == SCE_OK);
    assert(count == 2);
    assert((flags.flagBits[0] & (1u << 1)) != 0); // Trophy 1
    assert((flags.flagBits[0] & (1u << 7)) != 0); // Trophy 7

    assert(trophy.DestroyHandle(handle) == SCE_OK);
    assert(trophy.DestroyContext(context) == SCE_OK);
    std::cout << "  [PASS] libSceNpTrophy2 passed!" << std::endl;
}

void TestPlayGo() {
    std::cout << "[Test 6] Testing Core SCE Service: libScePlayGo..." << std::endl;
    PlayGoManager& playgo = PlayGoManager::Instance();

    assert(playgo.Initialize(nullptr) == SCE_OK);
    assert(playgo.IsInitialized());

    // Verify Chunk 0 (base chunk) is 100% installed
    ScePlayGoChunkStatus chunk0{};
    assert(playgo.GetChunkStatus(0, &chunk0) == SCE_OK);
    assert(chunk0.chunkId == 0);
    assert(chunk0.progress >= 100.0f);
    assert(chunk0.state == 2); // 2 = Installed

    // Verify Chunk 1 (high resolution textures) is 100% installed
    ScePlayGoChunkStatus chunk1{};
    assert(playgo.GetChunkStatus(1, &chunk1) == SCE_OK);
    assert(chunk1.chunkId == 1);
    assert(chunk1.progress >= 100.0f);
    assert(chunk1.state == 2); // 2 = Installed
    std::cout << "  -> Verified Game Chunks 0 & 1 reported as 100% installed (state=2)" << std::endl;

    // Verify overall progress
    ScePlayGoProgress progress{};
    assert(playgo.GetProgress(&progress) == SCE_OK);
    assert(progress.totalProgress >= 100.0f);
    assert(progress.installedBytes == progress.totalBytes);

    u64 installSpeed = 0;
    assert(playgo.GetInstallSpeed(&installSpeed) == SCE_OK);
    assert(installSpeed >= 100'000'000ULL);

    u32 locus = 999;
    assert(playgo.GetLocus(0, &locus) == SCE_OK);
    assert(locus == 0); // Local NVMe storage

    assert(playgo.Terminate() == SCE_OK);
    std::cout << "  [PASS] libScePlayGo passed!" << std::endl;
}

int main() {
    std::cout << "=================================================" << std::endl;
    std::cout << " SailorBox Stage 7: Audio, Video & SCE Services" << std::endl;
    std::cout << "=================================================" << std::endl;

    try {
        TestAudioAndTempest3D();
        TestAvPlayerDemuxer();
        TestUserService();
        TestSaveData();
        TestNpTrophy();
        TestPlayGo();

        std::cout << "\n>>> ALL STAGE 7 TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED]: " << e.what() << std::endl;
        return 1;
    }
}
