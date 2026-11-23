#include "tools/GamePatch.hpp"
#include "tools/StubGenerator.hpp"
#include "common/Nid.hpp"
#include "common/Types.hpp"

#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstring>

using namespace Sailor;
using namespace Sailor::Tools;

void TestGamePatchEngine() {
    std::cout << "[Test 1] Testing Game Patch Engine (patches.toml)..." << std::endl;
    PatchEngine engine;

    const std::string tomlContent = R"(
# Sample SailorBox Game Patches
[patches.unlock_60fps]
title_id = "PPSA01234" # Game Title
address = 0x140000100
type = "u32" # 32-bit integer
enabled = true # active patch
expected = 0x0000001E # 30 fps
value = 0x0000003C    # 60 fps
description = "Unlocks framerate to 60 FPS"

[patches.skip_logos]
title_id = "*"
address = 0x140000200
type = "bytes" # Byte array
expected = "48 83 EC 28" # sub rsp, 40
value = "C3 90 90 90" # ret; nop; nop; nop
description = "Fast skip intro video logos"
)";

    assert(engine.LoadFromString(tomlContent));
    assert(engine.GetPatches().size() == 2);
    std::cout << "  -> Loaded 2 patches from TOML string" << std::endl;

    // Create a 1024-byte virtual image buffer simulating loaded game executable
    std::vector<u8> memory(1024, 0);
    const u64 baseAddress = 0x140000000ULL;

    // Set up original 30 FPS at 0x100
    const u32 originalFps = 30; // 0x1E
    std::memcpy(memory.data() + 0x100, &originalFps, sizeof(originalFps));

    // Set up original bytes at 0x200: "48 83 EC 28"
    const u8 originalIntroCode[4] = {0x48, 0x83, 0xEC, 0x28};
    std::memcpy(memory.data() + 0x200, originalIntroCode, sizeof(originalIntroCode));

    // Apply patches matching title "PPSA01234"
    size_t applied = engine.ApplyPatches(memory, baseAddress, "PPSA01234");
    assert(applied == 2);

    // Verify 60 FPS patched (0x3C = 60)
    u32 patchedFps = 0;
    std::memcpy(&patchedFps, memory.data() + 0x100, sizeof(patchedFps));
    assert(patchedFps == 60);
    std::cout << "  -> 60 FPS patch successfully modified framerate: 30 -> 60" << std::endl;

    // Verify bytes patch applied: "C3 90 90 90" (ret, nop, nop, nop)
    const u8 expectedPatchedCode[4] = {0xC3, 0x90, 0x90, 0x90};
    assert(std::memcmp(memory.data() + 0x200, expectedPatchedCode, 4) == 0);
    std::cout << "  -> Intro skip patch successfully wrote ret/nop machine code" << std::endl;

    // Test corruption prevention: mismatched expected bytes must abort
    PatchEngine badEngine;
    PatchEntry badPatch;
    badPatch.name = "corrupt_test";
    badPatch.address = 0x140000100;
    badPatch.expected = {0xFF, 0xFF, 0xFF, 0xFF}; // Wrong expected
    badPatch.replacement = {0xAA, 0xBB, 0xCC, 0xDD};
    badEngine.AddPatch(badPatch);

    size_t badApplied = badEngine.ApplyPatches(memory, baseAddress, "*");
    assert(badApplied == 0); // Must be rejected
    std::memcpy(&patchedFps, memory.data() + 0x100, sizeof(patchedFps));
    assert(patchedFps == 60); // Must not be corrupted
    std::cout << "  -> Corruption guard verified: rejected mismatched patch" << std::endl;

    // Test on-disk PE image RVA -> PointerToRawData translation
    std::vector<u8> peImage(0x1000, 0);
    peImage[0] = 'M'; peImage[1] = 'Z';
    const u32 peOffset = 0x80;
    *reinterpret_cast<u32*>(peImage.data() + 0x3C) = peOffset;
    peImage[peOffset] = 'P'; peImage[peOffset + 1] = 'E';
    *reinterpret_cast<u16*>(peImage.data() + peOffset + 6) = 1; // 1 section
    *reinterpret_cast<u16*>(peImage.data() + peOffset + 20) = 240; // opt size
    *reinterpret_cast<u32*>(peImage.data() + peOffset + 24 + 60) = 0x200; // sizeOfHeaders
    const std::size_t secOff = peOffset + 24 + 240;
    *reinterpret_cast<u32*>(peImage.data() + secOff + 8) = 0x1000; // vSize
    *reinterpret_cast<u32*>(peImage.data() + secOff + 12) = 0x1000; // vAddr
    *reinterpret_cast<u32*>(peImage.data() + secOff + 16) = 0x200; // rawSize
    *reinterpret_cast<u32*>(peImage.data() + secOff + 20) = 0x200; // rawPtr
    peImage[0x250] = 0x11;

    PatchEngine peEngine;
    PatchEntry diskPatch;
    diskPatch.name = "disk_pe_patch";
    diskPatch.address = 0x140001050; // RVA 0x1050 -> file offset 0x250
    diskPatch.type = PatchType::U8;
    diskPatch.expected = {0x11};
    diskPatch.replacement = {0x77};
    peEngine.AddPatch(diskPatch);

    size_t peApplied = peEngine.ApplyPatches(peImage, baseAddress, "*");
    assert(peApplied == 1);
    assert(peImage[0x250] == 0x77);
    std::cout << "  -> On-disk PE RVA-to-raw translation verified: patched raw offset 0x250" << std::endl;

    std::cout << "  [PASS] Game Patch Engine passed!" << std::endl;
}

void TestStubGenerator() {
    std::cout << "[Test 2] Testing Automated Stub Generator (stubgen)..." << std::endl;

    // Create a temporary unhandled_apis.log with duplicates
    const std::filesystem::path tempLog = std::filesystem::temp_directory_path() / "test_unhandled_apis.log";
    {
        std::ofstream file(tempLog);
        file << "[STUB] Module: libSceNpManager, Function: sceNpManagerGetStatus, NID: d45f6a7b8c9\n"
             << "[STUB] Module: libSceNpManager, Function: sceNpManagerGetStatus, NID: d45f6a7b8c9\n"
             << "[STUB] Module: libSceUserService, Function: sceUserServiceGetInitialUser, NID: a1b2c3d4e5f\n"
             << "[STUB] Module: libSceUserService, Function: sceUserServiceGetInitialUser, NID: a1b2c3d4e5f\n";
    }

    StubGenerator gen;
    size_t count = gen.ParseLogFile(tempLog.string());
    assert(count == 2);
    assert(gen.EntryCount() == 2);
    std::cout << "  -> Parsed 2 unique unhandled APIs from log (deduplication verified)" << std::endl;

    // Generate header and source strings
    std::string hCode = gen.GenerateHeader("libSceNpManager");
    std::string cCode = gen.GenerateSource("libSceNpManager");

    assert(hCode.find("SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpManagerGetStatus(void);") != std::string::npos);
    assert(cCode.find("SB_LOG_STUB(\"libSceNpManager\", \"sceNpManagerGetStatus\"") != std::string::npos);
    assert(cCode.find("return SCE_OK;") != std::string::npos);
    std::cout << "  -> Verified generated C++ header & source signatures" << std::endl;

    // Test writing to disk
    const std::filesystem::path tempOutDir = std::filesystem::temp_directory_path() / "SailorBox_StubGenTest";
    assert(gen.GenerateFiles("libSceNpManager", tempOutDir.string()));
    assert(std::filesystem::exists(tempOutDir / "libSceNpManager.hpp"));
    assert(std::filesystem::exists(tempOutDir / "libSceNpManager.cpp"));
    std::cout << "  -> Verified generated files on disk: " << tempOutDir.string() << std::endl;

    // Cleanup
    std::error_code ec;
    std::filesystem::remove(tempLog, ec);
    std::filesystem::remove_all(tempOutDir, ec);

    // Test parameter parsing with argument count & signatures
    const std::filesystem::path tempLogArgs = std::filesystem::temp_directory_path() / "test_unhandled_args.log";
    {
        std::ofstream file(tempLogArgs);
        file << "[STUB] Module: libScePad, Function: scePadSetVibration, NID: e1f2a3b4c5d, Args: 2\n";
    }
    StubGenerator genArgs;
    assert(genArgs.ParseLogFile(tempLogArgs.string()) == 1);
    std::string hCodeArgs = genArgs.GenerateHeader("libScePad");
    std::string cCodeArgs = genArgs.GenerateSource("libScePad");
    assert(hCodeArgs.find("SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePadSetVibration(u64 arg1, u64 arg2);") != std::string::npos);
    assert(cCodeArgs.find("(void)arg1;") != std::string::npos);
    assert(cCodeArgs.find("(void)arg2;") != std::string::npos);
    std::filesystem::remove(tempLogArgs, ec);
    std::cout << "  -> Parameter signature parsing verified (u64 arg1, u64 arg2)!" << std::endl;

    std::cout << "  [PASS] Automated Stub Generator passed!" << std::endl;
}

void TestNidDatabase() {
    std::cout << "[Test 3] Testing Sony NID Symbol Resolution..." << std::endl;

    const std::string sym = "sceKernelExitProcess";
    const Nid nid = Nid::Compute(sym);
    const u64 nid64 = nid.Value();
    assert(nid64 != 0);

    const std::string nidStr = nid.ToString();
    assert(nidStr.size() == 11);

    std::cout << "  -> Symbol: " << sym << " -> 64-bit NID: 0x"
              << std::hex << nid64 << std::dec << " -> 11-char NID: " << nidStr << std::endl;

    Nid decoded(nidStr);
    assert(decoded.Value() == nid64);
    std::cout << "  [PASS] Sony NID Symbol Resolution passed!" << std::endl;
}

int main() {
    std::cout << "=================================================" << std::endl;
    std::cout << " SailorBox Phase 6: Developer Tooling Test Suite" << std::endl;
    std::cout << "=================================================" << std::endl;

    try {
        TestGamePatchEngine();
        TestStubGenerator();
        TestNidDatabase();

        std::cout << "\n>>> ALL PHASE 6 TOOLING TESTS PASSED! <<<\n" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED]: " << e.what() << std::endl;
        return 1;
    }
}
