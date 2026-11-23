#include "relinker/Relinker.hpp"
#include "relinker/PeImportBuilder.hpp"
#include "relinker/PeExportBuilder.hpp"
#include "relinker/EntryStubBuilder.hpp"
#include "common/Logger.hpp"
#include <unordered_set>

namespace Sailor {

namespace {
u32 AlignUp(u32 val, u32 align) {
    return (val + align - 1) & ~(align - 1);
}
} // namespace

bool Relinker::Relink(const std::filesystem::path& inputElfPath,
                      const std::filesystem::path& outputPePath,
                      const RelinkOptions& options) {
    SB_LOG_INFO("Relinker", "Starting relink conversion: {} -> {}",
        inputElfPath.string(), outputPePath.string());

    ElfReader reader;
    if (!reader.Load(inputElfPath)) {
        SB_LOG_ERROR("Relinker", "Failed to load input ELF: {}", inputElfPath.string());
        return false;
    }

    bool isDll = options.isDll;
    std::string outExt = outputPePath.extension().string();
    std::transform(outExt.begin(), outExt.end(), outExt.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (outExt == ".exe") {
        isDll = false;
    } else if (outExt == ".dll") {
        isDll = true;
    } else if (!isDll) {
        // PS5 ET_SCE_DYNAMIC (0xFE18) or ET_DYN (3) are PRX dynamic libraries
        isDll = (reader.GetType() == 0xFE18 || reader.GetType() == 3);
    }

    u64 effectiveBaseAddress = options.fixedBaseAddress;
    if (isDll && effectiveBaseAddress == 0x140000000ULL) {
        effectiveBaseAddress = 0x200000000ULL + ((Nid::Compute(inputElfPath.filename().string()).Value() & 0x7FFULL) << 24);
        SB_LOG_INFO("Relinker", "SPRX/DLL detected: assigning deterministic module base address 0x{:X}", effectiveBaseAddress);
    } else if (!isDll) {
        SB_LOG_INFO("Relinker", "Main application executable (EXE) detected: using base address 0x{:X}", effectiveBaseAddress);
    }

    PeWriter writer;
    writer.SetImageBase(effectiveBaseAddress);
    writer.SetIsDll(isDll);

    u64 elfEntry = reader.GetEntryPoint();
    u32 currentRva = 0x1000;

    u64 minVaddr = UINT64_MAX;
    for (const auto& seg : reader.GetSegments()) {
        minVaddr = std::min(minVaddr, seg.vaddr);
    }
    const bool isPie = (minVaddr < effectiveBaseAddress);
    const u64 pieSkew = (isPie && minVaddr < 0x1000) ? 0x1000ULL : 0ULL;
    if (pieSkew > 0) {
        SB_LOG_INFO("Relinker", "Position-Independent Executable detected (minVaddr=0x{:X}), applying uniform +0x{:X} skew",
            minVaddr, pieSkew);
    }

    int sectionIndex = 0;
    for (const auto& seg : reader.GetSegments()) {
        std::string name;
        u32 characteristics = 0;

        if (seg.flags & PF_X) {
            name = std::format(".text{}", sectionIndex ? std::to_string(sectionIndex) : "");
            characteristics = 0x60000020; // CNT_CODE | MEM_EXECUTE | MEM_READ
        } else if (seg.flags & PF_W) {
            name = std::format(".data{}", sectionIndex ? std::to_string(sectionIndex) : "");
            characteristics = 0xC0000040; // CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE
        } else {
            name = std::format(".rdata{}", sectionIndex ? std::to_string(sectionIndex) : "");
            characteristics = 0x40000040; // CNT_INITIALIZED_DATA | MEM_READ
        }

        u64 segVaddr = seg.vaddr;
        if (isPie) {
            segVaddr += pieSkew;
        }
        u32 vaddrRva = static_cast<u32>(
            (segVaddr >= effectiveBaseAddress)
                ? (segVaddr - effectiveBaseAddress)
                : segVaddr
        );

        u32 pageOffset = vaddrRva & 0xFFFU;
        u32 alignedRva = vaddrRva & ~0xFFFU;
        u32 alignedMemSz = static_cast<u32>(seg.memsz) + pageOffset;
        std::vector<u8> alignedData;
        if (!seg.data.empty()) {
            alignedData.resize(pageOffset + seg.data.size(), 0);
            std::memcpy(alignedData.data() + pageOffset, seg.data.data(), seg.data.size());
        }
        writer.AddSection(name, characteristics, alignedRva, alignedMemSz, std::move(alignedData));

        currentRva = std::max(currentRva, AlignUp(alignedRva + alignedMemSz, 0x1000));
        sectionIndex++;
    }

    const auto& dynInfo = reader.GetDynamicLinker().GetInfo();

    // 0. Synthesize .edata (PE Exports) for PRX / DLL modules
    if (isDll) {
        PeExportBuilder exportBuilder;
        exportBuilder.SetModuleName(outputPePath.filename().string());

        for (std::size_t i = 0; i < dynInfo.symbolNames.size(); ++i) {
            const std::string& symName = dynInfo.symbolNames[i];
            if (symName.empty()) continue;
            if (i >= dynInfo.symbolValues.size() || dynInfo.symbolValues[i] == 0) continue;

            u64 symVal = dynInfo.symbolValues[i];
            if (isPie) symVal += pieSkew;
            u32 funcRva = static_cast<u32>((symVal >= effectiveBaseAddress) ? (symVal - effectiveBaseAddress) : symVal);

            std::string bareName = symName;
            auto hashPos = bareName.find('#');
            if (hashPos != std::string::npos) {
                bareName = bareName.substr(0, hashPos);
            }

            exportBuilder.AddExport(bareName, funcRva);
            if (bareName != symName) {
                exportBuilder.AddExport(symName, funcRva);
            }
        }

        auto builtExports = exportBuilder.Build(currentRva);
        if (!builtExports.edataSectionData.empty()) {
            writer.AddSection(".edata", 0x40000040, currentRva,
                static_cast<u32>(builtExports.edataSectionData.size()),
                builtExports.edataSectionData);
            writer.SetExportDirectory(builtExports.exportDirectoryRva, builtExports.exportDirectorySize);
            currentRva = AlignUp(currentRva + static_cast<u32>(builtExports.edataSectionData.size()), 0x1000);
            SB_LOG_INFO("Relinker", "Generated .edata export table for {} ({} symbols, size {} bytes)",
                outputPePath.filename().string(), dynInfo.symbolNames.size(), builtExports.edataSectionData.size());
        }
    }

    // 1. Synthesize .idata (PE Imports)
    u32 idataRva = currentRva;
    PeImportBuilder importBuilder;

    importBuilder.AddModule("KERNEL32.dll", {
        "ExitProcess",
        "VirtualAlloc",
        "VirtualFree",
        "VirtualProtect",
        "LoadLibraryA",
        "GetProcAddress"
    });

    importBuilder.AddModule("sailor_kernel.dll", {
        "__sailor_unhandled_nid",
        "__sailor_init_tls",
        "sceSysmoduleLoadModule",
        "sceSysmoduleLoadModuleByName",
        "sceSysmoduleUnloadModule",
        "sceKernelLoadStartModule",
        "sceKernelAllocateDirectMemory",
        "sceKernelMapDirectMemory",
        "sceKernelReleaseDirectMemory",
        "sceKernelMprotect",
        "sceKernelMunmap",
        "sceKernelReserveVirtualRange",
        "sceKernelMapFlexibleMemory",
        "sceKernelVirtualQuery",
        "sceKernelBatchMap",
        "sceKernelBatchMap2",
        "sceKernelGetDirectMemorySize",
        "sceKernelAvailableDirectMemorySize",
        "sceKernelAvailableFlexibleMemorySize",
        "sceKernelCreateEqueue",
        "sceKernelDeleteEqueue",
        "sceKernelAddUserEvent",
        "sceKernelTriggerUserEvent",
        "sceKernelWaitEqueue",
        "sceAprSubmitCommandBuffer",
        "sceAprWait",
        "scePthreadCreate",
        "scePthreadJoin",
        "scePthreadExit",
        "scePthreadSelf",
        "scePthreadMutexInit",
        "scePthreadMutexDestroy",
        "scePthreadMutexLock",
        "scePthreadMutexTrylock",
        "scePthreadMutexUnlock",
        "scePthreadCondInit",
        "scePthreadCondDestroy",
        "scePthreadCondWait",
        "scePthreadCondTimedwait",
        "scePthreadCondSignal",
        "scePthreadCondBroadcast",
        "sceKernelCreateSema",
        "sceKernelDeleteSema",
        "sceKernelWaitSema",
        "sceKernelPollSema",
        "sceKernelSignalSema",
        "sceKernelOpen",
        "sceKernelClose",
        "sceKernelRead",
        "sceKernelWrite",
        "sceKernelPread",
        "sceKernelPwrite",
        "sceKernelLseek",
        "sceKernelStat",
        "sceKernelFstat",
        "sceKernelMkdir",
        "sceKernelGetProcessTime",
        "sceKernelGetProcessTimeCounter",
        "sceKernelGetProcessTimeCounterFrequency",
        "sceKernelUsleep",
        "sceKernelNanosleep",
        "sceKernelGetAppInfo",

        // Event Flag Subsystem
        "sceKernelCreateEventFlag",
        "sceKernelDeleteEventFlag",
        "sceKernelSetEventFlag",
        "sceKernelClearEventFlag",
        "sceKernelWaitEventFlag",
        "sceKernelPollEventFlag",

        // Pthread & POSIX
        "scePthreadGetthreadid",
        "scePthreadYield",
        "sched_yield",
        "pthread_mutexattr_init",
        "pthread_mutexattr_settype",
        "pthread_mutexattr_destroy",
        "scePthreadMutexattrInit",
        "scePthreadMutexattrDestroy",
        "scePthreadCondattrInit",
        "scePthreadCondattrDestroy",
        "scePthreadRwlockInit",
        "scePthreadRwlockDestroy",
        "scePthreadRwlockRdlock",
        "scePthreadRwlockWrlock",
        "scePthreadRwlockTryrdlock",
        "scePthreadRwlockTrywrlock",
        "scePthreadRwlockUnlock",
        "scePthreadRwlockattrInit",
        "scePthreadRwlockattrDestroy",
        "pthread_rwlock_init",
        "pthread_rwlock_destroy",
        "pthread_rwlock_rdlock",
        "pthread_rwlock_wrlock",
        "pthread_rwlock_tryrdlock",
        "pthread_rwlock_trywrlock",
        "pthread_rwlock_unlock",
        "__error",

        // New Pthread & Sync Exports
        "scePthreadRename",
        "scePthreadSetschedparam",
        "scePthreadGetschedparam",
        "scePthreadAttrSetstacksize",
        "scePthreadAttrInit",
        "scePthreadAttrDestroy",
        "scePthreadKeyCreate",
        "scePthreadKeyDelete",
        "scePthreadGetspecific",
        "scePthreadSetspecific",
        "scePthreadSetaffinity",
        "scePthreadGetaffinity",
        "scePthreadSetprio",
        "scePthreadGetprio",
        "scePthreadMutexattrSettype",
        "scePthreadMutexattrSetprotocol",
        "pthread_key_create",
        "pthread_key_delete",
        "pthread_getspecific",
        "pthread_setspecific",
        "pthread_attr_init",
        "pthread_attr_destroy",
        "pthread_attr_setstacksize",
        "pthread_attr_setdetachstate",
        "pthread_attr_setschedparam",
        "pthread_attr_setguardsize",
        "scePthreadAttrSetguardsize",
        "pthread_attr_setschedpolicy",
        "pthread_attr_setinheritsched",
        "scePthreadAttrSetschedpolicy",
        "scePthreadAttrSetschedparam",
        "scePthreadAttrSetinheritsched",
        "scePthreadAttrSetaffinity",
        "__stack_chk_fail",
        "__tls_get_addr",

        // Kernel Timers & System Services
        "sceKernelClockGettime",
        "sceKernelGetTscFrequency",
        "sceKernelReadTsc",
        "sceKernelUsleep",
        "sceKernelUnlink",
        "sceKernelGetdents",
        "sceKernelFtruncate",
        "sceKernelStopUnloadModule",

        // User & App Subsystems
        "sceNpRegisterStateCallbackA",
        "sceAppContentInitialize",
        "sceAppContentTemporaryDataMount2",
        "sceNpEntitlementAccessInitialize",
        "sceNpEntitlementAccessGetSkuFlag",
        "sceSaveDataInitialize3",

        // Math & CRT
        "ps5_sincosf",
        "sincosf",
        "vsnprintf",
        "snprintf",
        "fprintf",
        "printf",
        "sprintf",
        "vsprintf",
        "vfprintf",
        "sscanf",
        "time",
        "atexit",

        // libSceNet Networking Subsystem
        "sceNetSocket",
        "sceNetSetsockopt",
        "sceNetGetsockopt",
        "sceNetHtons",
        "sceNetNtohs",
        "sceNetHtonl",
        "sceNetNtohl",
        "sceNetErrnoLoc",
        "sceNetSocketClose",
        "sceNetSocketAbort",
        "sceNetShutdown",
        "sceNetBind",
        "sceNetListen",
        "sceNetConnect",
        "sceNetAccept",
        "sceNetSend",
        "sceNetSendto",
        "sceNetRecv",
        "sceNetRecvfrom",
        "sceNetGetsockname",
        "sceNetGetpeername",
        "sceNetPoolCreate",
        "sceNetPoolDestroy",
        "sceNetResolverCreate",
        "sceNetResolverDestroy",
        "sceNetResolverStartNtoa",
        "sceNetResolverStartAton",
        "sceNetInit",
        "sceNetTerm",
        "sceNetCtlInit",
        "sceNetCtlTerm",
        "sceNetCtlGetInfo",
        "sceNetCtlGetState",
        "inet_pton",
        "inet_ntop",

        // C++ ABI Helpers
        "__cxa_guard_acquire",
        "__cxa_guard_release",
        "__cxa_guard_abort",
        "__cxa_atexit",
        "__cxa_pure_virtual",
        "__cxa_finalize",
        "sUP1hBaouOw",
        "bzQExy189ZI",
        "ps5_tolower_table",
        "rcQCUr0EaRU",

        // POSIX Pthread aliases
        "pthread_mutex_init",
        "pthread_mutex_destroy",
        "pthread_mutex_lock",
        "pthread_mutex_trylock",
        "pthread_mutex_unlock",
        "pthread_cond_init",
        "pthread_cond_destroy",
        "pthread_cond_wait",
        "pthread_cond_timedwait",
        "pthread_cond_signal",
        "pthread_cond_broadcast",
        "pthread_create",
        "pthread_join",
        "pthread_exit",
        "pthread_self",

        // libSceHttp Subsystem
        "sceHttpInit",
        "sceHttpTerm",
        "sceHttpCreateTemplate",
        "sceHttpDeleteTemplate",
        "sceHttpCreateConnection",
        "sceHttpCreateConnectionWithURL",
        "sceHttpDeleteConnection",
        "sceHttpCreateRequest",
        "sceHttpCreateRequestWithURL2",
        "sceHttpDeleteRequest",
        "sceHttpSendRequest",
        "sceHttpAbortRequest",
        "sceHttpAbortRequestForce",
        "sceHttpSetNonblock",
        "sceHttpSetAutoRedirect",
        "sceHttpSetAuthEnabled",
        "sceHttpSetCookieEnabled",
        "sceHttpSetConnectTimeOut",
        "sceHttpSetSendTimeOut",
        "sceHttpSetRecvTimeOut",
        "sceHttpSetResolveTimeOut",
        "sceHttpAddRequestHeader",
        "sceHttpsDisableOption",
        "sceHttpsSetMinSslVersion",
        "sceHttpsSetSslCallback",
        "sceHttpGetResponseContentLength",
        "sceHttpGetStatusCode",
        "sceHttpGetAllResponseHeaders",

        // libSceSsl Subsystem
        "sceSslInit",
        "sceSslTerm",
        "sceSslCreateConnection",
        "sceSslDeleteConnection",
        "sceSslConnect",
        "sceSslClose",
        "sceSslGetMemoryPoolStats",

        // libSceHttp2 Subsystem
        "sceHttp2Init",
        "sceHttp2Term",

        // libSceNpManager Callbacks
        "sceNpUnregisterNpReachabilityStateCallback",
        "sceNpCheckCallback",
        "scePlayGoGetChunkId",

        // Common Dialog & Invitation Dialog
        "sceCommonDialogInitialize",
        "scePlayerInvitationDialogInitialize",

        // AudioOut Subsystem
        "sceAudioOut2Initialize",
        "sceAudioOut2LoInit",
        "sceAudioOut2LoTerminate",
        "sceAudioOut2Terminate",
        "sceAudioOutInit"
    });

    importBuilder.AddModule("ucrtbase.dll", {
        "abort", "acos", "acosf", "asin", "asinf", "atan", "atan2", "atan2f", "atanf",
        "atof", "atoi", "atol", "atoll", "bsearch", "calloc", "clearerr", "clock",
        "cos", "cosf", "exit", "_exit", "exp", "exp2", "exp2f", "expf",
        "fclose", "feof", "ferror", "fflush", "fgetc", "fgets", "fmod", "fmodf",
        "fopen", "fputc", "fputs", "fread", "free", "frexp", "fseek", "ftell", "fwrite",
        "getenv", "isalnum", "isalpha", "isdigit", "isspace", "ldexp", "localeconv",
        "log", "log10", "log10f", "log2f", "logf", "longjmp", "malloc",
        "memchr", "memcmp", "memcpy", "memmove", "memmove_s", "memset",
        "modf", "modff", "perror", "pow", "powf", "putchar", "puts", "qsort",
        "rand", "realloc", "remove", "rewind", "setbuf", "setjmp", "setvbuf",
        "sin", "sinf", "srand", "strcat", "strcat_s", "strchr", "strcmp", "strcpy", "strcpy_s",
        "strcspn", "strerror", "strftime", "strlen", "strncat", "strncmp", "strncpy", "strncpy_s",
        "strpbrk", "strrchr", "strspn", "strstr", "strtod", "strtof", "strtok", "strtol", "strtoll",
        "strtoul", "strtoull", "tan", "tanf", "tolower", "toupper",
        "wcschr", "wcscmp", "wcscpy", "wcscpy_s", "wcslen", "wcsncpy", "wcsncpy_s", "wcsstr", "wcstol"
    });

    importBuilder.AddModule("sailor_pad.dll", {
        "scePadInit",
        "scePadOpen",
        "scePadClose",
        "scePadRead",
        "scePadReadState",
        "scePadSetVibration",
        "scePadSetLightBar",
        "scePadResetOrientation",
        "scePadSetMotionSensorState"
    });

    importBuilder.AddModule("sailor_audio.dll", {
        "sceAudioOutInit",
        "sceAudioOutOpen",
        "sceAudioOutClose",
        "sceAudioOutOutput",
        "sceAudioOutSetVolume",
        "sceAudioOutGetPortInfo",
        "sceAudioOutSetPortGain",
        "sceAudio3dInitialize",
        "sceAudio3dPortCreate",
        "sceAudio3dPortDestroy",
        "sceAudio3dPortAdvance",
        "sceAudio3dPortPush",
        "sceAudio3dPortFlush",
        "sceAudio3dObjectReserve",
        "sceAudio3dObjectUnreserve",
        "sceAudio3dObjectSetPosition",
        "sceAudio3dObjectSetVelocity",
        "sceAudio3dObjectSetVolume",
        "sceAudio3dObjectSetSpread",
        "sceAudio3dBedWrite",
        "sceAudio3dGetDefaultAttributes",
        "sceAudio3dCreateContext",
        "sceAudio3dDestroyContext"
    });

importBuilder.AddModule("sailor_gpu.dll", {
        "sceAgcAcbAcquireMem",
        "sceAgcAcbAcquireMemGetSize",
        "sceAgcAcbAtomicGds_0900",
        "sceAgcAcbAtomicGdsGetSize",
        "sceAgcAcbAtomicMem",
        "sceAgcAcbAtomicMemGetSize",
        "sceAgcAcbCondExec",
        "sceAgcAcbCondExecGetSize",
        "sceAgcAcbCopyData",
        "sceAgcAcbCopyDataGetSize",
        "sceAgcAcbDispatchIndirect",
        "sceAgcAcbDispatchIndirectGetSize",
        "sceAgcAcbDmaData",
        "sceAgcAcbDmaDataGetSize",
        "sceAgcAcbEventWrite",
        "sceAgcAcbEventWriteGetSize",
        "sceAgcAcbJump",
        "sceAgcAcbJumpGetSize",
        "sceAgcAcbMemSemaphore",
        "sceAgcAcbPopMarker",
        "sceAgcAcbPrimeUtcl2",
        "sceAgcAcbPrimeUtcl2GetSize",
        "sceAgcAcbPushMarker",
        "sceAgcAcbQueueEndOfShaderActionGetSize",
        "sceAgcAcbResetQueue",
        "sceAgcAcbRewind",
        "sceAgcAcbRewindGetSize",
        "sceAgcAcbSetFlip",
        "sceAgcAcbSetMarker",
        "sceAgcAcbSetWorkloadComplete",
        "sceAgcAcbSetWorkloadsActive",
        "sceAgcAcbSetWorkloadStreamInactive",
        "sceAgcAcbWaitOnAddressGetSize",
        "sceAgcAcbWaitRegMem",
        "sceAgcAcbWaitUntilSafeForRendering",
        "sceAgcAcbWriteData",
        "sceAgcAsyncCondExecPatchSetCommandAddress",
        "sceAgcAsyncCondExecPatchSetEnd",
        "sceAgcAsyncRewindPatchSetRewindState",
        "sceAgcBranchPatchSetCompareAddress",
        "sceAgcBranchPatchSetElseTarget_0300",
        "sceAgcBranchPatchSetThenTarget_0300",
        "sceAgcCbBranch",
        "sceAgcCbBranchGetSize",
        "sceAgcCbCondWrite",
        "sceAgcCbCondWriteGetSize",
        "sceAgcCbDispatch",
        "sceAgcCbDispatchGetSize",
        "sceAgcCbNopGetSize",
        "sceAgcCbQueueEndOfPipeActionGetSize",
        "sceAgcCbReleaseMem",
        "sceAgcCbSetShRegisterRangeDirect",
        "sceAgcCbSetShRegisterRangeDirectGetSize",
        "sceAgcCbSetShRegistersDirect",
        "sceAgcCbSetShRegistersDirectGetSize",
        "sceAgcCbSetUcRegisterRangeDirect",
        "sceAgcCbSetUcRegisterRangeDirectGetSize",
        "sceAgcCbSetUcRegistersDirect",
        "sceAgcCbSetUcRegistersDirectGetSize",
        "sceAgcCondExecPatchSetCommandAddress",
        "sceAgcCondExecPatchSetEnd",
        "sceAgcCreateInterpolantMapping_0100",
        "sceAgcCreatePrimState",
        "sceAgcCreateShader",
        "sceAgcDcbAcquireMem",
        "sceAgcDcbAcquireMemGetSize",
        "sceAgcDcbAtomicGds_0900",
        "sceAgcDcbAtomicGdsGetSize",
        "sceAgcDcbAtomicMem",
        "sceAgcDcbAtomicMemGetSize",
        "sceAgcDcbBeginOcclusionQueryGetSize",
        "sceAgcDcbCondExec",
        "sceAgcDcbCondExecGetSize",
        "sceAgcDcbContextStateOp_0100",
        "sceAgcDcbContextStateOpGetSize",
        "sceAgcDcbCopyData",
        "sceAgcDcbCopyDataGetSize",
        "sceAgcDcbDispatchIndirect",
        "sceAgcDcbDispatchIndirectGetSize",
        "sceAgcDcbDmaData",
        "sceAgcDcbDmaDataGetSize",
        "sceAgcDcbDrawIndex",
        "sceAgcDcbDrawIndexAuto",
        "sceAgcDcbDrawIndexAutoGetSize",
        "sceAgcDcbDrawIndexGetSize",
        "sceAgcDcbDrawIndexIndirect",
        "sceAgcDcbDrawIndexIndirectGetSize",
        "sceAgcDcbDrawIndexIndirectMulti",
        "sceAgcDcbDrawIndexIndirectMultiGetSize",
        "sceAgcDcbDrawIndexMultiInstanced",
        "sceAgcDcbDrawIndexMultiInstancedGetSize",
        "sceAgcDcbDrawIndexOffset",
        "sceAgcDcbDrawIndexOffsetGetSize",
        "sceAgcDcbDrawIndirect",
        "sceAgcDcbDrawIndirectGetSize",
        "sceAgcDcbDrawIndirectMulti",
        "sceAgcDcbDrawIndirectMultiGetSize",
        "sceAgcDcbEndOcclusionQueryGetSize",
        "sceAgcDcbEventWrite",
        "sceAgcDcbEventWriteGetSize",
        "sceAgcDcbGetLodStats",
        "sceAgcDcbGetLodStatsGetSize",
        "sceAgcDcbJump",
        "sceAgcDcbJumpGetSize",
        "sceAgcDcbMemSemaphore",
        "sceAgcDcbPopMarker",
        "sceAgcDcbPrimeUtcl2",
        "sceAgcDcbPrimeUtcl2GetSize",
        "sceAgcDcbPushMarker",
        "sceAgcDcbQueueEndOfShaderActionGetSize",
        "sceAgcDcbResetQueue",
        "sceAgcDcbRewind",
        "sceAgcDcbRewindGetSize",
        "sceAgcDcbSetBaseDispatchIndirectArgsGetSize",
        "sceAgcDcbSetBaseDrawIndirectArgsGetSize",
        "sceAgcDcbSetBaseIndirectArgs",
        "sceAgcDcbSetBoolPredicationEnableGetSize",
        "sceAgcDcbSetCxRegisterDirect",
        "sceAgcDcbSetCxRegisterDirectGetSize",
        "sceAgcDcbSetCxRegistersIndirect",
        "sceAgcDcbSetCxRegistersIndirectGetSize",
        "sceAgcDcbSetFlip",
        "sceAgcDcbSetIndexBuffer",
        "sceAgcDcbSetIndexBufferGetSize",
        "sceAgcDcbSetIndexCount",
        "sceAgcDcbSetIndexCountGetSize",
        "sceAgcDcbSetIndexIndirectArgs",
        "sceAgcDcbSetIndexIndirectArgsGetSize",
        "sceAgcDcbSetIndexSize",
        "sceAgcDcbSetIndexSizeGetSize",
        "sceAgcDcbSetMarker",
        "sceAgcDcbSetNumInstances",
        "sceAgcDcbSetNumInstancesGetSize",
        "sceAgcDcbSetPredication",
        "sceAgcDcbSetPredicationDisableGetSize",
        "sceAgcDcbSetShRegisterDirect",
        "sceAgcDcbSetShRegisterDirectGetSize",
        "sceAgcDcbSetShRegistersIndirect",
        "sceAgcDcbSetShRegistersIndirectGetSize",
        "sceAgcDcbSetUcRegisterDirect",
        "sceAgcDcbSetUcRegisterDirectGetSize",
        "sceAgcDcbSetUcRegistersIndirect",
        "sceAgcDcbSetUcRegistersIndirectGetSize",
        "sceAgcDcbSetWorkloadComplete",
        "sceAgcDcbSetWorkloadsActive",
        "sceAgcDcbSetWorkloadStreamInactive",
        "sceAgcDcbSetZPassPredicationEnableGetSize",
        "sceAgcDcbStallCommandBufferParser",
        "sceAgcDcbStallCommandBufferParserGetSize",
        "sceAgcDcbWaitOnAddressGetSize",
        "sceAgcDcbWaitRegMem",
        "sceAgcDcbWaitUntilSafeForRendering",
        "sceAgcDcbWriteData",
        "sceAgcDcbWriteDataGetSize",
        "sceAgcDebugRaiseException",
        "sceAgcDmaDataPatchSetDstAddressOrOffset",
        "sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate",
        "sceAgcDriverAddEqEvent",
        "sceAgcDriverAgrSubmitDcb",
        "sceAgcDriverAgrSubmitMultiDcbs",
        "sceAgcDriverClose",
        "sceAgcDriverDeleteEqEvent",
        "sceAgcDriverFindResourcesPublic",
        "sceAgcDriverGetDefaultOwner",
        "sceAgcDriverGetEqContextId",
        "sceAgcDriverGetEqEventType",
        "sceAgcDriverGetOwnerName",
        "sceAgcDriverGetResourceBaseAddressAndSizeInBytes",
        "sceAgcDriverGetResourceName",
        "sceAgcDriverGetResourceRegistrationMaxNameLength",
        "sceAgcDriverGetResourceShaderGuid",
        "sceAgcDriverGetResourceType",
        "sceAgcDriverGetResourceUserData",
        "sceAgcDriverGetSetFlipPacketSizeInDwords",
        "sceAgcDriverGetShaderDebuggingStatus",
        "sceAgcDriverGetWaitRenderingPacketSizeInDwords",
        "sceAgcDriverInitResourceRegistration",
        "sceAgcDriverIsCaptureInProgress",
        "sceAgcDriverIsSubmitValidationEnabled",
        "sceAgcDriverIsTraceInProgress",
        "sceAgcDriverQueryResourceRegistrationUserMemoryRequirements",
        "sceAgcDriverRegisterDefaultOwner",
        "sceAgcDriverRegisterGdsResource",
        "sceAgcDriverRegisterOwner",
        "sceAgcDriverRegisterResource",
        "sceAgcDriverRegisterWorkloadStream",
        "sceAgcDriverRequestCaptureStart",
        "sceAgcDriverRequestCaptureStop",
        "sceAgcDriverSetHsOffchipParam",
        "sceAgcDriverSetResourceUserData",
        "sceAgcDriverSetTFRing",
        "sceAgcDriverSubmitAcb",
        "sceAgcDriverSubmitCommandBuffer",
        "sceAgcDriverSubmitDcb",
        "sceAgcDriverSubmitMultiAcbs",
        "sceAgcDriverSubmitMultiCommandBuffers",
        "sceAgcDriverSubmitMultiDcbs",
        "sceAgcDriverTriggerCapture",
        "sceAgcDriverUnregisterAllResourcesForOwner",
        "sceAgcDriverUnregisterOwnerAndResources",
        "sceAgcDriverUnregisterResource",
        "sceAgcDriverUnregisterWorkloadStream",
        "sceAgcDriverWaitUntilSafeForRendering",
        "sceAgcFuseShaderHalves_0200",
        "sceAgcGetDataPacketPayloadAddress_0090",
        "sceAgcGetDataPacketPayloadRange",
        "sceAgcGetDefaultCxStateFlat",
        "sceAgcGetFusedShaderSize_0080",
        "sceAgcGetGsOversubscription",
        "sceAgcGetPacketSize",
        "sceAgcGetRegisterDefaults2",
        "sceAgcGetRegisterDefaults2Internal",
        "sceAgcInit_0090",
        "sceAgcJumpPatchSetTarget_0300",
        "sceAgcQueueEndOfPipeActionPatchAddress",
        "sceAgcQueueEndOfPipeActionPatchData",
        "sceAgcQueueEndOfPipeActionPatchGcrCntl",
        "sceAgcQueueEndOfPipeActionPatchType",
        "sceAgcRewindPatchSetRewindState",
        "sceAgcSetCxRegIndirectPatchAddRegisters",
        "sceAgcSetCxRegIndirectPatchSetAddress",
        "sceAgcSetCxRegIndirectPatchSetNumRegisters",
        "sceAgcSetNop",
        "sceAgcSetPacketPredication",
        "sceAgcSetRangePredication",
        "sceAgcSetShRegIndirectPatchAddRegisters",
        "sceAgcSetShRegIndirectPatchSetAddress",
        "sceAgcSetShRegIndirectPatchSetNumRegisters",
        "sceAgcSetUcRegIndirectPatchAddRegisters",
        "sceAgcSetUcRegIndirectPatchSetAddress",
        "sceAgcSetUcRegIndirectPatchSetNumRegisters",
        "sceAgcSuspendPoint",
        "sceAgcUpdateInterpolantMapping_0100",
        "sceAgcUpdatePrimState",
        "sceAgcWaitRegMemPatchAddress",
        "sceAgcWaitRegMemPatchCompareFunction",
        "sceAgcWaitRegMemPatchReference",
        "sceVideoOutAddFlipEvent",
        "sceVideoOutClose",
        "sceVideoOutGetResolutionStatus",
        "sceVideoOutOpen",
        "sceVideoOutRegisterBuffers",
        "sceVideoOutRegisterBuffers2",
        "sceVideoOutRegisterFlipEventQueue",
        "sceVideoOutSetBufferAttribute2",
        "sceVideoOutSetFlipRate",
        "sceVideoOutSubmitFlip"
    });

    importBuilder.AddModule("sailor_media.dll", {
        "sceAvPlayerInit",
        "sceAvPlayerClose",
        "sceAvPlayerAddSource",
        "sceAvPlayerStart",
        "sceAvPlayerStop",
        "sceAvPlayerPause",
        "sceAvPlayerIsActive",
        "sceAvPlayerGetVideoData",
        "sceAvPlayerGetAudioData",
        "sceAvPlayerSetLooping"
    });

    importBuilder.AddModule("sailor_services.dll", {
        // libSceUserService
        "sceUserServiceInitialize",
        "sceUserServiceTerminate",
        "sceUserServiceGetInitialUser",
        "sceUserServiceGetLoginUserIdList",
        "sceUserServiceGetUserName",
        "sceUserServiceGetForegroundUser",
        "sceUserServiceGetUserIdByPadId",
        "sceUserServiceGetPadIdByUserId",
        "sceUserServiceRegisterEventCallback",
        "sceUserServiceUnregisterEventCallback",
        "sceUserServiceGetEvent",

        // libSceSaveData
        "sceSaveDataInitialize",
        "sceSaveDataInitialize2",
        "sceSaveDataTerminate",
        "sceSaveDataMount",
        "sceSaveDataMount2",
        "sceSaveDataUmount",
        "sceSaveDataDelete",
        "sceSaveDataGetSaveDataMemory",
        "sceSaveDataDirNameSearch",
        "sceSaveDataSave",
        "sceSaveDataLoad",

        // libSceNpTrophy2
        "sceNpTrophyCreateContext",
        "sceNpTrophyDestroyContext",
        "sceNpTrophyCreateHandle",
        "sceNpTrophyDestroyHandle",
        "sceNpTrophyUnlockTrophy",
        "sceNpTrophyGetTrophyUnlockState",
        "sceNpTrophyShowTrophyList",
        "sceNpTrophyRegisterContext",
        "sceNpTrophySetScreenshot",
        "sceNpTrophyGetProgress",

        // libScePlayGo
        "scePlayGoInitialize",
        "scePlayGoTerminate",
        "scePlayGoGetChunkStatus",
        "scePlayGoGetProgress",
        "scePlayGoGetInstallSpeed",
        "scePlayGoSetLanguage",
        "scePlayGoOpen",
        "scePlayGoClose",
        "scePlayGoPrefetch",
        "scePlayGoGetLocus"
    });

    std::unordered_set<std::string> companionSymbols;
    std::unordered_map<std::string, std::vector<std::string>> companionImports;
    std::string currentModule = outputPePath.filename().string();

    for (const std::string& symName : dynInfo.symbolNames) {
        if (symName.empty()) continue;
        auto hashPos = symName.find('#');
        if (hashPos == std::string::npos) continue;

        std::string bareName = symName.substr(0, hashPos);
        std::string suffix = symName.substr(hashPos);

        std::string targetDll;
        if (!isDll) {
            // Main application eboot.bin
            if (suffix == "#V#W") {
                targetDll = "pros.sdk.Prospero.dll";
            } else if (suffix == "#B#C") {
                targetDll = "libfmod.dll";
            } else if (suffix == "#C#D") {
                targetDll = "libfmodstudio.dll";
            } else if (suffix == "#o#n") {
                targetDll = "libSceAmpr.dll";
            } else if (suffix == "#n#m") {
                targetDll = "libc.dll";
            }
        } else {
            // Companion PRX modules: pros.sdk.Prospero.dll links against libc.dll
            if (currentModule == "pros.sdk.Prospero.dll") {
                if (suffix == "#C#D") {
                    targetDll = "libc.dll";
                }
            }
        }

        if (!targetDll.empty() && targetDll != currentModule) {
            companionImports[targetDll].push_back(bareName);
            companionSymbols.insert(bareName);
            companionSymbols.insert(symName);
        }
    }

    for (auto& [dllName, syms] : companionImports) {
        std::sort(syms.begin(), syms.end());
        syms.erase(std::unique(syms.begin(), syms.end()), syms.end());
        importBuilder.AddModule(dllName, syms);
        SB_LOG_INFO("Relinker", "Configured companion import {} with {} symbols for {}", dllName, syms.size(), currentModule);
    }

    auto builtImports = importBuilder.Build(idataRva);
    writer.AddSection(".idata", 0xC0000040, idataRva,
        static_cast<u32>(builtImports.idataSectionData.size()),
        builtImports.idataSectionData);

    writer.SetImportDirectory(builtImports.importDirectoryRva, builtImports.importDirectorySize);
    writer.SetIatDirectory(builtImports.iatRva, builtImports.iatSize);

    currentRva = AlignUp(idataRva + static_cast<u32>(builtImports.idataSectionData.size()), 0x1000);

    // 2. Synthesize .thunk section (System V Direct Jump Thunks + Universal Fallback Stubs)
    u32 thunkSectionRva = currentRva;
    std::vector<u8> thunkSectionData;
    std::unordered_map<std::string, u64> functionThunkVas;
    EntryStubBuilder stubBuilder;

    // Direct 6-byte jmp thunks under Clang/GCC (where SAILOR_SYSV_ABI works natively),
    // or System V -> MS x64 ABI translation bridge thunks under MSVC (where cl.exe uses MS x64 ABI)
    for (const auto& [funcName, iatRva] : builtImports.functionIatRvas) {
        u64 iatVa = effectiveBaseAddress + iatRva;
        u64 thunkVa = effectiveBaseAddress + thunkSectionRva + thunkSectionData.size();

        std::vector<u8> thunkBytes;
        if (companionSymbols.count(funcName)) {
            // Companion PS5 PRX code: already expects System V ABI, use direct jmp
            thunkBytes = stubBuilder.SynthesizeDirectJumpThunk(iatVa, thunkVa);
        } else {
#if defined(__clang__) || defined(__GNUC__)
            thunkBytes = stubBuilder.SynthesizeDirectJumpThunk(iatVa, thunkVa);
#else
            if (funcName == "ps5_sincosf" || funcName == "sincosf") {
                thunkBytes = stubBuilder.SynthesizeSysVToMsThunkSincosf(iatVa);
            } else {
                thunkBytes = stubBuilder.SynthesizeSysVToMsThunk(iatVa);
            }
#endif
        }

        functionThunkVas[funcName] = thunkVa;
        thunkSectionData.insert(thunkSectionData.end(), thunkBytes.begin(), thunkBytes.end());
    }

    // Build secondary NID -> Thunk VA lookup map for Sony NID-hashed symbols
    std::unordered_map<u64, u64> nidThunkVas;
    for (const auto& [funcName, thunkVa] : functionThunkVas) {
        u64 nidVal = 0;
        try {
            nidVal = (funcName.size() == 11) ? Nid(funcName).Value() : Nid::Compute(funcName).Value();
        } catch (...) {
            nidVal = Nid::Compute(funcName).Value();
        }
        nidThunkVas[nidVal] = thunkVa;
    }

    // Explicit aliases for Sony NIDs (ones that start with digits or differ from plain name)
    auto AddNidAlias = [&](std::string_view nidStr, const std::string& targetName) {
        auto it = functionThunkVas.find(targetName);
        if (it != functionThunkVas.end()) {
            u64 va = it->second;
            functionThunkVas[std::string(nidStr)] = va;
            try {
                nidThunkVas[Nid(nidStr).Value()] = va;
            } catch (...) {}
        }
    };

    AddNidAlias("3GPpjQdAMTw", "__cxa_guard_acquire");
    AddNidAlias("9rAeANT2tyE", "__cxa_guard_release");
    AddNidAlias("2emaaluWzUw", "__cxa_guard_abort");
    AddNidAlias("tsvEmnenz48", "__cxa_atexit");
    AddNidAlias("zr094EQ39Ww", "__cxa_pure_virtual");
    AddNidAlias("H2e8t5ScQGc", "__cxa_finalize");

    // C++ operator new / delete aliases to malloc / free
    AddNidAlias("fJnpuVVBbKk", "malloc"); // _Znwm (operator new(size_t))
    AddNidAlias("hdm0YfMa7TQ", "malloc"); // _Znam (operator new[](size_t))
    AddNidAlias("ryUxD-60bKM", "malloc"); // _ZnwmRKSt9nothrow_t
    AddNidAlias("Jh5qUcwiSEk", "malloc"); // _ZnamRKSt9nothrow_t
    AddNidAlias("z+P+xCnWLBk", "free");   // _ZdlPv (operator delete(void*))
    AddNidAlias("MLWl90SFWNE", "free");   // _ZdaPv (operator delete[](void*))
    AddNidAlias("lYDzBVE5mZs", "free");   // _ZdlPvm (operator delete(void*, size_t))
    AddNidAlias("FOt55ZNaVJk", "free");   // _ZdaPvm (operator delete[](void*, size_t))
    AddNidAlias("McsGnqV6yRE", "free");   // _ZdlPvRKSt9nothrow_t
    AddNidAlias("m-fSo3EbxNA", "free");   // _ZdaPvRKSt9nothrow_t

    // C++ standard exception runtime helpers
    AddNidAlias("oe9tS0VztYk", "__cxa_finalize"); // _ZNSt13runtime_errorD2Ev (no-op dtor)
    AddNidAlias("MOBxtefPZUg", "__cxa_finalize"); // _ZNSt9exceptionD2Ev (no-op dtor)
    AddNidAlias("qYhnoevd9bI", "abort");          // _ZSt9terminatev

    // Process Time Counter aliases
    AddNidAlias("fgxnMeTNUtY", "sceKernelGetProcessTimeCounter");
    AddNidAlias("BNowx2l588E", "sceKernelGetProcessTimeCounterFrequency");
    AddNidAlias("1uJgoVq3bQU", "ps5_tolower_table");
    AddNidAlias("rcQCUr0EaRU", "rcQCUr0EaRU");

    // Math & CRT aliases
    AddNidAlias("pztV4AF18iI", "ps5_sincosf");
    AddNidAlias("Q2V+iqvjgC0", "vsnprintf");
    AddNidAlias("9BcDykPmo1I", "__error");
    AddNidAlias("vNe1w4diLCs", "__tls_get_addr");

    // Network (libSceNet) aliases
    AddNidAlias("Q4qBuN-c0ZM", "sceNetSocket");
    AddNidAlias("2mKX2Spso7I", "sceNetSetsockopt");
    AddNidAlias("xphrZusl78E", "sceNetGetsockopt");
    AddNidAlias("iWQWrwiSt8A", "sceNetHtons");
    AddNidAlias("HQOwnfMGipQ", "sceNetErrnoLoc");
    AddNidAlias("45ggEzakPJQ", "sceNetSocketClose");
    AddNidAlias("zJGf8xjFnQE", "sceNetSocketAbort");
    AddNidAlias("TSM6whtekok", "sceNetShutdown");
    AddNidAlias("beRjXBn-z+o", "sceNetSend");
    AddNidAlias("gvD1greCu0A", "sceNetSendto");
    AddNidAlias("9wO9XrMsNhc", "sceNetRecv");
    AddNidAlias("304ooNZxWDY", "sceNetRecvfrom");
    AddNidAlias("kOj1HiAGE54", "sceNetListen");
    AddNidAlias("KuOmgKoqCdY", "sceNetBind");
    AddNidAlias("XVL8So3QJUk", "sceNetConnect");
    AddNidAlias("3e+4Iv7IJ8U", "sceNetAccept");
    AddNidAlias("hoOAofhhRvE", "sceNetGetsockname");
    AddNidAlias("TCkRD0DWNLg", "sceNetGetpeername");
    AddNidAlias("8Kcp5d-q1Uo", "inet_pton");
    AddNidAlias("9vA2aW+CHuA", "inet_ntop");
    AddNidAlias("dgJBaeJnGpo", "sceNetPoolCreate");
    AddNidAlias("K7RlrTkI-mw", "sceNetPoolDestroy");
    AddNidAlias("C4UgDHHPvdw", "sceNetResolverCreate");
    AddNidAlias("kJlYH5uMAWI", "sceNetResolverDestroy");
    AddNidAlias("Nd91WaWmG2w", "sceNetResolverStartNtoa");
    AddNidAlias("Apb4YDxKsRI", "sceNetResolverStartAton");

    // Kernel EventFlag & Threading aliases
    AddNidAlias("IOnSvHzqu6A", "sceKernelSetEventFlag");
    AddNidAlias("7uhBFWRAS60", "sceKernelClearEventFlag");
    AddNidAlias("JTvBflhYazQ", "sceKernelWaitEventFlag");
    AddNidAlias("BpFoboUJoZU", "sceKernelCreateEventFlag");
    AddNidAlias("8mql9OcQnd4", "sceKernelDeleteEventFlag");
    AddNidAlias("9lvj5DjHZiA", "sceKernelPollEventFlag");
    AddNidAlias("MBuItvba6z8", "sceKernelReleaseDirectMemory");
    AddNidAlias("hwVSPCmp5tM", "sceKernelReleaseDirectMemory");
    AddNidAlias("EI-5-jlq2dE", "scePthreadGetthreadid");
    AddNidAlias("T72hz6ffq08", "scePthreadYield");
    AddNidAlias("6XG4B33N09g", "sched_yield");
    AddNidAlias("dQHWEsJtoE4", "pthread_mutexattr_init");
    AddNidAlias("mDmgMOGVUqg", "pthread_mutexattr_settype");
    AddNidAlias("HF7lK46xzjY", "pthread_mutexattr_destroy");
    AddNidAlias("rVjRvHJ0X6c", "sceKernelVirtualQuery");
    AddNidAlias("4+h9EzwKF4I", "scePthreadAttrSetschedpolicy");
    AddNidAlias("DzES9hQF4f4", "scePthreadAttrSetschedparam");
    AddNidAlias("eXbUSpEaTsA", "scePthreadAttrSetinheritsched");
    AddNidAlias("3qxgM4ezETA", "scePthreadAttrSetaffinity");
    AddNidAlias("-2IRUCO--PM", "sceKernelReadTsc");
    AddNidAlias("aNz11fnnzi4", "sceKernelAvailableFlexibleMemorySize");
    AddNidAlias("C0f7TJcbfac", "sceKernelAvailableDirectMemorySize");
    AddNidAlias("3Zl8BePTh9Y", "sceNpCheckCallback");
    AddNidAlias("73fF1MFU8hA", "scePlayGoGetChunkId");
    AddNidAlias("CehC8MRfoPO", "pthread_attr_setstacksize");
    AddNidAlias("+HeBUYvEx8A", "pthread_attr_setguardsize");
    AddNidAlias("wExsHODYs6N", "pthread_attr_setdetachstate");

    // Auto-resolved PS5 NID aliases (Clean-Room)
    AddNidAlias("+L98PIbGttk", "scePthreadRwlockUnlock"); // +L98PIbGttk#Z#Y
    AddNidAlias("+P6FRGH4LfA", "memmove"); // +P6FRGH4LfA#n#m
    AddNidAlias("+r3rMFwItV4", "sceKernelPread"); // +r3rMFwItV4#Z#Y
    AddNidAlias("-P6FNMzk2Kc", "cosf"); // -P6FNMzk2Kc#n#m
    AddNidAlias("-RJWNMK3fC8", "scePlayGoGetProgress"); // -RJWNMK3fC8#S#T
    AddNidAlias("-kU6bB4M-+k", "strspn"); // -kU6bB4M-+k#n#m
    AddNidAlias("0-KXaS70xy4", "pthread_getspecific"); // 0-KXaS70xy4#X#Y
    AddNidAlias("0TyVk4MSLt0", "pthread_cond_init"); // 0TyVk4MSLt0#X#Y
    AddNidAlias("0WMHDb5Dt94", "modf"); // 0WMHDb5Dt94#n#m
    AddNidAlias("0hlfW1O4Aa4", "localeconv"); // 0hlfW1O4Aa4#n#m
    AddNidAlias("0nV21JjYCH8", "wcsncpy"); // 0nV21JjYCH8#n#m
    AddNidAlias("0wu33hunNdE", "sleep"); // 0wu33hunNdE#X#Y
    AddNidAlias("1-LFLmRFxxM", "sceKernelMkdir"); // 1-LFLmRFxxM#Z#Y
    AddNidAlias("12wOHk8ywb0", "sceKernelPollSema"); // 12wOHk8ywb0#Z#Y
    AddNidAlias("1471ajPzxh0", "pthread_rwlock_destroy"); // 1471ajPzxh0#X#Y
    AddNidAlias("188x57JYp0g", "sceKernelCreateSema"); // 188x57JYp0g#Z#Y
    AddNidAlias("1D0H2KNjshE", "powf"); // 1D0H2KNjshE#n#m
    AddNidAlias("1G3lF1Gg1k8", "sceKernelOpen"); // 1G3lF1Gg1k8#Z#Y
    AddNidAlias("1Pk0qZQGeWo", "sscanf"); // 1Pk0qZQGeWo#n#m
    AddNidAlias("1jfXLRVzisc", "sceKernelUsleep"); // 1jfXLRVzisc#Z#Y
    AddNidAlias("1xxcMiGu2fo", "sceUserServiceGetUserName"); // 1xxcMiGu2fo#f#f
    AddNidAlias("27bAgiJmOh0", "pthread_cond_timedwait"); // 27bAgiJmOh0#X#Y
    AddNidAlias("2MOy+rUfuhQ", "pthread_cond_signal"); // 2MOy+rUfuhQ#X#Y
    AddNidAlias("2Of0f+3mhhE", "scePthreadMutexDestroy"); // 2Of0f+3mhhE#Z#Y
    AddNidAlias("2SKEx6bSq-4", "sceKernelBatchMap"); // 2SKEx6bSq-4#Z#Y
    AddNidAlias("2Tb92quprl0", "scePthreadCondInit"); // 2Tb92quprl0#Z#Y
    AddNidAlias("2X5agFjKxMc", "calloc"); // 2X5agFjKxMc#n#m
    AddNidAlias("2Z+PpY6CaJg", "pthread_mutex_unlock"); // 2Z+PpY6CaJg#X#Y
    AddNidAlias("2emaaluWzUw", "__cxa_guard_abort"); // 2emaaluWzUw#n#m
    AddNidAlias("2mKX2Spso7I", "sceNetSetsockopt"); // 2mKX2Spso7I#W#X
    AddNidAlias("2vDqwBlpF-o", "strtod"); // 2vDqwBlpF-o#n#m
    AddNidAlias("3+UPM-9E6xY", "modff"); // 3+UPM-9E6xY#n#m
    AddNidAlias("304ooNZxWDY", "sceNetRecvfrom"); // 304ooNZxWDY#W#X
    AddNidAlias("3GPpjQdAMTw", "__cxa_guard_acquire"); // 3GPpjQdAMTw#n#m
    AddNidAlias("3QIPIh-GDjw", "rewind"); // 3QIPIh-GDjw#n#m
    AddNidAlias("3e+4Iv7IJ8U", "accept"); // 3e+4Iv7IJ8U#X#Y
    AddNidAlias("3kg7rT0NQIs", "scePthreadExit"); // 3kg7rT0NQIs#Z#Y
    AddNidAlias("45ggEzakPJQ", "sceNetSocketClose"); // 45ggEzakPJQ#W#X
    AddNidAlias("4czppHBiriw", "sceKernelSignalSema"); // 4czppHBiriw#Z#Y
    AddNidAlias("4n51s0zEf0c", "inet_pton"); // 4n51s0zEf0c#X#Y
    AddNidAlias("4wSze92BhLI", "sceKernelWrite"); // 4wSze92BhLI#Z#Y
    AddNidAlias("5OqszGpy7Mg", "strtoull"); // 5OqszGpy7Mg#n#m
    AddNidAlias("5Xa2ACNECdo", "strcpy_s"); // 5Xa2ACNECdo#n#m
    AddNidAlias("5jRCs2axtr4", "inet_ntop"); // 5jRCs2axtr4#X#Y
    AddNidAlias("6O8EwYOgH9Y", "getsockopt"); // 6O8EwYOgH9Y#X#Y
    AddNidAlias("6ULAa0fq4jA", "scePthreadRwlockInit"); // 6ULAa0fq4jA#Z#Y
    AddNidAlias("6UgtwV+0zb4", "scePthreadCreate"); // 6UgtwV+0zb4#Z#Y
    AddNidAlias("6XG4B33N09g", "sched_yield"); // 6XG4B33N09g#X#Y
    AddNidAlias("6f5f-qx4ucA", "wcscpy_s"); // 6f5f-qx4ucA#n#m
    AddNidAlias("6ncge5+l5Qs", "scePadClose"); // 6ncge5+l5Qs#Y#Z
    AddNidAlias("6sJWiWSRuqk", "strncpy"); // 6sJWiWSRuqk#n#m
    AddNidAlias("7H0iTOciTLo", "pthread_mutex_lock"); // 7H0iTOciTLo#X#Y
    AddNidAlias("7Ly52zaL44Q", "asin"); // 7Ly52zaL44Q#n#m
    AddNidAlias("7oxv3PPCumo", "sceKernelReserveVirtualRange"); // 7oxv3PPCumo#Z#Y
    AddNidAlias("7uhBFWRAS60", "sceKernelClearEventFlag"); // 7uhBFWRAS60#Z#Y
    AddNidAlias("88Vv-AzHVj8", "fmodf"); // 88Vv-AzHVj8#n#m
    AddNidAlias("8G2LB+A3rzg", "atexit"); // 8G2LB+A3rzg#n#m
    AddNidAlias("8mql9OcQnd4", "sceKernelDeleteEventFlag"); // 8mql9OcQnd4#Z#Y
    AddNidAlias("8nY19bKoiZk", "fcntl"); // 8nY19bKoiZk#X#Y
    AddNidAlias("8u8lPzUEq+U", "memchr"); // 8u8lPzUEq+U#n#m
    AddNidAlias("8zTFvBIAIN8", "memset"); // 8zTFvBIAIN8#n#m
    AddNidAlias("8zsu04XNsZ4", "expf"); // 8zsu04XNsZ4#n#m
    AddNidAlias("9BcDykPmo1I", "__error"); // 9BcDykPmo1I#Z#Y
    AddNidAlias("9T2pDF2Ryqg", "sceNetHtonl"); // 9T2pDF2Ryqg#W#X
    AddNidAlias("9UK1vLZQft4", "scePthreadMutexLock"); // 9UK1vLZQft4#Z#Y
    AddNidAlias("9lvj5DjHZiA", "sceKernelPollEventFlag"); // 9lvj5DjHZiA#Z#Y
    AddNidAlias("9rAeANT2tyE", "__cxa_guard_release"); // 9rAeANT2tyE#n#m
    AddNidAlias("9wO9XrMsNhc", "sceNetRecv"); // 9wO9XrMsNhc#W#X
    AddNidAlias("9yDWMxEFdJU", "strrchr"); // 9yDWMxEFdJU#n#m
    AddNidAlias("AEJdIVZTEmo", "qsort"); // AEJdIVZTEmo#n#m
    AddNidAlias("AEuF3F2f8TA", "fgetc"); // AEuF3F2f8TA#n#m
    AddNidAlias("AHxyhN96dy4", "ferror"); // AHxyhN96dy4#n#m
    AddNidAlias("Apb4YDxKsRI", "sceNetResolverStartAton"); // Apb4YDxKsRI#W#X
    AddNidAlias("AqBioC2vF3I", "read"); // AqBioC2vF3I#X#Y
    AddNidAlias("Av3zjWi64Kw", "strftime"); // Av3zjWi64Kw#n#m
    AddNidAlias("B59+zQQCcbU", "memmove_s"); // B59+zQQCcbU#n#m
    AddNidAlias("BB+kb08Tl9A", "scePthreadRwlockDestroy"); // BB+kb08Tl9A#Z#Y
    AddNidAlias("BmMjYxmew1w", "scePthreadCondTimedwait"); // BmMjYxmew1w#Z#Y
    AddNidAlias("BpFoboUJoZU", "sceKernelCreateEventFlag"); // BpFoboUJoZU#Z#Y
    AddNidAlias("C4UgDHHPvdw", "sceNetResolverCreate"); // C4UgDHHPvdw#W#X
    AddNidAlias("CBiu4mCE1DA", "sceVideoOutSetFlipRate"); // CBiu4mCE1DA#d#d
    AddNidAlias("CdWp0oHWGr0", "sceUserServiceGetInitialUser"); // CdWp0oHWGr0#f#f
    AddNidAlias("Cg4srZ6TKbU", "sceKernelRead"); // Cg4srZ6TKbU#Z#Y
    AddNidAlias("D0OdFMjp46I", "sceKernelCreateEqueue"); // D0OdFMjp46I#Z#Y
    AddNidAlias("DfivPArhucg", "memcmp"); // DfivPArhucg#n#m
    AddNidAlias("E6ao34wPw+U", "stat"); // E6ao34wPw+U#X#Y
    AddNidAlias("EH-x713A99c", "atan2f"); // EH-x713A99c#n#m
    AddNidAlias("EI-5-jlq2dE", "scePthreadGetthreadid"); // EI-5-jlq2dE#Z#Y
    AddNidAlias("EMutwaQ34Jo", "perror"); // EMutwaQ34Jo#n#m
    AddNidAlias("EgmLo6EWgso", "pthread_rwlock_unlock"); // EgmLo6EWgso#X#Y
    AddNidAlias("EotR8a3ASf4", "pthread_self"); // EotR8a3ASf4#X#Y
    AddNidAlias("Ez8xjo9UF4E", "recv"); // Ez8xjo9UF4E#X#Y
    AddNidAlias("Ezzq78ZgHPs", "wcschr"); // Ezzq78ZgHPs#n#m
    AddNidAlias("F0Y42t-3e18", "sceAgcDriverInitResourceRegistration"); // F0Y42t-3e18#b#b
    AddNidAlias("F8bUHwAG284", "scePthreadMutexattrInit"); // F8bUHwAG284#Z#Y
    AddNidAlias("FJrT5LuUBAU", "pthread_exit"); // FJrT5LuUBAU#X#Y
    AddNidAlias("FM5NPnLqBc8", "wcscpy"); // FM5NPnLqBc8#n#m
    AddNidAlias("FN4gaPmuFV8", "write"); // FN4gaPmuFV8#X#Y
    AddNidAlias("Fm-dmyywH9Q", "fileno"); // Fm-dmyywH9Q#n#m
    AddNidAlias("GMpvxPFW924", "vprintf"); // GMpvxPFW924#n#m
    AddNidAlias("GZWjF-YIFFk", "asinf"); // GZWjF-YIFFk#n#m
    AddNidAlias("HF7lK46xzjY", "pthread_mutexattr_destroy"); // HF7lK46xzjY#X#Y
    AddNidAlias("HQOwnfMGipQ", "sceNetErrnoLoc"); // HQOwnfMGipQ#W#X
    AddNidAlias("HUbZmOnT-Dg", "atan2"); // HUbZmOnT-Dg#n#m
    AddNidAlias("IOnSvHzqu6A", "sceKernelSetEventFlag"); // IOnSvHzqu6A#Z#Y
    AddNidAlias("IWIBBdTHit4", "sceKernelMapFlexibleMemory"); // IWIBBdTHit4#Z#Y
    AddNidAlias("JBcgYuW8lPU", "acos"); // JBcgYuW8lPU#n#m
    AddNidAlias("JGgj7Uvrl+A", "scePthreadCondBroadcast"); // JGgj7Uvrl+A#Z#Y
    AddNidAlias("JTvBflhYazQ", "sceKernelWaitEventFlag"); // JTvBflhYazQ#Z#Y
    AddNidAlias("JrwFIMzKNr0", "ldexp"); // JrwFIMzKNr0#n#m
    AddNidAlias("K+gcnFFJKVc", "strcat_s"); // K+gcnFFJKVc#n#m
    AddNidAlias("K-jXhbt2gn4", "pthread_mutex_trylock"); // K-jXhbt2gn4#X#Y
    AddNidAlias("K7RlrTkI-mw", "sceNetPoolDestroy"); // K7RlrTkI-mw#W#X
    AddNidAlias("KdP-nULpuGw", "fgets"); // KdP-nULpuGw#n#m
    AddNidAlias("KuOmgKoqCdY", "bind"); // KuOmgKoqCdY#X#Y
    AddNidAlias("KuOuD58hqn4", "malloc_stats_fast"); // KuOuD58hqn4#n#m
    AddNidAlias("L-Q3LEjIbgA", "sceKernelMapDirectMemory"); // L-Q3LEjIbgA#Z#Y
    AddNidAlias("L1SBTkC+Cvw", "abort"); // L1SBTkC+Cvw#n#m
    AddNidAlias("Ls4tzzhimqQ", "strcat"); // Ls4tzzhimqQ#n#m
    AddNidAlias("LwG8g3niqwA", "sceKernelDlsym"); // LwG8g3niqwA#Z#Y
    AddNidAlias("LxcEU+ICu8U", "feof"); // LxcEU+ICu8U#n#m
    AddNidAlias("M1Gma1ocrGE", "scePlayGoOpen"); // M1Gma1ocrGE#S#T
    AddNidAlias("MBuItvba6z8", "sceKernelReleaseDirectMemory"); // MBuItvba6z8#Z#Y
    AddNidAlias("MPe0EeBGM-E", "scePlayGoTerminate"); // MPe0EeBGM-E#S#T
    AddNidAlias("MUjC4lbHrK4", "fflush"); // MUjC4lbHrK4#n#m
    AddNidAlias("MZO7FXyAPU8", "remove"); // MZO7FXyAPU8#n#m
    AddNidAlias("MpxhMh8QFro", "fwrite"); // MpxhMh8QFro#n#m
    AddNidAlias("NN01qLRhiqU", "rename"); // NN01qLRhiqU#X#Y
    AddNidAlias("Nd91WaWmG2w", "sceNetResolverStartNtoa"); // Nd91WaWmG2w#W#X
    AddNidAlias("NesIgTmfF0Q", "bsearch"); // NesIgTmfF0Q#n#m
    AddNidAlias("OXXX4mUk3uk", "sceNetConnect"); // OXXX4mUk3uk#W#X
    AddNidAlias("OXmauLdQ8kY", "atan"); // OXmauLdQ8kY#n#m
    AddNidAlias("Op8TBGY5KHg", "pthread_cond_wait"); // Op8TBGY5KHg#X#Y
    AddNidAlias("Ovb2dSJOAuE", "strcmp"); // Ovb2dSJOAuE#n#m
    AddNidAlias("Ox9i0c7L5w0", "scePthreadRwlockRdlock"); // Ox9i0c7L5w0#Z#Y
    AddNidAlias("OxhIB8LB-PQ", "pthread_create"); // OxhIB8LB-PQ#X#Y
    AddNidAlias("PIWqhn9oSxc", "sceNetAccept"); // PIWqhn9oSxc#W#X
    AddNidAlias("Q2V+iqvjgC0", "vsnprintf"); // Q2V+iqvjgC0#n#m
    AddNidAlias("Q3VBxCXhUHs", "memcpy"); // Q3VBxCXhUHs#n#m
    AddNidAlias("Q4qBuN-c0ZM", "sceNetSocket"); // Q4qBuN-c0ZM#W#X
    AddNidAlias("Q4rRL34CEeE", "sinf"); // Q4rRL34CEeE#n#m
    AddNidAlias("QI-x0SL8jhw", "acosf"); // QI-x0SL8jhw#n#m
    AddNidAlias("QKd0qM58Qes", "sceKernelStopUnloadModule"); // QKd0qM58Qes#Z#Y
    AddNidAlias("QMFyLoqNxIg", "setvbuf"); // QMFyLoqNxIg#n#m
    AddNidAlias("QZP6I9ZZxpE", "clock"); // QZP6I9ZZxpE#n#m
    AddNidAlias("Qazy8LmXTvw", "ftell"); // Qazy8LmXTvw#n#m
    AddNidAlias("QrZZdJ8XsX0", "fputs"); // QrZZdJ8XsX0#n#m
    AddNidAlias("R1Jvn8bSCW8", "sceKernelDeleteSema"); // R1Jvn8bSCW8#Z#Y
    AddNidAlias("RIa6GnWp+iU", "strerror"); // RIa6GnWp+iU#n#m
    AddNidAlias("RQXLbdT2lc4", "logf"); // RQXLbdT2lc4#n#m
    AddNidAlias("RXXqi4CtF8w", "pthread_cond_destroy"); // RXXqi4CtF8w#X#Y
    AddNidAlias("Rbvt+5Y2iEw", "sceNetNtohs"); // Rbvt+5Y2iEw#W#X
    AddNidAlias("RenI1lL1WFk", "getsockname"); // RenI1lL1WFk#X#Y
    AddNidAlias("S1GkePI17zQ", "sceSaveDataDelete"); // S1GkePI17zQ#T#U
    AddNidAlias("SRI6S9B+-a4", "atof"); // SRI6S9B+-a4#n#m
    AddNidAlias("Slmz4HMpNGs", "wcsncpy_s"); // Slmz4HMpNGs#n#m
    AddNidAlias("St9nbxSoezk", "clearerr"); // St9nbxSoezk#n#m
    AddNidAlias("T72hz6ffq08", "scePthreadYield"); // T72hz6ffq08#Z#Y
    AddNidAlias("T8fER+tIGgk", "select"); // T8fER+tIGgk#X#Y
    AddNidAlias("TCkRD0DWNLg", "sceNetGetpeername"); // TCkRD0DWNLg#W#X
    AddNidAlias("TSM6whtekok", "sceNetShutdown"); // TSM6whtekok#W#X
    AddNidAlias("TU-d9PfIHPM", "socket"); // TU-d9PfIHPM#X#Y
    AddNidAlias("TUuiYS2kE8s", "shutdown"); // TUuiYS2kE8s#X#Y
    AddNidAlias("TXFFFiNldU8", "getpeername"); // TXFFFiNldU8#X#Y
    AddNidAlias("U9ueyEhSkF4", "sceAgcDriverRegisterDefaultOwner"); // U9ueyEhSkF4#b#b
    AddNidAlias("UK2Tl2DWUns", "sceKernelClose"); // UK2Tl2DWUns#Z#Y
    AddNidAlias("Uco1I0dlDi8", "scePlayGoClose"); // Uco1I0dlDi8#S#T
    AddNidAlias("Ujf3KzMvRmI", "memalign"); // Ujf3KzMvRmI#n#m
    AddNidAlias("Up36PTk687E", "sceVideoOutOpen"); // Up36PTk687E#d#d
    AddNidAlias("UqDGjXA5yUM", "munmap"); // UqDGjXA5yUM#X#Y
    AddNidAlias("VAzswvTOCzI", "unlink"); // VAzswvTOCzI#X#Y
    AddNidAlias("VOBg+iNwB-4", "strtoll"); // VOBg+iNwB-4#n#m
    AddNidAlias("WDpobjImAb4", "wcsstr"); // WDpobjImAb4#n#m
    AddNidAlias("WKAXJ4XBPQ4", "scePthreadCondWait"); // WKAXJ4XBPQ4#Z#Y
    AddNidAlias("WkkeywLJcgU", "wcslen"); // WkkeywLJcgU#n#m
    AddNidAlias("WrOLvHU0yQM", "pthread_setspecific"); // WrOLvHU0yQM#X#Y
    AddNidAlias("WuMbPBKN1TU", "log10"); // WuMbPBKN1TU#n#m
    AddNidAlias("XVL8So3QJUk", "connect"); // XVL8So3QJUk#X#Y
    AddNidAlias("XwLA5cTHjt4", "__gxx_personality_v0"); // XwLA5cTHjt4#n#m
    AddNidAlias("Y7aJ1uydPMo", "realloc"); // Y7aJ1uydPMo#n#m
    AddNidAlias("YNzNkJzYqEg", "strncpy_s"); // YNzNkJzYqEg#n#m
    AddNidAlias("YQ0navp+YIc", "puts"); // YQ0navp+YIc#n#m
    AddNidAlias("YndgXqQVV7c", "scePadReadState"); // YndgXqQVV7c#Y#Z
    AddNidAlias("ZE6RNL+eLbk", "tanf"); // ZE6RNL+eLbk#n#m
    AddNidAlias("Zxa0VhQVTsk", "sceKernelWaitSema"); // Zxa0VhQVTsk#Z#Y
    AddNidAlias("aI+OeCz8xrQ", "scePthreadSelf"); // aI+OeCz8xrQ#Z#Y
    AddNidAlias("aZK8lNei-Qw", "fputc"); // aZK8lNei-Qw#n#m
    AddNidAlias("aesyjrHVWy4", "strncmp"); // aesyjrHVWy4#n#m
    AddNidAlias("bErx49PgxyY", "sceNetBind"); // bErx49PgxyY#W#X
    AddNidAlias("bY-PO6JhzhQ", "close"); // bY-PO6JhzhQ#X#Y
    AddNidAlias("beRjXBn-z+o", "sceNetSend"); // beRjXBn-z+o#W#X
    AddNidAlias("bwFjS+bX9mA", "sceUserServiceTerminate"); // bwFjS+bX9mA#f#f
    AddNidAlias("cQke9UuBQOk", "sceKernelMunmap"); // cQke9UuBQOk#Z#Y
    AddNidAlias("cmo1RIYva9o", "scePthreadMutexInit"); // cmo1RIYva9o#Z#Y
    AddNidAlias("cpCOXWMgha0", "rand"); // cpCOXWMgha0#n#m
    AddNidAlias("d3dMyWORw8A", "wcstol"); // d3dMyWORw8A#n#m
    AddNidAlias("dQHWEsJtoE4", "pthread_mutexattr_init"); // dQHWEsJtoE4#X#Y
    AddNidAlias("dgJBaeJnGpo", "sceNetPoolCreate"); // dgJBaeJnGpo#W#X
    AddNidAlias("dnaeGXbjP6E", "exp2"); // dnaeGXbjP6E#n#m
    AddNidAlias("eLdDw6l0-bU", "snprintf"); // eLdDw6l0-bU#n#m
    AddNidAlias("eR2bZFAAU0Q", "sceSysmoduleUnloadModule"); // eR2bZFAAU0Q#O#P
    AddNidAlias("eV9wAD2riIA", "sceKernelStat"); // eV9wAD2riIA#Z#Y
    AddNidAlias("fFxGkxF2bVo", "setsockopt"); // fFxGkxF2bVo#X#Y
    AddNidAlias("fPhymKNvK-A", "sceUserServiceGetLoginUserIdList"); // fPhymKNvK-A#f#f
    AddNidAlias("fZOeZIOEmLw", "send"); // fZOeZIOEmLw#X#Y
    AddNidAlias("fffwELXNVFA", "fprintf"); // fffwELXNVFA#n#m
    AddNidAlias("fiiNDnNBKVY", "localtime_s"); // fiiNDnNBKVY#n#m
    AddNidAlias("fzyMKs9kim0", "sceKernelWaitEqueue"); // fzyMKs9kim0#Z#Y
    AddNidAlias("g+PZd2hiacg", "scePthreadCondDestroy"); // g+PZd2hiacg#Z#Y
    AddNidAlias("g8cM39EUZ6o", "sceSysmoduleLoadModule"); // g8cM39EUZ6o#O#P
    AddNidAlias("gNQ1V2vfXDE", "setjmp"); // gNQ1V2vfXDE#n#m
    AddNidAlias("gQX+4GDQjpM", "malloc"); // gQX+4GDQjpM#n#m
    AddNidAlias("gSRnr79F8tQ", "sceAgcDriverSubmitAcb"); // gSRnr79F8tQ#b#b
    AddNidAlias("gvD1greCu0A", "sceNetSendto"); // gvD1greCu0A#W#X
    AddNidAlias("h9CcP3J0oVM", "pthread_join"); // h9CcP3J0oVM#X#Y
    AddNidAlias("hcuQgD53UxM", "printf"); // hcuQgD53UxM#n#m
    AddNidAlias("hoOAofhhRvE", "sceNetGetsockname"); // hoOAofhhRvE#W#X
    AddNidAlias("hsi9drzHR2k", "log2f"); // hsi9drzHR2k#n#m
    AddNidAlias("hv1luiJrqQM", "scePadInit"); // hv1luiJrqQM#Y#Z
    AddNidAlias("iGjsr1WAtI0", "pthread_rwlock_rdlock"); // iGjsr1WAtI0#X#Y
    AddNidAlias("iWQWrwiSt8A", "sceNetHtons"); // iWQWrwiSt8A#W#X
    AddNidAlias("j3YMu1MVNNo", "sceUserServiceInitialize"); // j3YMu1MVNNo#f#f
    AddNidAlias("j4ViWNHEgww", "strlen"); // j4ViWNHEgww#n#m
    AddNidAlias("jbz9I9vkqkk", "vsprintf"); // jbz9I9vkqkk#n#m
    AddNidAlias("jpFjmgAC5AE", "sceKernelDeleteEqueue"); // jpFjmgAC5AE#Z#Y
    AddNidAlias("kA-TdiOCsaY", "frexp"); // kA-TdiOCsaY#n#m
    AddNidAlias("kBwCPsYX-m4", "sceKernelFstat"); // kBwCPsYX-m4#Z#Y
    AddNidAlias("kDZvoVssCgQ", "strpbrk"); // kDZvoVssCgQ#n#m
    AddNidAlias("kDh-NfxgMtE", "scePthreadCondSignal"); // kDh-NfxgMtE#Z#Y
    AddNidAlias("kHg45qPC6f0", "strncat"); // kHg45qPC6f0#n#m
    AddNidAlias("kJlYH5uMAWI", "sceNetResolverDestroy"); // kJlYH5uMAWI#W#X
    AddNidAlias("kOj1HiAGE54", "sceNetListen"); // kOj1HiAGE54#W#X
    AddNidAlias("kiZSXIWd9vg", "strcpy"); // kiZSXIWd9vg#n#m
    AddNidAlias("lKEN2IebgJ0", "longjmp"); // lKEN2IebgJ0#n#m
    AddNidAlias("lLMT9vJAck0", "clock_gettime"); // lLMT9vJAck0#X#Y
    AddNidAlias("lUk6wrGXyMw", "recvfrom"); // lUk6wrGXyMw#X#Y
    AddNidAlias("lbB+UlZqVG0", "fread"); // lbB+UlZqVG0#n#m
    AddNidAlias("lhpd6Wk6ccs", "log10f"); // lhpd6Wk6ccs#n#m
    AddNidAlias("ltCfaGr2JGE", "pthread_mutex_destroy"); // ltCfaGr2JGE#X#Y
    AddNidAlias("m5-2bsNfv7s", "scePthreadCondattrInit"); // m5-2bsNfv7s#Z#Y
    AddNidAlias("m5wN+SwZOR4", "putchar"); // m5wN+SwZOR4#n#m
    AddNidAlias("mDmgMOGVUqg", "pthread_mutexattr_settype"); // mDmgMOGVUqg#X#Y
    AddNidAlias("mXlxhmLNMPg", "strtol"); // mXlxhmLNMPg#n#m
    AddNidAlias("mkx2fVhNMsg", "pthread_cond_broadcast"); // mkx2fVhNMsg#X#Y
    AddNidAlias("mqQMh1zPPT8", "fstat"); // mqQMh1zPPT8#X#Y
    AddNidAlias("mqULNdimTn0", "pthread_key_create"); // mqULNdimTn0#X#Y
    AddNidAlias("mqdNorrB+gI", "scePthreadRwlockWrlock"); // mqdNorrB+gI#Z#Y
    AddNidAlias("oBr313PppNE", "sendto"); // oBr313PppNE#X#Y
    AddNidAlias("oVkZ8W8-Q8A", "strtok"); // oVkZ8W8-Q8A#n#m
    AddNidAlias("ob5xAW4ln-0", "strchr"); // ob5xAW4ln-0#n#m
    AddNidAlias("oib76F-12fk", "sceKernelLseek"); // oib76F-12fk#Z#Y
    AddNidAlias("onNY9Byn-W8", "scePthreadJoin"); // onNY9Byn-W8#Z#Y
    AddNidAlias("pDBDcY6uLSA", "vfprintf"); // pDBDcY6uLSA#n#m
    AddNidAlias("pKwslsMUmSk", "fmod"); // pKwslsMUmSk#n#m
    AddNidAlias("pNtJdE3x49E", "wcscmp"); // pNtJdE3x49E#n#m
    AddNidAlias("pO96TwzOm5E", "sceKernelGetDirectMemorySize"); // pO96TwzOm5E#Z#Y
    AddNidAlias("pQGpHYopAIY", "sceNetNtohl"); // pQGpHYopAIY#W#X
    AddNidAlias("pxnCmagrtao", "listen"); // pxnCmagrtao#X#Y
    AddNidAlias("pztV4AF18iI", "sincosf"); // pztV4AF18iI#n#m
    AddNidAlias("q0F6yS-rCms", "strcspn"); // q0F6yS-rCms#n#m
    AddNidAlias("rQFVBXp-Cxg", "fseek"); // rQFVBXp-Cxg#n#m
    AddNidAlias("rTXw65xmLIA", "sceKernelAllocateDirectMemory"); // rTXw65xmLIA#Z#Y
    AddNidAlias("rVjRvHJ0X6c", "sceKernelVirtualQuery"); // rVjRvHJ0X6c#Z#Y
    AddNidAlias("sIlRvQqsN2Y", "pthread_rwlock_wrlock"); // sIlRvQqsN2Y#X#Y
    AddNidAlias("smWEktiyyG0", "scePthreadMutexattrDestroy"); // smWEktiyyG0#Z#Y
    AddNidAlias("tIhsqj0qsFE", "free"); // tIhsqj0qsFE#n#m
    AddNidAlias("tcVi5SivF7Q", "sprintf"); // tcVi5SivF7Q#n#m
    AddNidAlias("tn3VlD0hG60", "scePthreadMutexUnlock"); // tn3VlD0hG60#Z#Y
    AddNidAlias("ts6GlZOKRrE", "scePlayGoInitialize"); // ts6GlZOKRrE#S#T
    AddNidAlias("tsvEmnenz48", "__cxa_atexit"); // tsvEmnenz48#n#m
    AddNidAlias("ttHNfU+qDBU", "pthread_mutex_init"); // ttHNfU+qDBU#X#Y
    AddNidAlias("uMei1W9uyNo", "exit"); // uMei1W9uyNo#n#m
    AddNidAlias("uWIYLFkkwqk", "scePlayGoGetLocus"); // uWIYLFkkwqk#S#T
    AddNidAlias("uodLYyUip20", "fclose"); // uodLYyUip20#n#m
    AddNidAlias("uquVH4-Du78", "sceVideoOutClose"); // uquVH4-Du78#d#d
    AddNidAlias("vSMAm3cxYTY", "sceKernelMprotect"); // vSMAm3cxYTY#Z#Y
    AddNidAlias("vNe1w4diLCs", "__tls_get_addr"); // vNe1w4diLCs#B#C
    AddNidAlias("vZMcAfsA31I", "setbuf"); // vZMcAfsA31I#n#m
    AddNidAlias("viiwFMaNamA", "strstr"); // viiwFMaNamA#n#m
    AddNidAlias("wLlFkwG9UcQ", "time"); // wLlFkwG9UcQ#n#m
    AddNidAlias("waPcxYiR3WA", "scePthreadCondattrDestroy"); // waPcxYiR3WA#Z#Y
    AddNidAlias("weDug8QD-lE", "atanf"); // weDug8QD-lE#n#m
    AddNidAlias("wuAQt-j+p4o", "exp2f"); // wuAQt-j+p4o#n#m
    AddNidAlias("wuCroIGjt2g", "open"); // wuCroIGjt2g#X#Y
    AddNidAlias("wzvqT4UqKX8", "sceKernelLoadStartModule"); // wzvqT4UqKX8#Z#Y
    AddNidAlias("xeYO4u7uyJ0", "fopen"); // xeYO4u7uyJ0#n#m
    AddNidAlias("xk0AcarP3V4", "scePadOpen"); // xk0AcarP3V4#Y#Z
    AddNidAlias("xphrZusl78E", "sceNetGetsockopt"); // xphrZusl78E#W#X
    AddNidAlias("yFVnOdGxvZY", "scePadSetVibration"); // yFVnOdGxvZY#Y#Z
    AddNidAlias("yKDy8S5yLA0", "sceSaveDataTerminate"); // yKDy8S5yLA0#T#U
    AddNidAlias("ytQULN-nhL4", "pthread_rwlock_init"); // ytQULN-nhL4#X#Y
    AddNidAlias("z0dtnPxYgtg", "chmod"); // z0dtnPxYgtg#X#Y
    AddNidAlias("zHchY8ft5pk", "pthread_attr_destroy"); // zHchY8ft5pk#X#Y
    AddNidAlias("zJGf8xjFnQE", "sceNetSocketAbort"); // zJGf8xjFnQE#W#X
    AddNidAlias("zr094EQ39Ww", "__cxa_pure_virtual"); // zr094EQ39Ww#n#m

    AddNidAlias("QBi7HCK03hw", "sceKernelClockGettime"); // 0x4018BB1C22B4DE1C
    AddNidAlias("1j3S3n-tTW4", "sceKernelGetTscFrequency"); // 0xD63DD2DE7FED4D6E
    AddNidAlias("1jfXLRVzisc", "sceKernelUsleep"); // 1jfXLRVzisc#Z#Y
    AddNidAlias("GBUY7ywdULE", "scePthreadRename"); // 0x181518EF2C1D50B1
    AddNidAlias("oIRFTjoILbg", "scePthreadSetschedparam"); // 0xA084454E3A082DB8
    AddNidAlias("P41kTWUS3EI", "scePthreadGetschedparam"); // 0x3F8D644D6512DC42
    AddNidAlias("UTXzJbWhhTE", "scePthreadAttrSetstacksize"); // 0x5135F325B5A18531
    AddNidAlias("nsYoNRywwNg", "scePthreadAttrInit"); // 0x9EC628351CB0C0D8
    AddNidAlias("62KCwEMmzcM", "scePthreadAttrDestroy"); // 62KCwEMmzcM#Z#Y
    AddNidAlias("geDaqgH9lTg", "scePthreadKeyCreate"); // 0x81E0DAAA01FD9538
    AddNidAlias("PrdHuuDekhY", "scePthreadKeyDelete"); // PrdHuuDekhY#Z#Y
    AddNidAlias("eoht7mQOCmo", "scePthreadGetspecific"); // eoht7mQOCmo#Z#Y
    AddNidAlias("+BzXYkqYeLE", "scePthreadSetspecific"); // +BzXYkqYeLE#Z#Y
    AddNidAlias("iMp8QpE+XO4", "scePthreadMutexattrSettype"); // 0x88CA7C42913E5CEE
    AddNidAlias("1FGvU0i9saQ", "scePthreadMutexattrSetprotocol"); // 0xD451AF5348BDB1A4
    AddNidAlias("AUXVxWeJU-A", "sceKernelUnlink"); // AUXVxWeJU-A#Z#Y
    AddNidAlias("j2AIqSqJP0w", "sceKernelGetdents"); // j2AIqSqJP0w#Z#Y
    AddNidAlias("VW3TVZiM4-E", "sceKernelFtruncate"); // VW3TVZiM4-E#Z#Y
    AddNidAlias("qQJfO8HAiaY", "sceNpRegisterStateCallbackA"); // 0xA9025F3BC1C089A6
    AddNidAlias("R9lA82OraNs", "sceAppContentInitialize"); // 0x47D940F363AB68DB
    AddNidAlias("buYbeLOGWmA", "sceAppContentTemporaryDataMount2"); // 0x6EE61B78B3865A60
    AddNidAlias("jO8DM8oyego", "sceNpEntitlementAccessInitialize"); // 0x8CEF0333CA327A0A
    AddNidAlias("0HBYxYAjmf0", "sceNpEntitlementAccessGetSkuFlag"); // 0xD07058C5802399FD
    AddNidAlias("TywrFKCoLGY", "sceSaveDataInitialize3"); // 0x4F2C2B14A0A82C66
    AddNidAlias("8G2LB+A3rzg", "atexit"); // 0xF06D8B07E037AF38
    AddNidAlias("wLlFkwG9UcQ", "time"); // 0xC0B9459301BD51C4
    AddNidAlias("eLdDw6l0-bU", "snprintf"); // 0x78B743C3A974FDB5
    AddNidAlias("fffwELXNVFA", "fprintf"); // fffwELXNVFA#n#m
    AddNidAlias("hcuQgD53UxM", "printf"); // hcuQgD53UxM#n#m
    AddNidAlias("rcrVFJsQWRY", "scePthreadGetaffinity"); // rcrVFJsQWRY#Z#Y
    AddNidAlias("bt3CTBKmGyI", "scePthreadSetaffinity"); // bt3CTBKmGyI#Z#Y
    AddNidAlias("1tKyG7RlMJo", "scePthreadGetprio"); // 1tKyG7RlMJo#Z#Y
    AddNidAlias("W0Hpm2X0uPE", "scePthreadSetprio"); // W0Hpm2X0uPE#Z#Y
    AddNidAlias("Ou3iL1abvng", "__stack_chk_fail"); // 0x3AEDE22F569BBE78

    // VideoOut & AGC Driver Aliases
    AddNidAlias("Up36PTk687E", "sceVideoOutOpen"); // 0x529dfa3d393af3b1
    AddNidAlias("rKBUtgRrtbk", "sceVideoOutRegisterBuffers2"); // 0xaca054b6046bb5b9
    AddNidAlias("PjS5uASwcV8", "sceVideoOutSetBufferAttribute2"); // 0x3e34b9b804b0715f
    AddNidAlias("HXzjK9yI30k", "sceVideoOutAddFlipEvent"); // 0x1d7ce32bdc88df49
    AddNidAlias("uquVH4-Du78", "sceVideoOutClose"); // 0xbaab951f8fc3bbbf
    AddNidAlias("CBiu4mCE1DA", "sceVideoOutSetFlipRate"); // 0x818aee26084d430
    AddNidAlias("UglJIZjGssM", "sceAgcDriverSubmitDcb"); // 0x5209492198c6b2c3
AddNidAlias("U46NwOiJpys", "sceVideoOutSubmitFlip");
    AddNidAlias("w3BY+tAEiQY", "sceVideoOutRegisterBuffers");
    AddNidAlias("6kPnj51T62Y", "sceVideoOutGetResolutionStatus");
    AddNidAlias("Q5Ehvgsz+5Q", "sceVideoOutRegisterFlipEventQueue");
    AddNidAlias("23LRUSvYu1M", "sceAgcInit_0090"); // PLT[642] 23LRUSvYu1M#c#c
    AddNidAlias("2JtWUUiYBXs", "sceAgcGetRegisterDefaults2"); // PLT[643] 2JtWUUiYBXs#c#c
    AddNidAlias("wRbq6ZjNop4", "sceAgcGetRegisterDefaults2Internal"); // PLT[644] wRbq6ZjNop4#c#c
    AddNidAlias("UglJIZjGssM", "sceAgcDriverSubmitDcb"); // PLT[645] UglJIZjGssM#b#b
    AddNidAlias("AhGvpITrf4M", "sceAgcDriverAgrSubmitDcb"); // PLT[646] AhGvpITrf4M#b#b
    AddNidAlias("6UzEidRZwkg", "sceAgcDriverSubmitMultiDcbs"); // PLT[647] 6UzEidRZwkg#b#b
    AddNidAlias("+T8Xo6LtFJI", "sceAgcDriverAgrSubmitMultiDcbs"); // PLT[648] +T8Xo6LtFJI#b#b
    AddNidAlias("gSRnr79F8tQ", "sceAgcDriverSubmitAcb"); // PLT[649] gSRnr79F8tQ#b#b
    AddNidAlias("HF3YllT3mXU", "sceAgcDriverSubmitMultiAcbs"); // PLT[650] HF3YllT3mXU#b#b
    AddNidAlias("w2rJhmD+dsE", "sceAgcDriverAddEqEvent"); // PLT[651] w2rJhmD+dsE#b#b
    AddNidAlias("DL2RXaXOy88", "sceAgcDriverDeleteEqEvent"); // PLT[652] DL2RXaXOy88#b#b
    AddNidAlias("5CdQTZIQPxM", "sceAgcDriverGetEqEventType"); // PLT[653] 5CdQTZIQPxM#b#b
    AddNidAlias("Zw7uUVPulbw", "sceAgcDriverGetEqContextId"); // PLT[654] Zw7uUVPulbw#b#b
    AddNidAlias("h9z6+0hEydk", "sceAgcSuspendPoint"); // PLT[655] h9z6+0hEydk#c#c
    AddNidAlias("AAeX-U5-P3M", "sceAgcGetDefaultCxStateFlat"); // PLT[656] AAeX-U5-P3M#c#c
    AddNidAlias("Xq5WmbwPTnQ", "sceAgcDriverTriggerCapture"); // PLT[657] Xq5WmbwPTnQ#b#b
    AddNidAlias("SAfhzJPcjuk", "sceAgcDriverRequestCaptureStart"); // PLT[658] SAfhzJPcjuk#b#b
    AddNidAlias("FOwvmNlFLjM", "sceAgcDriverRequestCaptureStop"); // PLT[659] FOwvmNlFLjM#b#b
    AddNidAlias("Ddwk4gLT5j0", "sceAgcDriverIsCaptureInProgress"); // PLT[660] Ddwk4gLT5j0#b#b
    AddNidAlias("qspAL8bgcBY", "sceAgcDriverIsSubmitValidationEnabled"); // PLT[661] qspAL8bgcBY#b#b
    AddNidAlias("+TN0oRTBxJQ", "sceAgcDriverIsTraceInProgress"); // PLT[662] +TN0oRTBxJQ#b#b
    AddNidAlias("rJUyMrDdxJg", "sceAgcDriverGetShaderDebuggingStatus"); // PLT[663] rJUyMrDdxJg#b#b
    AddNidAlias("3AyTaWcF-H8", "sceAgcDriverRegisterWorkloadStream"); // PLT[664] 3AyTaWcF-H8#b#b
    AddNidAlias("n5ElQVYsU1A", "sceAgcDriverUnregisterWorkloadStream"); // PLT[665] n5ElQVYsU1A#b#b
    AddNidAlias("AOLcoIkQDgM", "sceAgcDriverQueryResourceRegistrationUserMemoryRequirements"); // PLT[666] AOLcoIkQDgM#b#b
    AddNidAlias("F0Y42t-3e18", "sceAgcDriverInitResourceRegistration"); // PLT[667] F0Y42t-3e18#b#b
    AddNidAlias("uJziRsODk1c", "sceAgcDriverGetResourceRegistrationMaxNameLength"); // PLT[668] uJziRsODk1c#b#b
    AddNidAlias("X-Nm5KLREeg", "sceAgcDriverRegisterOwner"); // PLT[669] X-Nm5KLREeg#b#b
    AddNidAlias("W5z4eZrjEas", "sceAgcDriverRegisterResource"); // PLT[670] W5z4eZrjEas#b#b
    AddNidAlias("emP3ckeS2uo", "sceAgcDriverRegisterGdsResource"); // PLT[671] emP3ckeS2uo#b#b
    AddNidAlias("pWLG7WOpVcw", "sceAgcDriverUnregisterResource"); // PLT[672] pWLG7WOpVcw#b#b
    AddNidAlias("ZLJk9r2+2Aw", "sceAgcDriverUnregisterOwnerAndResources"); // PLT[673] ZLJk9r2+2Aw#b#b
    AddNidAlias("SCoAN5fYlUM", "sceAgcDriverUnregisterAllResourcesForOwner"); // PLT[674] SCoAN5fYlUM#b#b
    AddNidAlias("U9ueyEhSkF4", "sceAgcDriverRegisterDefaultOwner"); // PLT[675] U9ueyEhSkF4#b#b
    AddNidAlias("F0ZXt5q0ZTA", "sceAgcDriverGetDefaultOwner"); // PLT[676] F0ZXt5q0ZTA#b#b
    AddNidAlias("5l3IfCFJxBs", "sceAgcDriverFindResourcesPublic"); // PLT[677] 5l3IfCFJxBs#b#b
    AddNidAlias("M9yBzRKkjPc", "sceAgcDriverGetResourceName"); // PLT[678] M9yBzRKkjPc#b#b
    AddNidAlias("rI9lNAXPMIw", "sceAgcDriverGetResourceType"); // PLT[679] rI9lNAXPMIw#b#b
    AddNidAlias("NghWEUXp1qM", "sceAgcDriverGetResourceBaseAddressAndSizeInBytes"); // PLT[680] NghWEUXp1qM#b#b
    AddNidAlias("ls4jfY576lw", "sceAgcDriverGetResourceUserData"); // PLT[681] ls4jfY576lw#b#b
    AddNidAlias("VOMSpd9+vxU", "sceAgcDriverSetResourceUserData"); // PLT[682] VOMSpd9+vxU#b#b
    AddNidAlias("mXn+K9E-wOA", "sceAgcDriverGetResourceShaderGuid"); // PLT[683] mXn+K9E-wOA#b#b
    AddNidAlias("LepGrgk77sM", "sceAgcDriverGetOwnerName"); // PLT[684] LepGrgk77sM#b#b
    AddNidAlias("XlNp7jzGiPo", "sceAgcDriverSetTFRing"); // PLT[685] XlNp7jzGiPo#b#b
    AddNidAlias("MM4IZSEYytQ", "sceAgcDriverSetHsOffchipParam"); // PLT[686] MM4IZSEYytQ#b#b
    AddNidAlias("T6xuVw0KUJo", "sceAgcDebugRaiseException"); // PLT[687] T6xuVw0KUJo#c#c
    AddNidAlias("f3dg2CSgRKY", "sceAgcCreateShader"); // PLT[688] f3dg2CSgRKY#c#c
    AddNidAlias("dolOmWH+huQ", "sceAgcGetFusedShaderSize_0080"); // PLT[689] dolOmWH+huQ#c#c
    AddNidAlias("fd5Bp5tGTgo", "sceAgcFuseShaderHalves_0200"); // PLT[690] fd5Bp5tGTgo#c#c
    AddNidAlias("HV4j+E0MBHE", "sceAgcCreateInterpolantMapping_0100"); // PLT[691] HV4j+E0MBHE#c#c
    AddNidAlias("k0E7vkgqAuE", "sceAgcUpdateInterpolantMapping_0100"); // PLT[692] k0E7vkgqAuE#c#c
    AddNidAlias("D9sr1xGUriE", "sceAgcCreatePrimState"); // PLT[693] D9sr1xGUriE#c#c
    AddNidAlias("Y3ymLfZ1384", "sceAgcUpdatePrimState"); // PLT[694] Y3ymLfZ1384#c#c
    AddNidAlias("NKIzURsgV7I", "sceAgcGetGsOversubscription"); // PLT[695] NKIzURsgV7I#c#c
    AddNidAlias("w1KFAHVqpaU", "sceAgcCbBranch"); // PLT[696] w1KFAHVqpaU#c#c
    AddNidAlias("k3GhuSNmBLU", "sceAgcCbDispatch"); // PLT[697] k3GhuSNmBLU#c#c
    AddNidAlias("7toV+elXqNM", "sceAgcCbCondWrite"); // PLT[698] 7toV+elXqNM#c#c
    AddNidAlias("wr23dPKyWc0", "sceAgcCbReleaseMem"); // PLT[699] wr23dPKyWc0#c#c
    AddNidAlias("n2fD4A+pb+g", "sceAgcCbSetShRegisterRangeDirect"); // PLT[701] n2fD4A+pb+g#c#c
    AddNidAlias("MDLD5Ly94Xk", "sceAgcCbSetUcRegisterRangeDirect"); // PLT[702] MDLD5Ly94Xk#c#c
    AddNidAlias("UZbQjYAwwXM", "sceAgcCbSetShRegistersDirect"); // PLT[703] UZbQjYAwwXM#c#c
    AddNidAlias("03RZmELWWzw", "sceAgcCbSetUcRegistersDirect"); // PLT[704] 03RZmELWWzw#c#c
    AddNidAlias("Abendgtz+3o", "sceAgcCbDispatchGetSize"); // PLT[705] Abendgtz+3o#c#c
    AddNidAlias("uZW-mqsxkrM", "sceAgcCbBranchGetSize"); // PLT[706] uZW-mqsxkrM#c#c
    AddNidAlias("FuVbkyKlf+s", "sceAgcCbCondWriteGetSize"); // PLT[707] FuVbkyKlf+s#c#c
    AddNidAlias("t7PlZ9nt5Lc", "sceAgcCbNopGetSize"); // PLT[708] t7PlZ9nt5Lc#c#c
    AddNidAlias("hL7C0IRpWZI", "sceAgcCbQueueEndOfPipeActionGetSize"); // PLT[709] hL7C0IRpWZI#c#c
    AddNidAlias("bxGoVxpdSPQ", "sceAgcCbSetShRegisterRangeDirectGetSize"); // PLT[710] bxGoVxpdSPQ#c#c
    AddNidAlias("JOWmDrl+j20", "sceAgcCbSetUcRegisterRangeDirectGetSize"); // PLT[711] JOWmDrl+j20#c#c
    AddNidAlias("yUBESvCCJ4I", "sceAgcCbSetShRegistersDirectGetSize"); // PLT[712] yUBESvCCJ4I#c#c
    AddNidAlias("TGEZzUWLbrc", "sceAgcCbSetUcRegistersDirectGetSize"); // PLT[713] TGEZzUWLbrc#c#c
    AddNidAlias("XKKuA6VkSRc", "sceAgcAcbAtomicMem"); // PLT[714] XKKuA6VkSRc#c#c
    AddNidAlias("KT-hTp-Ch14", "sceAgcAcbAcquireMem"); // PLT[715] KT-hTp-Ch14#c#c
    AddNidAlias("qyM2bxYFPAk", "sceAgcAcbCondExec"); // PLT[716] qyM2bxYFPAk#c#c
    AddNidAlias("qzMN2XKGA4k", "sceAgcAcbCopyData"); // PLT[717] qzMN2XKGA4k#c#c
    AddNidAlias("j3EtxFkSIhQ", "sceAgcAcbDispatchIndirect"); // PLT[718] j3EtxFkSIhQ#c#c
    AddNidAlias("-RnpfpxIhec", "sceAgcAcbDmaData"); // PLT[719] -RnpfpxIhec#c#c
    AddNidAlias("cFazmnXpJOE", "sceAgcAcbEventWrite"); // PLT[720] cFazmnXpJOE#c#c
    AddNidAlias("e1DFTg+Sd8U", "sceAgcAcbJump"); // PLT[721] e1DFTg+Sd8U#c#c
    AddNidAlias("eZ4+17OQz4Q", "sceAgcAcbWriteData"); // PLT[722] eZ4+17OQz4Q#c#c
    AddNidAlias("htn36gPnBk4", "sceAgcAcbWaitRegMem"); // PLT[723] htn36gPnBk4#c#c
    AddNidAlias("GPbUp9jXQa8", "sceAgcAcbWaitUntilSafeForRendering"); // PLT[724] GPbUp9jXQa8#c#c
    AddNidAlias("DwICrVxerkY", "sceAgcAcbRewind"); // PLT[725] DwICrVxerkY#c#c
    AddNidAlias("JrtiDtKeS38", "sceAgcAcbResetQueue"); // PLT[726] JrtiDtKeS38#c#c
    AddNidAlias("szG7hz2yEhA", "sceAgcAcbPrimeUtcl2"); // PLT[727] szG7hz2yEhA#c#c
    AddNidAlias("gQkqkLttcpw", "sceAgcAcbAtomicGds_0900"); // PLT[728] gQkqkLttcpw#c#c
    AddNidAlias("xAeBOa0A3kk", "sceAgcAcbSetMarker"); // PLT[729] xAeBOa0A3kk#c#c
    AddNidAlias("cpCILPya5Zk", "sceAgcAcbPushMarker"); // PLT[730] cpCILPya5Zk#c#c
    AddNidAlias("6mFxkVqdmbQ", "sceAgcAcbPopMarker"); // PLT[731] 6mFxkVqdmbQ#c#c
    AddNidAlias("ebixW91gpPw", "sceAgcAcbSetFlip"); // PLT[732] ebixW91gpPw#c#c
    AddNidAlias("q4VuU-QsLOE", "sceAgcAcbMemSemaphore"); // PLT[733] q4VuU-QsLOE#c#c
    AddNidAlias("rVOmPz2RBlg", "sceAgcAcbSetWorkloadsActive"); // PLT[734] rVOmPz2RBlg#c#c
    AddNidAlias("opR1JeJZCBU", "sceAgcAcbSetWorkloadComplete"); // PLT[735] opR1JeJZCBU#c#c
    AddNidAlias("FcgdDM3MB+k", "sceAgcAcbSetWorkloadStreamInactive"); // PLT[736] FcgdDM3MB+k#c#c
    AddNidAlias("ewobAQeMo5k", "sceAgcAcbAcquireMemGetSize"); // PLT[737] ewobAQeMo5k#c#c
    AddNidAlias("hcIxS8pmXF4", "sceAgcAcbAtomicGdsGetSize"); // PLT[738] hcIxS8pmXF4#c#c
    AddNidAlias("da1Sm8-QDoU", "sceAgcAcbAtomicMemGetSize"); // PLT[739] da1Sm8-QDoU#c#c
    AddNidAlias("ozKzBP4aki4", "sceAgcAcbCondExecGetSize"); // PLT[740] ozKzBP4aki4#c#c
    AddNidAlias("b-oySn+G2tE", "sceAgcAcbJumpGetSize"); // PLT[741] b-oySn+G2tE#c#c
    AddNidAlias("CbQh3DKMSno", "sceAgcAcbCopyDataGetSize"); // PLT[742] CbQh3DKMSno#c#c
    AddNidAlias("M0ttm8h7SKA", "sceAgcAcbDmaDataGetSize"); // PLT[743] M0ttm8h7SKA#c#c
    AddNidAlias("PxKWV2fVAps", "sceAgcAcbDispatchIndirectGetSize"); // PLT[744] PxKWV2fVAps#c#c
    AddNidAlias("Y-5vneiBtzk", "sceAgcAcbEventWriteGetSize"); // PLT[745] Y-5vneiBtzk#c#c
    AddNidAlias("eCjKaqeeQ5s", "sceAgcAcbPrimeUtcl2GetSize"); // PLT[746] eCjKaqeeQ5s#c#c
    AddNidAlias("0ZOG0jc9nRg", "sceAgcAcbRewindGetSize"); // PLT[747] 0ZOG0jc9nRg#c#c
    AddNidAlias("idlaArvdXEs", "sceAgcAcbWaitOnAddressGetSize"); // PLT[748] idlaArvdXEs#c#c
    AddNidAlias("F8NLhWvFemI", "sceAgcAcbQueueEndOfShaderActionGetSize"); // PLT[749] F8NLhWvFemI#c#c
    AddNidAlias("57labkp+rSQ", "sceAgcDcbAcquireMem"); // PLT[750] 57labkp+rSQ#c#c
    AddNidAlias("1-gUn1PI4Sw", "sceAgcDcbAtomicMem"); // PLT[751] 1-gUn1PI4Sw#c#c
    AddNidAlias("qj7QZpgr9Uw", "sceAgcDcbContextStateOp_0100"); // PLT[752] qj7QZpgr9Uw#c#c
    AddNidAlias("BIPexNBSGog", "sceAgcDcbCondExec"); // PLT[753] BIPexNBSGog#c#c
    AddNidAlias("xSAR0LTcRKM", "sceAgcDcbJump"); // PLT[754] xSAR0LTcRKM#c#c
    AddNidAlias("1rZSWUv1IRc", "sceAgcDcbCopyData"); // PLT[755] 1rZSWUv1IRc#c#c
    AddNidAlias("WmAc2MEj6Io", "sceAgcDcbDmaData"); // PLT[756] WmAc2MEj6Io#c#c
    AddNidAlias("i1jyy49AjXU", "sceAgcDcbWriteData"); // PLT[757] i1jyy49AjXU#c#c
    AddNidAlias("-HOOCn0JY48", "sceAgcDcbSetShRegistersIndirect"); // PLT[758] -HOOCn0JY48#c#c
    AddNidAlias("ZvwO9euwYzc", "sceAgcDcbSetCxRegistersIndirect"); // PLT[759] ZvwO9euwYzc#c#c
    AddNidAlias("hvUfkUIQcOE", "sceAgcDcbSetUcRegistersIndirect"); // PLT[760] hvUfkUIQcOE#c#c
    AddNidAlias("w4-d0n60hdo", "sceAgcDcbSetUcRegisterDirect"); // PLT[761] w4-d0n60hdo#c#c
    AddNidAlias("LHFXRrlTPD8", "sceAgcDcbSetCxRegisterDirect"); // PLT[762] LHFXRrlTPD8#c#c
    AddNidAlias("pFLArOT53+w", "sceAgcDcbSetShRegisterDirect"); // PLT[763] pFLArOT53+w#c#c
    AddNidAlias("CtB+A9-VxO0", "sceAgcDcbDispatchIndirect"); // PLT[764] CtB+A9-VxO0#c#c
    AddNidAlias("Yw0jKSqop+E", "sceAgcDcbDrawIndexAuto"); // PLT[765] Yw0jKSqop+E#c#c
    AddNidAlias("q88lQ+GP5Yk", "sceAgcDcbDrawIndex"); // PLT[766] q88lQ+GP5Yk#c#c
    AddNidAlias("B+aG9DUnTKA", "sceAgcDcbDrawIndexOffset"); // PLT[767] B+aG9DUnTKA#c#c
    AddNidAlias("1q1titRBL6o", "sceAgcDcbDrawIndirect"); // PLT[768] 1q1titRBL6o#c#c
    AddNidAlias("kUlvghKs-mA", "sceAgcDcbDrawIndirectMulti"); // PLT[769] kUlvghKs-mA#c#c
    AddNidAlias("ypVBz4uPKcQ", "sceAgcDcbDrawIndexIndirectMulti"); // PLT[770] ypVBz4uPKcQ#c#c
    AddNidAlias("t1vNu082-jM", "sceAgcDcbDrawIndexIndirect"); // PLT[771] t1vNu082-jM#c#c
    AddNidAlias("Rlx+bykm0r0", "sceAgcDcbDrawIndexMultiInstanced"); // PLT[772] Rlx+bykm0r0#c#c
    AddNidAlias("aJf+j5yntiU", "sceAgcDcbEventWrite"); // PLT[773] aJf+j5yntiU#c#c
    AddNidAlias("vuSXe69VILM", "sceAgcDcbGetLodStats"); // PLT[774] vuSXe69VILM#c#c
    AddNidAlias("RmaJwLtc8rY", "sceAgcDcbSetBaseIndirectArgs"); // PLT[775] RmaJwLtc8rY#c#c
    AddNidAlias("GIIW2J37e70", "sceAgcDcbSetIndexSize"); // PLT[776] GIIW2J37e70#c#c
    AddNidAlias("l4fM9K-Lyks", "sceAgcDcbSetIndexBuffer"); // PLT[777] l4fM9K-Lyks#c#c
    AddNidAlias("8N2tmT3jmC8", "sceAgcDcbSetIndexCount"); // PLT[778] 8N2tmT3jmC8#c#c
    AddNidAlias("tSBxhAPyytQ", "sceAgcDcbSetNumInstances"); // PLT[779] tSBxhAPyytQ#c#c
    AddNidAlias("u2T2DiA5hRI", "sceAgcDcbStallCommandBufferParser"); // PLT[780] u2T2DiA5hRI#c#c
    AddNidAlias("VmW0Tdpy420", "sceAgcDcbWaitRegMem"); // PLT[781] VmW0Tdpy420#c#c
    AddNidAlias("zfcxg-ewMK8", "sceAgcDcbRewind"); // PLT[782] zfcxg-ewMK8#c#c
    AddNidAlias("jt3pl7EN17o", "sceAgcDcbPrimeUtcl2"); // PLT[783] jt3pl7EN17o#c#c
    AddNidAlias("TRO721eVt4g", "sceAgcDcbResetQueue"); // PLT[784] TRO721eVt4g#c#c
    AddNidAlias("0o3VDdtA6nM", "sceAgcDcbSetIndexIndirectArgs"); // PLT[785] 0o3VDdtA6nM#c#c
    AddNidAlias("bbFueFP+J4k", "sceAgcDcbSetPredication"); // PLT[786] bbFueFP+J4k#c#c
    AddNidAlias("zARR5aCmkoY", "sceAgcDcbAtomicGds_0900"); // PLT[787] zARR5aCmkoY#c#c
    AddNidAlias("MWiElSNE8j8", "sceAgcDcbWaitUntilSafeForRendering"); // PLT[788] MWiElSNE8j8#c#c
    AddNidAlias("QhCbS4X9Rl8", "sceAgcDcbSetMarker"); // PLT[789] QhCbS4X9Rl8#c#c
    AddNidAlias("+kSrjIVxKFE", "sceAgcDcbPushMarker"); // PLT[790] +kSrjIVxKFE#c#c
    AddNidAlias("H7uZqCoNuWk", "sceAgcDcbPopMarker"); // PLT[791] H7uZqCoNuWk#c#c
    AddNidAlias("YUeqkyT7mEQ", "sceAgcDcbSetFlip"); // PLT[792] YUeqkyT7mEQ#c#c
    AddNidAlias("G0jrLdvEqDw", "sceAgcDcbMemSemaphore"); // PLT[793] G0jrLdvEqDw#c#c
    AddNidAlias("LFSPFmGc9Hg", "sceAgcDcbSetWorkloadsActive"); // PLT[794] LFSPFmGc9Hg#c#c
    AddNidAlias("hEK26Wdny6s", "sceAgcDcbSetWorkloadComplete"); // PLT[795] hEK26Wdny6s#c#c
    AddNidAlias("FneFypEDRgY", "sceAgcDcbSetWorkloadStreamInactive"); // PLT[796] FneFypEDRgY#c#c
    AddNidAlias("nNlUtdDDvZ0", "sceAgcDcbSetShRegistersIndirectGetSize"); // PLT[797] nNlUtdDDvZ0#c#c
    AddNidAlias("GBCh3zCihoU", "sceAgcDcbSetCxRegistersIndirectGetSize"); // PLT[798] GBCh3zCihoU#c#c
    AddNidAlias("UQGTw4xRlcM", "sceAgcDcbSetUcRegistersIndirectGetSize"); // PLT[799] UQGTw4xRlcM#c#c
    AddNidAlias("QhPDD513V0w", "sceAgcDcbSetShRegisterDirectGetSize"); // PLT[800] QhPDD513V0w#c#c
    AddNidAlias("1DeUNpRIDDA", "sceAgcDcbSetCxRegisterDirectGetSize"); // PLT[801] 1DeUNpRIDDA#c#c
    AddNidAlias("aP1Ki9G3++4", "sceAgcDcbSetUcRegisterDirectGetSize"); // PLT[802] aP1Ki9G3++4#c#c
    AddNidAlias("-vnlTPPXPrw", "sceAgcDcbAcquireMemGetSize"); // PLT[803] -vnlTPPXPrw#c#c
    AddNidAlias("1tB0xkLNjcw", "sceAgcDcbAtomicGdsGetSize"); // PLT[804] 1tB0xkLNjcw#c#c
    AddNidAlias("oz6zQq1JwCE", "sceAgcDcbAtomicMemGetSize"); // PLT[805] oz6zQq1JwCE#c#c
    AddNidAlias("ou16V5hh5sg", "sceAgcDcbCondExecGetSize"); // PLT[806] ou16V5hh5sg#c#c
    AddNidAlias("H6vHS5cidSA", "sceAgcDcbContextStateOpGetSize"); // PLT[807] H6vHS5cidSA#c#c
    AddNidAlias("VEGu4dixjUg", "sceAgcDcbJumpGetSize"); // PLT[808] VEGu4dixjUg#c#c
    AddNidAlias("b5u0Jzm8TF8", "sceAgcDcbCopyDataGetSize"); // PLT[809] b5u0Jzm8TF8#c#c
    AddNidAlias("2ccJz9LQI+w", "sceAgcDcbDmaDataGetSize"); // PLT[810] 2ccJz9LQI+w#c#c
    AddNidAlias("p9tI+yTvx68", "sceAgcDcbWriteDataGetSize"); // PLT[811] p9tI+yTvx68#c#c
    AddNidAlias("zg6u-N6Otxs", "sceAgcDcbQueueEndOfShaderActionGetSize"); // PLT[812] zg6u-N6Otxs#c#c
    AddNidAlias("w8HVkEeXPv8", "sceAgcDcbDispatchIndirectGetSize"); // PLT[813] w8HVkEeXPv8#c#c
    AddNidAlias("WrdP9Zxx3lQ", "sceAgcDcbDrawIndexAutoGetSize"); // PLT[814] WrdP9Zxx3lQ#c#c
    AddNidAlias("6ee9Hd3EWXQ", "sceAgcDcbDrawIndexGetSize"); // PLT[815] 6ee9Hd3EWXQ#c#c
    AddNidAlias("qMlfB1ZhMDc", "sceAgcDcbDrawIndexOffsetGetSize"); // PLT[816] qMlfB1ZhMDc#c#c
    AddNidAlias("cxPZ4Wgvdj8", "sceAgcDcbDrawIndirectGetSize"); // PLT[817] cxPZ4Wgvdj8#c#c
    AddNidAlias("pYoKs3lPy88", "sceAgcDcbDrawIndirectMultiGetSize"); // PLT[818] pYoKs3lPy88#c#c
    AddNidAlias("r98I08t+LOg", "sceAgcDcbDrawIndexIndirectMultiGetSize"); // PLT[819] r98I08t+LOg#c#c
    AddNidAlias("mStuvI0zOtc", "sceAgcDcbDrawIndexIndirectGetSize"); // PLT[820] mStuvI0zOtc#c#c
    AddNidAlias("mR9j7+SfM34", "sceAgcDcbDrawIndexMultiInstancedGetSize"); // PLT[821] mR9j7+SfM34#c#c
    AddNidAlias("C4l9fB17t8w", "sceAgcDcbEventWriteGetSize"); // PLT[822] C4l9fB17t8w#c#c
    AddNidAlias("rUuVjyR+Rd4", "sceAgcDcbGetLodStatsGetSize"); // PLT[823] rUuVjyR+Rd4#c#c
    AddNidAlias("9S4noWrUI0s", "sceAgcDcbSetBaseDispatchIndirectArgsGetSize"); // PLT[824] 9S4noWrUI0s#c#c
    AddNidAlias("MMlmJAL7N5w", "sceAgcDcbSetBaseDrawIndirectArgsGetSize"); // PLT[825] MMlmJAL7N5w#c#c
    AddNidAlias("ca4KPvp0qLQ", "sceAgcDcbSetIndexSizeGetSize"); // PLT[826] ca4KPvp0qLQ#c#c
    AddNidAlias("j4emHHndCPY", "sceAgcDcbSetIndexBufferGetSize"); // PLT[827] j4emHHndCPY#c#c
    AddNidAlias("mljzuGDZRQ4", "sceAgcDcbSetIndexCountGetSize"); // PLT[828] mljzuGDZRQ4#c#c
    AddNidAlias("6DFuRKT4C9w", "sceAgcDcbSetNumInstancesGetSize"); // PLT[829] 6DFuRKT4C9w#c#c
    AddNidAlias("+u6dKSLWM2o", "sceAgcDcbStallCommandBufferParserGetSize"); // PLT[830] +u6dKSLWM2o#c#c
    AddNidAlias("43WJ08sSugE", "sceAgcDcbWaitOnAddressGetSize"); // PLT[831] 43WJ08sSugE#c#c
    AddNidAlias("QIXCsbipds0", "sceAgcDcbRewindGetSize"); // PLT[832] QIXCsbipds0#c#c
    AddNidAlias("ms1xVoZ-Vwc", "sceAgcDcbBeginOcclusionQueryGetSize"); // PLT[833] ms1xVoZ-Vwc#c#c
    AddNidAlias("P1CugZ99Uzc", "sceAgcDcbEndOcclusionQueryGetSize"); // PLT[834] P1CugZ99Uzc#c#c
    AddNidAlias("XN+Iuu7XsM8", "sceAgcDcbSetZPassPredicationEnableGetSize"); // PLT[835] XN+Iuu7XsM8#c#c
    AddNidAlias("yheJGN-ay+A", "sceAgcDcbSetBoolPredicationEnableGetSize"); // PLT[836] yheJGN-ay+A#c#c
    AddNidAlias("vLrBL8DQiz8", "sceAgcDcbSetPredicationDisableGetSize"); // PLT[837] vLrBL8DQiz8#c#c
    AddNidAlias("KjPeVduz6jU", "sceAgcDcbPrimeUtcl2GetSize"); // PLT[838] KjPeVduz6jU#c#c
    AddNidAlias("AFIh8SQkYlQ", "sceAgcDcbSetIndexIndirectArgsGetSize"); // PLT[839] AFIh8SQkYlQ#c#c
    AddNidAlias("7Wa3aeJgeVU", "sceAgcBranchPatchSetThenTarget_0300"); // PLT[840] 7Wa3aeJgeVU#c#c
    AddNidAlias("rP5xLdOf26k", "sceAgcBranchPatchSetElseTarget_0300"); // PLT[841] rP5xLdOf26k#c#c
    AddNidAlias("GXBlM-ekzrI", "sceAgcBranchPatchSetCompareAddress"); // PLT[842] GXBlM-ekzrI#c#c
    AddNidAlias("Ikfdt-rIqCE", "sceAgcJumpPatchSetTarget_0300"); // PLT[843] Ikfdt-rIqCE#c#c
    AddNidAlias("Qrj4c+61z4A", "sceAgcSetShRegIndirectPatchSetAddress"); // PLT[844] Qrj4c+61z4A#c#c
    AddNidAlias("nCUgItdN2ms", "sceAgcSetShRegIndirectPatchSetNumRegisters"); // PLT[845] nCUgItdN2ms#c#c
    AddNidAlias("z2duB-hHQSM", "sceAgcSetShRegIndirectPatchAddRegisters"); // PLT[846] z2duB-hHQSM#c#c
    AddNidAlias("vcmNN+AAXnY", "sceAgcSetCxRegIndirectPatchSetAddress"); // PLT[847] vcmNN+AAXnY#c#c
    AddNidAlias("whb1RL7K4Ss", "sceAgcSetCxRegIndirectPatchSetNumRegisters"); // PLT[848] whb1RL7K4Ss#c#c
    AddNidAlias("d-6uF9sZDIU", "sceAgcSetCxRegIndirectPatchAddRegisters"); // PLT[849] d-6uF9sZDIU#c#c
    AddNidAlias("6lNcCp+fxi4", "sceAgcSetUcRegIndirectPatchSetAddress"); // PLT[850] 6lNcCp+fxi4#c#c
    AddNidAlias("fRG-JOH5+sI", "sceAgcSetUcRegIndirectPatchSetNumRegisters"); // PLT[851] fRG-JOH5+sI#c#c
    AddNidAlias("vRoArM9zaIk", "sceAgcSetUcRegIndirectPatchAddRegisters"); // PLT[852] vRoArM9zaIk#c#c
    AddNidAlias("ORWsxIbk4TE", "sceAgcCondExecPatchSetEnd"); // PLT[853] ORWsxIbk4TE#c#c
    AddNidAlias("YWTKOju587o", "sceAgcCondExecPatchSetCommandAddress"); // PLT[854] YWTKOju587o#c#c
    AddNidAlias("k-JpyR2dYAM", "sceAgcAsyncCondExecPatchSetEnd"); // PLT[855] k-JpyR2dYAM#c#c
    AddNidAlias("3ZWa3AoyWZQ", "sceAgcAsyncCondExecPatchSetCommandAddress"); // PLT[856] 3ZWa3AoyWZQ#c#c
    AddNidAlias("ziVA3whp3p4", "sceAgcRewindPatchSetRewindState"); // PLT[857] ziVA3whp3p4#c#c
    AddNidAlias("eWaWyFegzgQ", "sceAgcAsyncRewindPatchSetRewindState"); // PLT[858] eWaWyFegzgQ#c#c
    AddNidAlias("w6Dj1VJt5qY", "sceAgcSetPacketPredication"); // PLT[859] w6Dj1VJt5qY#c#c
    AddNidAlias("n8vgpaQg6dA", "sceAgcSetRangePredication"); // PLT[860] n8vgpaQg6dA#c#c
    AddNidAlias("Lkf86B98qPc", "sceAgcGetPacketSize"); // PLT[861] Lkf86B98qPc#c#c
    AddNidAlias("K2mciNVxUCE", "sceAgcSetNop"); // PLT[862] K2mciNVxUCE#c#c
    AddNidAlias("V++UgBtQhn0", "sceAgcGetDataPacketPayloadAddress_0090"); // PLT[863] V++UgBtQhn0#c#c
    AddNidAlias("s+VGAMDQ0AQ", "sceAgcGetDataPacketPayloadRange"); // PLT[864] s+VGAMDQ0AQ#c#c
    AddNidAlias("IxYiarKlXxM", "sceAgcDmaDataPatchSetDstAddressOrOffset"); // PLT[865] IxYiarKlXxM#c#c
    AddNidAlias("cdDRpqcFGbU", "sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate"); // PLT[866] cdDRpqcFGbU#c#c
    AddNidAlias("3KDcnM3lrcU", "sceAgcWaitRegMemPatchAddress"); // PLT[867] 3KDcnM3lrcU#c#c
    AddNidAlias("n485EBnIWmk", "sceAgcWaitRegMemPatchCompareFunction"); // PLT[868] n485EBnIWmk#c#c
    AddNidAlias("7nOoijNPvEU", "sceAgcWaitRegMemPatchReference"); // PLT[869] 7nOoijNPvEU#c#c
    AddNidAlias("0fWWK5uG9rQ", "sceAgcQueueEndOfPipeActionPatchAddress"); // PLT[870] 0fWWK5uG9rQ#c#c
    AddNidAlias("J8YCgfKAMQs", "sceAgcQueueEndOfPipeActionPatchGcrCntl"); // PLT[871] J8YCgfKAMQs#c#c
    AddNidAlias("MlEw1feXcjg", "sceAgcQueueEndOfPipeActionPatchData"); // PLT[872] MlEw1feXcjg#c#c
    AddNidAlias("T9fjQIINoeE", "sceAgcQueueEndOfPipeActionPatchType"); // PLT[873] T9fjQIINoeE#c#c // sceAgcGetDefaultCxStateFlat
    AddNidAlias("QKd0qM58Qes", "sceKernelStopUnloadModule"); // 0x40a774a8ce7c41eb

    // POSIX networking aliases to sailor_kernel!sceNet*
    AddNidAlias("TU-d9PfIHPM", "sceNetSocket");
    AddNidAlias("KuOmgKoqCdY", "sceNetBind");
    AddNidAlias("pxnCmagrtao", "sceNetListen");
    AddNidAlias("XVL8So3QJUk", "sceNetConnect");
    AddNidAlias("3e+4Iv7IJ8U", "sceNetAccept");
    AddNidAlias("fZOeZIOEmLw", "sceNetSend");
    AddNidAlias("oBr313PppNE", "sceNetSendto");
    AddNidAlias("Ez8xjo9UF4E", "sceNetRecv");
    AddNidAlias("lUk6wrGXyMw", "sceNetRecvfrom");
    AddNidAlias("RenI1lL1WFk", "sceNetGetsockname");
    AddNidAlias("TXFFFiNldU8", "sceNetGetpeername");
    AddNidAlias("fFxGkxF2bVo", "sceNetSetsockopt");
    AddNidAlias("6O8EwYOgH9Y", "sceNetGetsockopt");
    AddNidAlias("TUuiYS2kE8s", "sceNetShutdown");
    AddNidAlias("bY-PO6JhzhQ", "sceNetSocketClose");
    AddNidAlias("lLMT9vJAck0", "sceKernelClockGettime");
    AddNidAlias("0wu33hunNdE", "sceKernelUsleep");

    // libSceHttp & libSceSsl Aliases
    AddNidAlias("A9cVMUtEp4Y", "sceHttpInit"); // 0x03D715314B44A786
    AddNidAlias("Ik-KpLTlf7Q", "sceHttpTerm"); // 0x224FCAA4B4E57FB4
    AddNidAlias("0gYjPTR-6cY", "sceHttpCreateTemplate"); // 0xD206233D347FE9C6
    AddNidAlias("4I8vEpuEhZ8", "sceHttpDeleteTemplate"); // 0xEB7F3575617EC6C4
    AddNidAlias("s2-NPIvz+iA", "sceHttpSetNonblock"); // 0xB36FCD3C8BF3FA20
    AddNidAlias("14bOACANTBo", "sceSslInit"); // 0xD786CE00200D4C1A
    AddNidAlias("0K1yQ6Lv-Yc", "sceSslTerm"); // 0xD0AD7243A2EFFD87
    AddNidAlias("+wCt7fCijgk", "sceHttp2Init"); // 0xFB00ADEDF0A28E09
    AddNidAlias("cRILAEvn+9M", "sceNpUnregisterNpReachabilityStateCallback"); // 0x71120B004BE7FBD3

    // CommonDialog & PlayerInvitationDialog Aliases
    AddNidAlias("uoUpLGNkygk", "sceCommonDialogInitialize"); // 0xBA85292C6364CA09
    AddNidAlias("hw5KNqAAels", "sceNpRegisterStateCallbackA"); // 0x870E4A36A0007A5B

    // AudioOut Aliases
    AddNidAlias("f7uOxY9mM1U", "sceAudioOut2Initialize"); // 0x7FBB8EC58F663355
    AddNidAlias("aaDMGGkXFxo", "sceAudioOut2Initialize"); // 0x69A0CC186917171A
    AddNidAlias("JrwFIMzKNr0", "sceAudioOut2LoInit");     // 0x26BC0520CCCA36BD



    // Find IAT VA of sailor_kernel!__sailor_unhandled_nid
    u64 unhandledNidIatVa = 0;
    auto unhandledIt = builtImports.functionIatRvas.find("__sailor_unhandled_nid");
    if (unhandledIt != builtImports.functionIatRvas.end()) {
        unhandledNidIatVa = effectiveBaseAddress + unhandledIt->second;
    }

    // Dynamic unhandled fallback stubs: synthesize a 24-byte stub for any unmapped symbol in dynamic info
    for (std::size_t i = 0; i < dynInfo.symbolNames.size(); ++i) {
        const std::string& symName = dynInfo.symbolNames[i];
        if (symName.empty()) continue;
        if (i < dynInfo.symbolValues.size() && dynInfo.symbolValues[i] != 0) continue; // internally defined

        std::string bareName = symName;
        auto hashPos = bareName.find('#');
        if (hashPos != std::string::npos) {
            bareName = bareName.substr(0, hashPos);
        }

        u64 symNid = (i < dynInfo.symbolNids.size()) ? dynInfo.symbolNids[i].Value() : 0;
        if (symNid == 0 && bareName.size() == 11) {
            try { symNid = Nid(bareName).Value(); } catch (...) {}
        }
        if (symNid == 0) {
            symNid = Nid::Compute(bareName).Value();
        }

        if (functionThunkVas.find(bareName) == functionThunkVas.end() &&
            functionThunkVas.find(symName) == functionThunkVas.end() &&
            nidThunkVas.find(symNid) == nidThunkVas.end()) {
            u64 stubVa = effectiveBaseAddress + thunkSectionRva + thunkSectionData.size();
            auto stubBytes = stubBuilder.SynthesizeUnhandledNidStub(symNid, unhandledNidIatVa);
            functionThunkVas[bareName] = stubVa;
            functionThunkVas[symName] = stubVa;
            nidThunkVas[symNid] = stubVa;
            thunkSectionData.insert(thunkSectionData.end(), stubBytes.begin(), stubBytes.end());
            SB_LOG_DEBUG("Relinker", "Synthesized dynamic fallback stub for NID 0x{:016X} ({}) at VA 0x{:X}", symNid, symName, stubVa);
        }
    }

    // Universal default fallback stub for completely unexpected relocations
    u64 defaultFallbackStubVa = effectiveBaseAddress + thunkSectionRva + thunkSectionData.size();
    auto defaultStubBytes = stubBuilder.SynthesizeUnhandledNidStub(0xDEADBEEFULL, unhandledNidIatVa);
    thunkSectionData.insert(thunkSectionData.end(), defaultStubBytes.begin(), defaultStubBytes.end());

    if (!thunkSectionData.empty()) {
        writer.AddSection(".thunk", 0x60000020, thunkSectionRva,
            static_cast<u32>(thunkSectionData.size()),
            thunkSectionData);
        currentRva = AlignUp(thunkSectionRva + static_cast<u32>(thunkSectionData.size()), 0x1000);
        SB_LOG_INFO("Relinker", "Generated {} direct jump thunks & fallback stubs in .thunk (size {} bytes)",
            functionThunkVas.size(), thunkSectionData.size());
    }

    // 3. Patch ELF GOT/PLT relocations
    size_t pltPatchedCount = 0;

    auto ProcessRelocList = [&](const std::vector<RelocEntry>& relocs) {
        for (const auto& rel : relocs) {
            u32 type = static_cast<u32>(rel.type);
            u32 symIdx = rel.symbolIndex;

            if (type == 7 /* R_X86_64_JUMP_SLOT */ || type == 6 /* R_X86_64_GLOB_DAT */ || type == 1 /* R_X86_64_64 */) {
                if (symIdx < dynInfo.symbolNames.size()) {
                    const std::string& symName = dynInfo.symbolNames[symIdx];
                    u64 targetVa = 0;

                    std::string bareName = symName;
                    auto hashPos = bareName.find('#');
                    if (hashPos != std::string::npos) {
                        bareName = bareName.substr(0, hashPos);
                    }

                    // 1. Try plain C symbol name match (bareName, then symName)
                    auto it = functionThunkVas.find(bareName);
                    if (it != functionThunkVas.end()) {
                        targetVa = it->second;
                    } else if ((it = functionThunkVas.find(symName)) != functionThunkVas.end()) {
                        targetVa = it->second;
                    } else if (symIdx < dynInfo.symbolNids.size()) {
                        // 2. Try Sony NID match from symIdx
                        u64 symNid = dynInfo.symbolNids[symIdx].Value();
                        auto nidIt = nidThunkVas.find(symNid);
                        if (nidIt != nidThunkVas.end()) {
                            targetVa = nidIt->second;
                        }
                    }

                    if (targetVa == 0 && bareName.size() == 11) {
                        try {
                            u64 bareNid = Nid(bareName).Value();
                            auto nidIt = nidThunkVas.find(bareNid);
                            if (nidIt != nidThunkVas.end()) {
                                targetVa = nidIt->second;
                            }
                        } catch (...) {}
                    }

                    // 3. Fallback: IAT directly
                    if (targetVa == 0) {
                        auto iatIt = builtImports.functionIatRvas.find(bareName);
                        if (iatIt != builtImports.functionIatRvas.end()) {
                            targetVa = effectiveBaseAddress + iatIt->second;
                        } else {
                            iatIt = builtImports.functionIatRvas.find(symName);
                            if (iatIt != builtImports.functionIatRvas.end()) {
                                targetVa = effectiveBaseAddress + iatIt->second;
                            }
                        }
                    }

                    // 4. Fallback: Internal defined ELF symbol (st_value != 0)
                    if (targetVa == 0 && symIdx < dynInfo.symbolValues.size() && dynInfo.symbolValues[symIdx] != 0) {
                        u64 symVal = dynInfo.symbolValues[symIdx];
                        if (isPie) {
                            symVal += pieSkew;
                        }
                        targetVa = (symVal >= effectiveBaseAddress)
                            ? symVal
                            : (effectiveBaseAddress + symVal);
                    }

                    // 5. Fallback: Never leave targetVa == 0! Route unhandled NIDs to dynamic fallback stub
                    if (targetVa == 0) {
                        targetVa = defaultFallbackStubVa;
                        SB_LOG_WARN("Relinker", "Relocation symbol '{}' (idx {}) unmapped, routing to fallback stub at 0x{:X}",
                            symName, symIdx, targetVa);
                    }

                    if (targetVa != 0 && type == 1 /* R_X86_64_64 */) {
                        targetVa += rel.addend;
                    }

                    u64 relOffset = rel.offset;
                    if (isPie) {
                        relOffset += pieSkew;
                    }
                    u32 rva = static_cast<u32>(
                        (relOffset >= effectiveBaseAddress)
                            ? (relOffset - effectiveBaseAddress)
                            : relOffset
                    );
                    if (targetVa != 0 && writer.PatchQword(rva, targetVa)) {
                        pltPatchedCount++;
                    }
                }
            } else if (type == 8 /* R_X86_64_RELATIVE */) {
                u64 relOffset = rel.offset;
                u64 targetAddend = rel.addend;
                if (isPie) {
                    relOffset += pieSkew;
                    targetAddend += pieSkew;
                }
                u32 rva = static_cast<u32>(
                    (relOffset >= effectiveBaseAddress)
                        ? (relOffset - effectiveBaseAddress)
                        : relOffset
                );
                u64 relocatedTarget = (targetAddend >= effectiveBaseAddress)
                    ? targetAddend
                    : (effectiveBaseAddress + targetAddend);
                writer.PatchQword(rva, relocatedTarget);
            } else if (type == 2 /* R_X86_64_PC32 */) {
                u64 targetVa = 0;
                if (symIdx < dynInfo.symbolNames.size()) {
                    const std::string& symName = dynInfo.symbolNames[symIdx];
                    std::string bareName = symName;
                    auto hashPos = bareName.find('#');
                    if (hashPos != std::string::npos) {
                        bareName = bareName.substr(0, hashPos);
                    }
                    auto it = functionThunkVas.find(bareName);
                    if (it != functionThunkVas.end()) {
                        targetVa = it->second;
                    } else if ((it = functionThunkVas.find(symName)) != functionThunkVas.end()) {
                        targetVa = it->second;
                    } else if (symIdx < dynInfo.symbolValues.size() && dynInfo.symbolValues[symIdx] != 0) {
                        u64 symVal = dynInfo.symbolValues[symIdx];
                        if (isPie) symVal += pieSkew;
                        targetVa = (symVal >= effectiveBaseAddress) ? symVal : (effectiveBaseAddress + symVal);
                    } else if (symIdx < dynInfo.symbolNids.size()) {
                        u64 symNid = dynInfo.symbolNids[symIdx].Value();
                        auto nidIt = nidThunkVas.find(symNid);
                        if (nidIt != nidThunkVas.end()) {
                            targetVa = nidIt->second;
                        }
                    }
                }
                if (targetVa != 0) {
                    u64 relOffset = rel.offset;
                    if (isPie) relOffset += pieSkew;
                    u64 pVa = (relOffset >= effectiveBaseAddress) ? relOffset : (effectiveBaseAddress + relOffset);
                    u32 rva = static_cast<u32>(pVa - effectiveBaseAddress);
                    i64 delta = static_cast<i64>(targetVa + rel.addend) - static_cast<i64>(pVa);
                    if (writer.PatchDword(rva, static_cast<u32>(static_cast<i32>(delta)))) {
                        pltPatchedCount++;
                    }
                }
            } else if (type == 9 /* R_X86_64_GOTPCREL */) {
                u64 gotEntryVa = 0;
                if (symIdx < dynInfo.symbolNames.size()) {
                    const std::string& symName = dynInfo.symbolNames[symIdx];
                    std::string bareName = symName;
                    auto hashPos = bareName.find('#');
                    if (hashPos != std::string::npos) {
                        bareName = bareName.substr(0, hashPos);
                    }
                    auto iatIt = builtImports.functionIatRvas.find(bareName);
                    if (iatIt != builtImports.functionIatRvas.end()) {
                        gotEntryVa = effectiveBaseAddress + iatIt->second;
                    } else if ((iatIt = builtImports.functionIatRvas.find(symName)) != builtImports.functionIatRvas.end()) {
                        gotEntryVa = effectiveBaseAddress + iatIt->second;
                    } else {
                        auto thunkIt = functionThunkVas.find(bareName);
                        if (thunkIt != functionThunkVas.end()) {
                            gotEntryVa = thunkIt->second;
                        }
                    }
                }
                if (gotEntryVa != 0) {
                    u64 relOffset = rel.offset;
                    if (isPie) relOffset += pieSkew;
                    u64 pVa = (relOffset >= effectiveBaseAddress) ? relOffset : (effectiveBaseAddress + relOffset);
                    u32 rva = static_cast<u32>(pVa - effectiveBaseAddress);
                    i64 delta = static_cast<i64>(gotEntryVa + rel.addend) - static_cast<i64>(pVa);
                    if (writer.PatchDword(rva, static_cast<u32>(static_cast<i32>(delta)))) {
                        pltPatchedCount++;
                    }
                }
            }
        }
    };

    ProcessRelocList(dynInfo.relocations);
    ProcessRelocList(dynInfo.pltRelocations);

    if (pltPatchedCount > 0) {
        SB_LOG_INFO("Relinker", "Successfully resolved and patched {} ELF GOT/PLT dynamic symbol relocations", pltPatchedCount);
    }

    // 4. Synthesize .entry (Bootstrap Entry Stub)
    u32 entryStubRva = currentRva;

    u32 exitProcessIatRva = builtImports.functionIatRvas["ExitProcess"];
    u64 exitProcessIatVa = isDll ? 0 : (effectiveBaseAddress + exitProcessIatRva);

    u64 initTlsIatVa = 0;
    if (!isDll) {
        auto initTlsIt = builtImports.functionIatRvas.find("__sailor_init_tls");
        if (initTlsIt != builtImports.functionIatRvas.end() && reader.GetTlsInfo().hasTls) {
            initTlsIatVa = effectiveBaseAddress + initTlsIt->second;
        }
    }

    std::vector<u64> remappedInitFns;
    for (u64 fnVa : dynInfo.initArrayFunctions) {
        if (fnVa == 0) continue;
        u64 remapped = fnVa;
        if (isPie) remapped += pieSkew;
        u64 finalVa = (remapped >= effectiveBaseAddress) ? remapped : (effectiveBaseAddress + remapped);
        remappedInitFns.push_back(finalVa);
    }

    u64 guestEntryVa = 0;
    if (elfEntry != 0) {
        u64 remappedElfEntry = elfEntry;
        if (isPie) {
            remappedElfEntry += pieSkew;
        }
        guestEntryVa = (remappedElfEntry >= effectiveBaseAddress)
            ? remappedElfEntry
            : (effectiveBaseAddress + remappedElfEntry);
    }

    u64 tlsTemplateVa = 0;
    u64 tlsFileSize = 0;
    u64 tlsMemSize = 0;
    u64 tlsAlign = 16;
    const auto& tlsInfo = reader.GetTlsInfo();
    if (!isDll && tlsInfo.hasTls) {
        u64 tlsVa = tlsInfo.vaddr;
        if (isPie) {
            tlsVa += pieSkew;
        }
        tlsTemplateVa = (tlsVa >= effectiveBaseAddress) ? tlsVa : (effectiveBaseAddress + tlsVa);
        tlsFileSize = tlsInfo.filesz;
        tlsMemSize = tlsInfo.memsz;
        tlsAlign = tlsInfo.align ? tlsInfo.align : 16;
    }

    auto stub = stubBuilder.Build(guestEntryVa, exitProcessIatVa, effectiveBaseAddress + entryStubRva, remappedInitFns, initTlsIatVa,
                                  tlsTemplateVa, tlsFileSize, tlsMemSize, tlsAlign);
    writer.AddSection(".entry", 0xE0000020, entryStubRva,
        static_cast<u32>(stub.codeData.size()),
        stub.codeData);

    writer.SetEntryPointRva(entryStubRva);
    writer.SetSubsystem(options.emitGuiSubsystem ? 2 /* IMAGE_SUBSYSTEM_WINDOWS_GUI */ : 3 /* IMAGE_SUBSYSTEM_WINDOWS_CUI */);
    writer.SetIsDll(isDll);

    // For main executable (eboot.exe), bind the master engine setup runner callback
    if (!isDll) {
        static constexpr u8 targetInstr[7] = { 0x48, 0x8D, 0x05, 0x36, 0x2E, 0x5C, 0x02 };
        if (writer.PatchBytes(0x1D33, targetInstr)) {
            static constexpr u8 skipCopy[2] = { 0xEB, 0x0F };
            writer.PatchBytes(0x1D41, skipCopy);
            SB_LOG_INFO("Relinker", "Successfully bound master engine bootstrap runner callback (0x1D33 -> 0x1425C4B70)");
        }

        // Skip destructor calls for uninitialized callback slots in 0x14000A250
        static constexpr u8 skipDestruct1[2] = { 0xEB, 0x0E };
        writer.PatchBytes(0xA7F5, skipDestruct1);
        static constexpr u8 skipDestruct2[2] = { 0xEB, 0x0F };
        writer.PatchBytes(0xA821, skipDestruct2);
        SB_LOG_INFO("Relinker", "Patched master callback destructors at 0xA7F5 and 0xA821");

        // Ensure gsInit (0x1405FA220) returns success (0x5FA303: 74 18 -> 90 90)
        static constexpr u8 nopGsInitJe[2] = { 0x90, 0x90 };
        writer.PatchBytes(0x5FA303, nopGsInitJe);

        // Bypass gsInit abort jump in 0x140001110 (RVA 0x1230: je 0x140001585 -> 6x NOP)
        static constexpr u8 nopSubsysJe[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        writer.PatchBytes(0x1230, nopSubsysJe);
        SB_LOG_INFO("Relinker", "Patched gsInit and subsystem 17 abort jump at 0x5FA303 and 0x1230");
    } else {
        if (currentModule == "pros.sdk.Prospero.dll") {
            // Bypass null buffer log formatting crash at 0xAE1B0 -> jump to 0xAE281
            // 0x64C0AE1B0: 0F 84 CB 00 00 00 (je 0xAE281) -> E9 CC 00 00 00 90 (jmp 0xAE281)
            static constexpr u8 jmpSkipLogFmt[6] = { 0xE9, 0xCC, 0x00, 0x00, 0x00, 0x90 };
            writer.PatchBytes(0xAE1B0, jmpSkipLogFmt);
            SB_LOG_INFO("Relinker", "Patched PROS offline logger to bypass null sink formatting at 0xAE1B0");
        }
    }

    if (!writer.BuildAndSave(outputPePath)) {
        SB_LOG_ERROR("Relinker", "Failed to generate Windows PE binary: {}", outputPePath.string());
        return false;
    }

    SB_LOG_INFO("Relinker", "Relink completed successfully: {}", outputPePath.string());
    return true;
}

} // namespace Sailor

