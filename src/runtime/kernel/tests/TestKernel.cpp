#include "kernel/DirectMemory.hpp"
#include "kernel/SyncPrimitives.hpp"
#include "kernel/VirtualFileSystem.hpp"
#include "kernel/ThreadManager.hpp"
#include "kernel/FiberManager.hpp"
#include "kernel/Decompressor.hpp"
#include "kernel/PadManager.hpp"
#include "kernel/SaveDataManager.hpp"
#include "kernel/AudioOutManager.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

void TestDirectMemory() {
    std::cout << "[TEST] Running DirectMemory tests...\n";
    auto& dm = Sailor::DirectMemoryManager::Instance();

    Sailor::u64 physOffset = 0;
    Sailor::i32 res = dm.Allocate(0x200000, 0x10000, 0, &physOffset);
    assert(res == Sailor::SCE_OK);
    assert(physOffset > 0);

    // 1. Map base block
    void* virtAddr = nullptr;
    res = dm.Map(&virtAddr, 0x100000, 3, 0, physOffset, 0x10000);
    assert(res == Sailor::SCE_OK);
    assert(virtAddr != nullptr);

    // Write to mapped memory
    std::memset(virtAddr, 0xAA, 0x1000);
    assert(*static_cast<Sailor::u8*>(virtAddr) == 0xAA);

    // 2. Test sub-block offset mapping (offset + 0x10000)
    void* subVirtAddr = nullptr;
    res = dm.Map(&subVirtAddr, 0x10000, 3, 0, physOffset + 0x10000, 0x10000);
    assert(res == Sailor::SCE_OK);
    assert(subVirtAddr != nullptr);
    std::memset(subVirtAddr, 0xBB, 0x1000);
    assert(*static_cast<Sailor::u8*>(subVirtAddr) == 0xBB);

    res = dm.Unmap(subVirtAddr, 0x10000);
    assert(res == Sailor::SCE_OK);

    // 3. Test sub-64KB offset mapping (16 KB PS5 page granularity: offset + 0x4000)
    void* sub16kAddr = nullptr;
    res = dm.Map(&sub16kAddr, 0x4000, 3, 0, physOffset + 0x4000, 0x4000);
    assert(res == Sailor::SCE_OK);
    assert(sub16kAddr != nullptr);
    std::memset(sub16kAddr, 0xCC, 0x1000);
    assert(*static_cast<Sailor::u8*>(sub16kAddr) == 0xCC);

    res = dm.Unmap(sub16kAddr, 0x4000);
    assert(res == Sailor::SCE_OK);

    // 4. Test SCE_KERNEL_MAP_FIXED with sub-64KB offset
    void* fixedProbe = VirtualAlloc(nullptr, 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    if (fixedProbe) {
        VirtualFree(fixedProbe, 0, MEM_RELEASE);
        void* requestedFixedAddr = static_cast<Sailor::u8*>(fixedProbe) + 0x4000;
        void* outFixedAddr = requestedFixedAddr;
        res = dm.Map(&outFixedAddr, 0x4000, 3, 0x10 /* SCE_KERNEL_MAP_FIXED */, physOffset + 0x4000, 0x4000);
        assert(res == Sailor::SCE_OK);
        assert(outFixedAddr == requestedFixedAddr); // Must land on exact requested address!
        std::memset(outFixedAddr, 0xDD, 0x1000);
        assert(*static_cast<Sailor::u8*>(outFixedAddr) == 0xDD);
        res = dm.Unmap(outFixedAddr, 0x4000);
        assert(res == Sailor::SCE_OK);
    }

    // 5. Test multiple sub-allocations sharing base mapping and out-of-order unmapping
    void* sliceA = nullptr;
    void* sliceB = nullptr;
    res = dm.Map(&sliceA, 0x4000, 3, 0, physOffset + 0x4000, 0x4000);
    assert(res == Sailor::SCE_OK);
    res = dm.Map(&sliceB, 0x4000, 3, 0, physOffset + 0x8000, 0x4000);
    assert(res == Sailor::SCE_OK);
    std::memset(sliceA, 0x11, 0x1000);
    std::memset(sliceB, 0x22, 0x1000);
    assert(*static_cast<Sailor::u8*>(sliceA) == 0x11);
    assert(*static_cast<Sailor::u8*>(sliceB) == 0x22);

    // Unmap sliceB first (out-of-order), verify sliceA remains completely valid
    res = dm.Unmap(sliceB, 0x4000);
    assert(res == Sailor::SCE_OK);
    assert(*static_cast<Sailor::u8*>(sliceA) == 0x11);

    // Now unmap sliceA
    res = dm.Unmap(sliceA, 0x4000);
    assert(res == Sailor::SCE_OK);

    // Test mprotect
    res = dm.Mprotect(virtAddr, 0x100000, 1); // Read-only
    assert(res == Sailor::SCE_OK);

    // Test mprotect with PROT_WRITE-only (implies read-write)
    res = dm.Mprotect(virtAddr, 0x100000, 2);
    assert(res == Sailor::SCE_OK);

    // Test mprotect with GPU-only flags (SCE_KERNEL_PROT_GPU_READ | SCE_KERNEL_PROT_GPU_WRITE = 0x30)
    res = dm.Mprotect(virtAddr, 0x100000, 0x30);
    assert(res == Sailor::SCE_OK);

    res = dm.Unmap(virtAddr, 0x100000);
    assert(res == Sailor::SCE_OK);

    res = dm.Release(physOffset);
    assert(res == Sailor::SCE_OK);

    // 4. Test VirtualAlloc2 Virtual Range Reservation & Flexible Memory Mapping
    void* reservedRange = nullptr;
    res = dm.ReserveVirtualRange(&reservedRange, 0x1000000 /* 16 MB */, 0, 0x10000);
    assert(res == Sailor::SCE_OK);
    assert(reservedRange != nullptr);

    void* flexibleMem = reservedRange;
    res = dm.MapFlexibleMemory(&flexibleMem, 0x20000 /* 128 KB */, 3, 0);
    assert(res == Sailor::SCE_OK);
    assert(flexibleMem != nullptr);
    std::memset(flexibleMem, 0x77, 0x1000);
    assert(*static_cast<Sailor::u8*>(flexibleMem) == 0x77);

    std::cout << "[TEST] DirectMemory tests passed!\n";
}

void TestVirtualFileSystem() {
    std::cout << "[TEST] Running VirtualFileSystem tests...\n";
    auto& vfs = Sailor::VirtualFileSystem::Instance();

    // Test path resolution
    auto appPath = vfs.ResolvePath("/app0/media/level1.dat");
    assert(appPath.string().find("level1.dat") != std::string::npos);

    auto savePath = vfs.ResolvePath("/savedata/user0.dat");
    assert(savePath.string().find("savedata") != std::string::npos);

    // Test file open/write/read/close
    const char* testGuestPath = "/savedata/test_file.bin";
    Sailor::i32 fd = vfs.Open(testGuestPath, 0x0002 | 0x0200 | 0x0400, 0666); // O_RDWR | O_CREAT | O_TRUNC
    assert(fd >= 10);

    const char testData[] = "SailorBox VFS Verification Payload";
    Sailor::i64 written = vfs.Write(fd, testData, sizeof(testData));
    assert(written == sizeof(testData));

    Sailor::i64 seekPos = vfs.Lseek(fd, 0, 0); // Seek to beginning
    assert(seekPos == 0);

    char readBuf[64] = {0};
    Sailor::i64 readBytes = vfs.Read(fd, readBuf, sizeof(readBuf));
    assert(readBytes == sizeof(testData));
    assert(std::strcmp(readBuf, testData) == 0);

    // Test Pwrite and Pread (must NOT mutate current file pointer)
    const char pwriteData[] = "OVERRIDE";
    Sailor::i64 currentPosBefore = vfs.Lseek(fd, 5, 0); // Seek to position 5
    assert(currentPosBefore == 5);

    Sailor::i64 pwWritten = vfs.Pwrite(fd, pwriteData, 8, 10);
    assert(pwWritten == 8);
    Sailor::i64 currentPosAfterPwrite = vfs.Lseek(fd, 0, 1); // Query current pos without moving
    assert(currentPosAfterPwrite == 5); // File pointer must remain untouched at 5!

    char preadBuf[16] = {0};
    Sailor::i64 prRead = vfs.Pread(fd, preadBuf, 8, 10);
    assert(prRead == 8);
    assert(std::memcmp(preadBuf, pwriteData, 8) == 0);
    Sailor::i64 currentPosAfterPread = vfs.Lseek(fd, 0, 1);
    assert(currentPosAfterPread == 5); // File pointer still remains untouched at 5!

    Sailor::SceKernelStat statBuf;
    std::memset(&statBuf, 0xEE, sizeof(statBuf));
    Sailor::i32 statRes = vfs.Fstat(fd, &statBuf);
    assert(statRes == Sailor::SCE_OK);
    assert(statBuf.st_size >= sizeof(testData));
    assert(statBuf.st_uid == 0 && statBuf.st_gid == 0);

    std::memset(&statBuf, 0xEE, sizeof(statBuf));
    statRes = vfs.Stat(testGuestPath, &statBuf);
    assert(statRes == Sailor::SCE_OK);
    assert(statBuf.st_size >= sizeof(testData));
    assert(statBuf.st_uid == 0 && statBuf.st_gid == 0);

    Sailor::i32 closeRes = vfs.Close(fd);
    assert(closeRes == Sailor::SCE_OK);

    // Test directory opening and Fstat (requires FILE_FLAG_BACKUP_SEMANTICS on Windows)
    Sailor::i32 dirFd = vfs.Open("/savedata", 0 /* O_RDONLY */, 0);
    assert(dirFd >= 10);
    std::memset(&statBuf, 0, sizeof(statBuf));
    Sailor::i32 dirStatRes = vfs.Fstat(dirFd, &statBuf);
    assert(dirStatRes == Sailor::SCE_OK);
    assert((statBuf.st_mode & 0040000) != 0); // S_IFDIR mode (0040777)
    assert(vfs.Close(dirFd) == Sailor::SCE_OK);

    // Test libSceApr asynchronous streaming decompression
    Sailor::u8 streamTarget[256];
    std::memset(streamTarget, 0xEE, sizeof(streamTarget));

    Sailor::AprCommand aprCmd{};
    aprCmd.fileOffset = 0x1000;
    aprCmd.destinationMemory = streamTarget;
    aprCmd.compressedSize = 128;
    aprCmd.uncompressedSize = 256;
    aprCmd.format = Sailor::AprCompressionFormat::Kraken;

    Sailor::AprCommandBuffer aprCmdBuf{};
    aprCmdBuf.commands = &aprCmd;
    aprCmdBuf.commandCount = 1;

    Sailor::SceAprSubmissionId aprSubId = 0;
    assert(vfs.SubmitAprCommandBuffer(&aprCmdBuf, &aprSubId) == Sailor::SCE_OK);
    assert(aprSubId > 0);
    assert(vfs.WaitApr(aprSubId) == Sailor::SCE_OK);
    assert(streamTarget[0] == 0); // Destination memory was filled

    std::cout << "[TEST] VirtualFileSystem tests passed!\n";
}

void TestSyncPrimitives() {
    std::cout << "[TEST] Running SyncPrimitives tests...\n";

    // 1. Mutex test
    Sailor::Mutex mutex(Sailor::MutexType::Normal, "TestMutex");
    assert(mutex.Lock() == Sailor::SCE_OK);
    assert(mutex.TryLock() == Sailor::SCE_KERNEL_ERROR_EBUSY); // Already locked by this thread
    assert(mutex.Unlock() == Sailor::SCE_OK);
    assert(mutex.TryLock() == Sailor::SCE_OK);
    assert(mutex.Unlock() == Sailor::SCE_OK);

    // 2. Semaphore test
    Sailor::Semaphore sem(0, 5, "TestSem");
    assert(sem.Poll(1) == Sailor::SCE_KERNEL_ERROR_EBUSY); // Initially 0 tokens
    assert(sem.Signal(2) == Sailor::SCE_OK);
    assert(sem.Poll(1) == Sailor::SCE_OK); // 1 token left
    assert(sem.Poll(1) == Sailor::SCE_OK); // 0 tokens left
    assert(sem.Poll(1) == Sailor::SCE_KERNEL_ERROR_EBUSY);

    // Semaphore Timed Wait test (verify remaining timeout modification)
    Sailor::u64 timeout = 5000; // 5ms
    assert(sem.Wait(1, &timeout) == Sailor::SCE_KERNEL_ERROR_ETIMEDOUT);
    assert(timeout == 0); // Timed out wait must zero the timeout

    assert(sem.Signal(1) == Sailor::SCE_OK);
    timeout = 100000; // 100ms
    assert(sem.Wait(1, &timeout) == Sailor::SCE_OK);
    assert(timeout > 0 && timeout <= 100000); // Successful wait must preserve remaining time!

    // 3. Condition variable cross-thread test
    Sailor::Mutex condMutex(Sailor::MutexType::Normal, "CondMutex");
    Sailor::ConditionVariable cond("TestCond");
    bool ready = false;

    std::thread worker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(condMutex.Lock() == Sailor::SCE_OK);
        ready = true;
        cond.Signal();
        assert(condMutex.Unlock() == Sailor::SCE_OK);
    });

    assert(condMutex.Lock() == Sailor::SCE_OK);
    while (!ready) {
        assert(cond.Wait(&condMutex) == Sailor::SCE_OK);
    }
    assert(ready == true);
    assert(condMutex.Unlock() == Sailor::SCE_OK);

    worker.join();

    // 4. SceKernelEqueue event ring test
    Sailor::EventQueue eq("GameEventLoopEqueue");
    assert(eq.AddUserEvent(101) == Sailor::SCE_OK);

    // Test timed wait on empty queue (immediate timeout)
    Sailor::SceKernelEvent ev{};
    int outCount = 0;
    Sailor::u64 zeroTimeout = 0;
    assert(eq.Wait(&ev, 1, &outCount, &zeroTimeout) == Sailor::SCE_KERNEL_ERROR_ETIMEDOUT);
    assert(outCount == 0);

    // Trigger user event and verify retrieval
    assert(eq.TriggerUserEvent(101, reinterpret_cast<void*>(0xC0FFEE)) == Sailor::SCE_OK);
    assert(eq.Wait(&ev, 1, &outCount, nullptr) == Sailor::SCE_OK);
    assert(outCount == 1);
    assert(ev.ident == 101);
    assert(ev.filter == Sailor::SCE_KERNEL_EVFILT_USER);
    assert(ev.udata == reinterpret_cast<void*>(0xC0FFEE));

    // Test external GPU EOP event push
    Sailor::SceKernelEvent gpuEv{};
    gpuEv.ident = 0x1000;
    gpuEv.filter = Sailor::SCE_KERNEL_EVFILT_GRAPHICS;
    gpuEv.data = 12345ULL; // Fence timestamp
    assert(eq.PushEvent(gpuEv) == Sailor::SCE_OK);
    assert(eq.Wait(&ev, 1, &outCount, nullptr) == Sailor::SCE_OK);
    assert(ev.filter == Sailor::SCE_KERNEL_EVFILT_GRAPHICS);
    assert(ev.data == 12345ULL);

    std::cout << "[TEST] SyncPrimitives tests passed!\n";
}

static void* SAILOR_SYSV_ABI TestGuestThreadEntry(void* arg) {
    auto val = reinterpret_cast<uintptr_t>(arg);
    return reinterpret_cast<void*>(val * 2);
}

void TestThreading() {
    std::cout << "[TEST] Running ThreadManager & SysV Trampoline tests...\n";
    auto& tm = Sailor::ThreadManager::Instance();

    Sailor::ScePthread thread = 0;
    Sailor::ScePthreadAttr attr{};
    Sailor::i32 res = tm.CreateThread(&thread, &attr, TestGuestThreadEntry, reinterpret_cast<void*>(21), "WorkerThread");
    assert(res == Sailor::SCE_OK);
    assert(thread != 0);

    void* retVal = nullptr;
    res = tm.JoinThread(thread, &retVal);
    assert(res == Sailor::SCE_OK);
    assert(reinterpret_cast<uintptr_t>(retVal) == 42);

    std::cout << "[TEST] ThreadManager & SysV Trampoline tests passed (thread returned 42)!\n";

#ifdef _WIN32
    std::cout << "[TEST] Running Pure System V Guest Thread Execution Test (RDI verification)...\n";
    // Machine code that strictly reads RDI (SysV AMD64 arg 1)
    // mov rax, rdi     (48 89 f8)
    // add rax, 10      (48 83 c0 0a)
    // ret              (c3)
    const Sailor::u8 sysvCode[] = { 0x48, 0x89, 0xF8, 0x48, 0x83, 0xC0, 0x0A, 0xC3 };
    void* codeMem = VirtualAlloc(nullptr, sizeof(sysvCode), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(codeMem != nullptr);
    std::memcpy(codeMem, sysvCode, sizeof(sysvCode));
    DWORD oldProt = 0;
    VirtualProtect(codeMem, sizeof(sysvCode), PAGE_EXECUTE_READ, &oldProt);

    auto sysvEntry = reinterpret_cast<Sailor::ScePthreadEntry>(codeMem);
    Sailor::ScePthread sysvThread = 0;
    res = tm.CreateThread(&sysvThread, &attr, sysvEntry, reinterpret_cast<void*>(32), "SysVGuestThread");
    assert(res == Sailor::SCE_OK);

    void* sysvRetVal = nullptr;
    res = tm.JoinThread(sysvThread, &sysvRetVal);
    assert(res == Sailor::SCE_OK);
    assert(reinterpret_cast<uintptr_t>(sysvRetVal) == 42); // 32 + 10 = 42! Verified read from RDI!

    VirtualFree(codeMem, 0, MEM_RELEASE);
    std::cout << "[TEST] Pure System V Guest Thread (RDI argument reading) verified bit-for-bit (returned 42)!\n";
#endif

    // Test ExitThread / scePthreadExit
    Sailor::ScePthread exitThread = 0;
    res = tm.CreateThread(&exitThread, &attr, [](void* arg) -> void* {
        (void)arg;
        Sailor::ThreadManager::Instance().ExitThread(reinterpret_cast<void*>(999));
        return reinterpret_cast<void*>(111); // Must never be reached!
    }, nullptr, "ExitThreadTest");
    assert(res == Sailor::SCE_OK);
    void* exitRetVal = nullptr;
    res = tm.JoinThread(exitThread, &exitRetVal);
    assert(res == Sailor::SCE_OK);
    assert(reinterpret_cast<uintptr_t>(exitRetVal) == 999);
    std::cout << "[TEST] ExitThread / scePthreadExit verified (returned 999)!\n";
}

static bool s_fiberRan = false;
static Sailor::u64 s_fiberArgPassed = 0;

static void TestFiberEntry(Sailor::u64 argInit, Sailor::u64 argRun) {
    s_fiberRan = true;
    s_fiberArgPassed = argInit + argRun;
    Sailor::FiberManager::Instance().ReturnToThread(12345, nullptr);
}

void TestFibers() {
    std::cout << "[TEST] Running libSceFiber Context Switching Tests...\n";
    auto& fm = Sailor::FiberManager::Instance();

    Sailor::SceFiberInternal fiber{};
    Sailor::i32 res = fm.InitializeFiber(&fiber, "WorkerFiber", TestFiberEntry, 100, nullptr, 0x20000);
    assert(res == Sailor::SCE_OK);

    Sailor::u64 returnVal = 0;
    res = fm.RunFiber(&fiber, 200, &returnVal);
    assert(res == Sailor::SCE_OK);
    assert(s_fiberRan);
    assert(s_fiberArgPassed == 300); // 100 + 200
    assert(returnVal == 12345);

    res = fm.FinalizeFiber(&fiber);
    assert(res == Sailor::SCE_OK);
    std::cout << "[TEST] libSceFiber tests passed!\n";
}

void TestDecompressor() {
    std::cout << "[TEST] Running High-Speed Decompression Tests...\n";
    auto& dec = Sailor::Decompressor::Instance();

    // 1. Raw buffer decompression
    const char rawData[] = "High Performance PS5 Asset Data Stream";
    char outRaw[64] = {0};
    size_t outSize = 0;
    Sailor::i32 res = dec.Decompress(rawData, sizeof(rawData), outRaw, sizeof(outRaw), Sailor::CompressionFormat::None, &outSize);
    assert(res == Sailor::SCE_OK);
    assert(outSize == sizeof(rawData));
    assert(std::strcmp(outRaw, rawData) == 0);

    // 2. LZ4 decompression
    const Sailor::u8 lz4Block[] = { 0x15, 'A', 0x01, 0x00 };
    char lz4Out[32] = {0};
    res = dec.Decompress(lz4Block, sizeof(lz4Block), lz4Out, sizeof(lz4Out), Sailor::CompressionFormat::Lz4, &outSize);
    assert(res == Sailor::SCE_OK);
    assert(outSize >= 10);
    for (size_t i = 0; i < 10; ++i) assert(lz4Out[i] == 'A');

    // 3. Kraken decompression fallback / hardware acceleration
    char krakenOut[64] = {0};
    res = dec.Decompress(rawData, sizeof(rawData), krakenOut, sizeof(krakenOut), Sailor::CompressionFormat::Kraken, &outSize);
    assert(res == Sailor::SCE_OK);
    assert(outSize == sizeof(rawData));
    assert(std::strcmp(krakenOut, rawData) == 0);

    std::cout << "[TEST] High-Speed Decompression tests passed!\n";
}

void TestPadManager() {
    std::cout << "[TEST] Running DualSense Controller & Input Tests...\n";
    auto& pad = Sailor::PadManager::Instance();
    assert(pad.Initialize() == Sailor::SCE_OK);

    Sailor::i32 handle = pad.Open(0, 0, 0, nullptr);
    assert(handle > 0);

    Sailor::ScePadData state{};
    assert(pad.ReadState(handle, &state) == Sailor::SCE_OK);
    assert(state.connected);
    assert(state.leftStickX == 128 && state.leftStickY == 128);

    Sailor::ScePadVibrationParam vib{120, 240};
    assert(pad.SetVibration(handle, &vib) == Sailor::SCE_OK);

    Sailor::ScePadColor col{0, 100, 255, 255};
    assert(pad.SetLightBar(handle, &col) == Sailor::SCE_OK);

    assert(pad.Close(handle) == Sailor::SCE_OK);
    std::cout << "[TEST] DualSense Controller tests passed!\n";
}

void TestSaveDataManager() {
    std::cout << "[TEST] Running Persistent Save Data Tests...\n";
    auto& sdm = Sailor::SaveDataManager::Instance();
    assert(sdm.Initialize() == Sailor::SCE_OK);

    Sailor::SceSaveDataMount mount{};
    mount.userId = 100;
    mount.dirName = "SLOT00";
    mount.mountPoint = "/savedata0";

    Sailor::SceSaveDataMountResult mountRes{};
    assert(sdm.Mount(&mount, &mountRes) == Sailor::SCE_OK);
    assert(std::strcmp(mountRes.mountPoint, "/savedata0") == 0);

    // Write a test save file through VFS into the mounted save data container
    auto& vfs = Sailor::VirtualFileSystem::Instance();
    Sailor::i32 fd = vfs.Open("/savedata0/profile.bin", 0x0002 | 0x0200 | 0x0400, 0666);
    assert(fd >= 10);
    const char saveContent[] = "SAVED_GAME_STATE_LEVEL_99";
    assert(vfs.Write(fd, saveContent, sizeof(saveContent)) == sizeof(saveContent));
    assert(vfs.Close(fd) == Sailor::SCE_OK);

    // Verify directory search
    Sailor::SceSaveDataDirNameSearch search{};
    search.userId = 100;
    search.maxResults = 10;
    Sailor::SceSaveDataDirNameSearchResult searchRes{};
    assert(sdm.SearchDirNames(&search, &searchRes) == Sailor::SCE_OK);
    assert(searchRes.hitNum >= 1);
    assert(std::strcmp(searchRes.dirNames[0], "SLOT00") == 0);

    assert(sdm.Umount("/savedata0") == Sailor::SCE_OK);
    std::cout << "[TEST] Persistent Save Data tests passed!\n";
}

void TestAudioOutManager() {
    std::cout << "[TEST] Running Multi-Channel Audio Out Tests...\n";
    auto& audio = Sailor::AudioOutManager::Instance();
    Sailor::i32 handle = audio.Open(0, 0, 0, 512, 48000, 0);
    assert(handle > 0);

    Sailor::SceAudioOutPortInfo info{};
    assert(audio.GetPortInfo(handle, &info) == Sailor::SCE_OK);
    assert(info.sampleRate == 48000);
    assert(info.numChannels == 2);

    Sailor::i16 pcmData[512 * 2] = {0};
    assert(audio.Output(handle, pcmData) == Sailor::SCE_OK);

    Sailor::i32 vol[2] = {100, 100};
    assert(audio.SetVolume(handle, 0, vol) == Sailor::SCE_OK);

    assert(audio.Close(handle) == Sailor::SCE_OK);
    std::cout << "[TEST] Multi-Channel Audio Out tests passed!\n";
}

int main() {
    std::cout << "=== SailorBox Phase 2 Kernel Test Suite ===\n";
    TestDirectMemory();
    TestVirtualFileSystem();
    TestSyncPrimitives();
    TestThreading();
    TestFibers();
    TestDecompressor();
    TestPadManager();
    TestSaveDataManager();
    TestAudioOutManager();
    std::cout << "=== All Phase 2 Tests PASSED Successfully! ===\n";
    return 0;
}
