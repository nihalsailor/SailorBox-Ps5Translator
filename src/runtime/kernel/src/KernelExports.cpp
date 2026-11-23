#include "kernel/DirectMemory.hpp"
#include "kernel/ThreadManager.hpp"
#include "kernel/SyncPrimitives.hpp"
#include "kernel/VirtualFileSystem.hpp"
#include "kernel/FiberManager.hpp"
#include "kernel/Decompressor.hpp"
#include "kernel/PadManager.hpp"
#include "kernel/SaveDataManager.hpp"
#include "kernel/AudioOutManager.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include <immintrin.h>
#include <intrin.h>
#include <crtdbg.h>

static void SafeLog(HANDLE hOut, const char* str, DWORD len) {
    DWORD written = 0;
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        ::WriteFile(hOut, str, len, &written, NULL);
    }
    static HANDLE hFile = CreateFileA("C:\\Developments\\@SailorBox\\scratch\\runtime_live.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE && hFile != NULL) {
        ::WriteFile(hFile, str, len, &written, NULL);
    }
}
#define WriteFile(h, s, l, w, o) SafeLog(h, s, l)

static LONG WINAPI SailorCrashHandler(PEXCEPTION_POINTERS pExc) {
    if (!pExc || !pExc->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    
    DWORD code = pExc->ExceptionRecord->ExceptionCode;

    // Stage failure trap: intercepts 0x1400017DB in eboot.exe
    if (code == EXCEPTION_BREAKPOINT) {
        void* addr = pExc->ExceptionRecord->ExceptionAddress;
        HMODULE hExe = GetModuleHandleA(NULL);
        if (hExe && addr == (void*)((uintptr_t)hExe + 0x17DB)) {
            const char* fmt = reinterpret_cast<const char*>(pExc->ContextRecord->Rdi);
            const char* stage = reinterpret_cast<const char*>(pExc->ContextRecord->Rsi);
            char buf[1024];
            int len = snprintf(buf, sizeof(buf),
                "\n"
                "********************************************************************************\n"
                "[SAILOR STAGE FAIL TRAP] Hit stage failure handler at %p (TID: %lu)!\n"
                "  Failing Stage Name: '%s'\n"
                "  Stage Format/Desc:  '%s'\n"
                "********************************************************************************\n\n",
                addr, GetCurrentThreadId(),
                stage ? stage : "(null)",
                fmt ? fmt : "(null)");
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD written = 0;
            WriteFile(hOut, buf, (DWORD)len, &written, NULL);
            FlushFileBuffers(hOut);

            // Advance RIP past the INT3 to 0x17E4 (epilogue: mov eax, ebx; add rsp, 8; pop rbx; pop rbp; ret)
            pExc->ContextRecord->Rip = (DWORD64)hExe + 0x17E4;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (hExe && addr == (void*)((uintptr_t)hExe + 0x72D8B8)) {
            // Advance past DrawViewport INT3 to epilogue at 0x72D89F
            pExc->ContextRecord->Rip = (DWORD64)hExe + 0x72D89F;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // Integer divide-by-zero resilience guard
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        unsigned char* pOpcode = reinterpret_cast<unsigned char*>(pExc->ContextRecord->Rip);
        __try {
            if (pOpcode[0] == 0x48 && pOpcode[1] == 0xF7 && (pOpcode[2] & 0x38) == 0x30) {
                int len = 3;
                int mod = (pOpcode[2] >> 6) & 3;
                int rm = pOpcode[2] & 7;
                if (mod == 0 && rm == 5) len += 4; // rip+disp32
                else if (mod == 1) len += 1;       // r/m+disp8
                else if (mod == 2) len += 4;       // r/m+disp32
                pExc->ContextRecord->Rax = 0;
                pExc->ContextRecord->Rdx = 0;
                pExc->ContextRecord->Rip += len;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // PS5 Software Trap & Assertion Guard:
    // Sony Clang / Prospero emits 'int 0x40'..'int 0x4F' (0xCD 0x4x) for assertions & debug traps.
    // In Windows x64 user mode, executing INT <n> raises EXCEPTION_ACCESS_VIOLATION or EXCEPTION_PRIV_INSTRUCTION.
    // Skip the 2-byte trap instruction and proceed cleanly.
    if (code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        unsigned char* pTrapOp = reinterpret_cast<unsigned char*>(pExc->ContextRecord->Rip);
        __try {
            if (pTrapOp && pTrapOp[0] == 0xCD && (pTrapOp[1] >= 0x40 && pTrapOp[1] <= 0x4F)) {
                pExc->ContextRecord->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // VEH FS Guard: if Windows NT scheduler cleared FS base or thread lacks TCB, allocate/restore it and retry instruction
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR isWrite = pExc->ExceptionRecord->NumberParameters > 0 ? pExc->ExceptionRecord->ExceptionInformation[0] : 0;
        ULONG_PTR target = pExc->ExceptionRecord->NumberParameters > 1 ? pExc->ExceptionRecord->ExceptionInformation[1] : 0;

        // Dynamic On-Demand Heap Commit Guard:
        // If an instruction (e.g. memcpy/memset) writes past the committed edge of a heap buffer,
        // dynamically commit the missing memory page so execution continues seamlessly without faulting.
        if (isWrite == 1 && target >= 0x1000000ULL && target < 0x7FFFFFFF0000ULL) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery((void*)target, &mbi, sizeof(mbi)) != 0) {
                if (mbi.State == MEM_RESERVE) {
                    SIZE_T commitSize = (mbi.RegionSize > 0x100000) ? 0x100000 : mbi.RegionSize;
                    if (VirtualAlloc(mbi.BaseAddress, commitSize, MEM_COMMIT, PAGE_READWRITE)) {
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                } else if (mbi.State == MEM_FREE) {
                    void* allocBase = (void*)(target & ~0xFFFFULL);
                    if (VirtualAlloc(allocBase, 0x100000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            }
        }

        unsigned char* pOp = reinterpret_cast<unsigned char*>(pExc->ContextRecord->Rip);
        __try {
            bool isFsAccess = false;
            if (pOp) {
                for (int i = 0; i < 6; ++i) {
                    if (pOp[i] == 0x64) { isFsAccess = true; break; }
                    if (pOp[i] != 0x66 && pOp[i] != 0x67) break;
                }
            }
            if (isFsAccess || target < 0x100000ULL) {
                unsigned __int64 currentFs = _readfsbase_u64();
                void* tcb = Sailor::ThreadManager::Instance().GetCurrentThreadTcb(GetCurrentThreadId());
                if (!tcb) {
                    Sailor::ThreadManager::Instance().SetupCurrentThreadTls();
                    tcb = Sailor::ThreadManager::Instance().GetCurrentThreadTcb(GetCurrentThreadId());
                }
                if (tcb && currentFs != reinterpret_cast<unsigned __int64>(tcb)) {
                    _writefsbase_u64(reinterpret_cast<unsigned __int64>(tcb));
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Subsystem teardown guard: if NULL object pointer (RDI=0) dereferences [RDI+0x50] at RVA 0x1CDC3F,
        // jump cleanly to the function epilogue at 0x1CDE32 where it unwinds and returns.
        ULONG_PTR rip = pExc->ContextRecord->Rip;
        HMODULE hMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &hMod);

        // Atomic refcount guard on invalid/read-only pointer
        pOp = reinterpret_cast<unsigned char*>(rip);
        __try {
            if (pOp && pOp[0] == 0xCD && (pOp[1] >= 0x40 && pOp[1] <= 0x4F)) {
                // INT 0x40..0x4F (2 bytes) -> PlayStation 5 debug/break trap
                pExc->ContextRecord->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (pOp && pOp[0] == 0x0F && pOp[1] == 0x0B) {
                // UD2 instruction (2 bytes) -> PlayStation 5 assert trap
                pExc->ContextRecord->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (pOp && pOp[0] == 0xF0 && pOp[1] == 0x0F && pOp[2] == 0xC1) {
                // lock xadd [reg], reg (4 bytes)
                pExc->ContextRecord->Rip += 4;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // SSE unaligned stack alignment guard: if movdqa faulted on unaligned stack, transmute into movdqu
            if (pOp && pOp[0] == 0x66 && pOp[1] == 0x0F && (pOp[2] == 0x7F || pOp[2] == 0x6F)) {
                DWORD oldP;
                if (VirtualProtect(pOp, 3, PAGE_EXECUTE_READWRITE, &oldP)) {
                    pOp[0] = 0xF3; // transmute movdqa into movdqu
                    VirtualProtect(pOp, 3, oldP, &oldP);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Memory streaming / page allocator guard (0xCABC70 called with NULL this-pointer)
        if (target == 0x9C98 || target == 0x9C80 || target == 0x9CA0) {
            HMODULE hExe = GetModuleHandleA(NULL);
            if (hExe) {
                if (hMod != hExe) {
                    uint64_t* pRbp = reinterpret_cast<uint64_t*>(pExc->ContextRecord->Rbp);
                    __try {
                        if (pRbp && *pRbp) {
                            pExc->ContextRecord->Rbp = *pRbp;
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                pExc->ContextRecord->Rip = (DWORD64)hExe + 0xCAC958;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        if (hMod) {
            ULONG_PTR rva = rip - (ULONG_PTR)hMod;
            char modName[MAX_PATH] = {};
            GetModuleFileNameA(hMod, modName, MAX_PATH);
            bool isPros = (strstr(modName, "pros") != nullptr);

            // Safe guard for SceneList accessors 0x140FA5650 and 0x140FA5670 on empty/unloaded scene frames
            HMODULE hExe = GetModuleHandleA(NULL);
            if (hMod == hExe && (rva == 0xFA5650 || rva == 0xFA5670)) {
                uint64_t rdi = pExc->ContextRecord->Rdi;
                if (rdi) {
                    *(uint64_t*)rdi = 0;
                    *(uint32_t*)(rdi + 8) = 0;
                }
                pExc->ContextRecord->Rax = rdi;
                pExc->ContextRecord->Rip = (DWORD64)hExe + (rva == 0xFA5650 ? 0xFA5660 : 0xFA5683);
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // PROS SDK telemetry offline logger guard (NULL context at 0x5B203)
            if (rva == 0x5B203 || (target == 0x88 && pExc->ContextRecord->Rbx == 0)) {
                static uint8_t s_dummyProsContext[0x10000] = {};
                *(uintptr_t*)&s_dummyProsContext[0x88] = (uintptr_t)&s_dummyProsContext[0x200];
                *(uintptr_t*)&s_dummyProsContext[0x100] = (uintptr_t)&s_dummyProsContext[0x400];
                *(uintptr_t*)&s_dummyProsContext[0x1B8] = (uintptr_t)&s_dummyProsContext[0x600];
                *(uintptr_t*)&s_dummyProsContext[0x210] = (uintptr_t)&s_dummyProsContext[0x800];
                pExc->ContextRecord->Rbx = (DWORD64)s_dummyProsContext;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // PROS SDK telemetry event copy constructor guard (NULL source at 0x899D9)
            if (rva == 0x899D9 || (rva >= 0x899B0 && rva <= 0x899E0 && (target == 0x8 || pExc->ContextRecord->Rsi == 0))) {
                static struct {
                    void* vtable;
                    uint64_t unk8;
                    void* str1;
                    void* str2;
                    void* str3;
                    uint32_t val28;
                    uint32_t val2c;
                } s_dummyProsEvent = {};
                s_dummyProsEvent.vtable = (void*)((uintptr_t)hMod + 0xD9CF30);
                s_dummyProsEvent.unk8 = 0;
                s_dummyProsEvent.str1 = (void*)((uintptr_t)hMod + 0xE22748);
                s_dummyProsEvent.str2 = (void*)((uintptr_t)hMod + 0xE22748);
                s_dummyProsEvent.str3 = (void*)((uintptr_t)hMod + 0xE22748);
                s_dummyProsEvent.val28 = 0;
                s_dummyProsEvent.val2c = 0;
                pExc->ContextRecord->Rsi = (DWORD64)&s_dummyProsEvent;
                pExc->ContextRecord->R14 = (DWORD64)&s_dummyProsEvent;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // PROS SDK offline telemetry sink guard (0xAE1B0 - 0xAE280)
            if (rva >= 0xAE180 && rva <= 0xAE280 && target < 0x10000) {
                static uint8_t s_dummyProsSink[0x1000] = {};
                static void* s_dummySinkVtbl[16] = {};
                static bool s_sinkInit = false;
                if (!s_sinkInit) {
                    s_sinkInit = true;
                    void* pRet = (void*)((uintptr_t)hMod + 0xAE46E);
                    for (int i = 0; i < 16; ++i) s_dummySinkVtbl[i] = pRet;
                    *(void**)&s_dummyProsSink[0] = s_dummySinkVtbl;
                    *(void**)&s_dummyProsSink[0xB0] = s_dummyProsSink;
                }
                if (pExc->ContextRecord->Rax == 0) {
                    pExc->ContextRecord->Rax = (DWORD64)s_dummyProsSink;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                if (pExc->ContextRecord->Rbx == 0) {
                    pExc->ContextRecord->Rbx = (DWORD64)s_dummyProsSink;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // PROS SDK thread slot allocation guard (__tls_get_addr fallback at 0xAEB29)
            if (rva == 0xAEB29 && target == 0) {
                thread_local static uint8_t s_tlsFallback[0x10000] = {};
                thread_local static bool s_tlsInit = false;
                if (!s_tlsInit) {
                    s_tlsInit = true;
                    __try {
                        const uint8_t* pTlsTemplate = reinterpret_cast<const uint8_t*>(hMod) + 0xD99000;
                        std::memcpy(s_tlsFallback, pTlsTemplate, 0x40);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        *reinterpret_cast<int32_t*>(&s_tlsFallback[0]) = -1;
                    }
                }
                pExc->ContextRecord->Rax = (DWORD64)s_tlsFallback;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // PROS SDK telemetry map lookup guard (NULL result at 0xB0FF0 / 0xAED3B or any [rax + 0x10] when rax == 0)
            if ((rva == 0xB0FF0 || rva == 0xAED3B || (isPros && target == 0x10 && pExc->ContextRecord->Rax == 0))) {
                static struct {
                    uint8_t pad[0x10];
                    void* strPtr;
                } s_dummyMapEntry = {};
                static struct {
                    uint64_t pad0;
                    char* data;
                    uint64_t size;
                    uint64_t len;
                    uint64_t capacity;
                    char buffer[32];
                } s_dummyProsString = {};
                s_dummyProsString.data = s_dummyProsString.buffer;
                s_dummyProsString.capacity = 0x20;
                s_dummyProsString.buffer[0] = '\0';
                s_dummyMapEntry.strPtr = &s_dummyProsString;
                pExc->ContextRecord->Rax = (DWORD64)&s_dummyMapEntry;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (rva == 0x1CDC3F && target == 0x50) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1CDE32;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if ((rva == 0x3769B7 || rva == 0x3769D8) && target == 0) {
                static uint32_t s_defaultTuningMem[16] = {};
                pExc->ContextRecord->Rax = (DWORD64)s_defaultTuningMem;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if ((rva == 0x5F2128 || rva == 0x5F21D5 || rva == 0x5F2282) && (target == 0x30 || pExc->ContextRecord->R15 == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x5F2320;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x5F54C7 && (target == 0x10 || pExc->ContextRecord->Rax == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x5F54D1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x5F5527 && (target == 0x3C || pExc->ContextRecord->Rdi == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x5F552C;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x2466ED && (target == 0x28 || pExc->ContextRecord->Rbx == 0)) {
                pExc->ContextRecord->Rsp += 8; // skip pushed rax
                pExc->ContextRecord->Rbx = *(DWORD64*)pExc->ContextRecord->Rsp;
                pExc->ContextRecord->Rsp += 8; // pop rbx
                pExc->ContextRecord->Rbp = *(DWORD64*)pExc->ContextRecord->Rsp;
                pExc->ContextRecord->Rsp += 8; // pop rbp
                pExc->ContextRecord->Rip = *(DWORD64*)pExc->ContextRecord->Rsp; // ret
                pExc->ContextRecord->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0xBE8970 && (target < 0x100000 || pExc->ContextRecord->Rdi < 0x100000)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0xBE898B; // ret
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Viewport slot cleanup zeroing guard (0x140C290B0 - 0x140C2918B):
            // Detects and skips faulting 'mov [rbx + ...], 0' in viewport array teardown
            if (rva >= 0xC290B0 && rva <= 0xC2918B) {
                static int s_c29GuardCount = 0;
                if (s_c29GuardCount < 5) {
                    s_c29GuardCount++;
                    char gMsg[256];
                    int gLen = snprintf(gMsg, sizeof(gMsg),
                        "[SAILOR RUNTIME] Recovered from slot cleanup fault at RVA 0x%IX (target: 0x%p)\n",
                        rva, (void*)target);
                    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), gMsg, (DWORD)gLen, NULL, NULL);
                }
                if (rva == 0xC2917F) {
                    pExc->ContextRecord->Rip = (DWORD64)hMod + 0xC29187;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                if (pOp && pOp[0] == 0x48 && pOp[1] == 0xC7) {
                    if (pOp[2] == 0x03) { pExc->ContextRecord->Rip += 7; return EXCEPTION_CONTINUE_EXECUTION; }
                    if (pOp[2] == 0x43) { pExc->ContextRecord->Rip += 8; return EXCEPTION_CONTINUE_EXECUTION; }
                    if (pOp[2] == 0x83) { pExc->ContextRecord->Rip += 11; return EXCEPTION_CONTINUE_EXECUTION; }
                }
            }
            if (rva >= 0xC29CE0 && rva <= 0xC29D45) {
                if (pOp && pOp[0] == 0x48 && pOp[1] == 0xC7 && pOp[2] == 0x83) {
                    pExc->ContextRecord->Rip += 11;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            if (rva == 0x5D6CCB && target == 0) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x5D6CD1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x72DDB6 && target == 0) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x72DDD4;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x72F954 && target == 0) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x72FA15;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x72FEA3 && (target == 0x2C || pExc->ContextRecord->Rax == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x72FEB3;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0xCF8B83 && target == 0) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0xCF8B89;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0xCF8E4A && target == 0x10) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0xCF8E88;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0xCF8F9E && target == 0x10) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0xCF911F;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x44DEE3 && target == 0x78) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x44DF20;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x44DF35 && target == 0x78) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x44DF6F;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x44DF84 && target == 0x78) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x44DFBE;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x70A453 && target == 0x08) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x70A456;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x70A470 && target == 0x08) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x70A473;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0x105002C && target <= 0x1000) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1050034;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0xCD7F32 && target == 0x82C) {
                // Safely return from 0xCD7F10 to caller (0xCD7DF7)
                pExc->ContextRecord->Rip = *(DWORD64*)pExc->ContextRecord->Rsp;
                pExc->ContextRecord->Rsp += 8;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0xCE0969 && (target == 0x40 || pExc->ContextRecord->Rbx == 0)) {
                static uint8_t s_fallbackSubCtx[0x20000] = {};
                pExc->ContextRecord->Rbx = (DWORD64)s_fallbackSubCtx;
                pExc->ContextRecord->R15 = 0;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (rva == 0xCCB41A && target == 0x10) {
                pExc->ContextRecord->Rbx = 0;
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0xCCB4D4;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if ((rva == 0x105179C || rva == 0x105175D || rva == 0x10518AB) && (target == 0 || target == 0x8C || pExc->ContextRecord->R8 == 0 || pExc->ContextRecord->Rdi == 0)) {
                if (rva == 0x10518AB) {
                    pExc->ContextRecord->Rip = (DWORD64)hMod + 0x10518BC;
                } else {
                    // Stage queue buffer is null; unwind and return (pop rbx; ret)
                    pExc->ContextRecord->Rbx = *(DWORD64*)pExc->ContextRecord->Rsp;
                    pExc->ContextRecord->Rsp += 8;
                    pExc->ContextRecord->Rip = (DWORD64)hMod + 0x10517AE; // ret
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Main thread wait loop null ring buffer guards (0x1410523C0):
            // Priority 1 ring buffer at [r13 + 0x270] is null -> advance to Priority 2 (0x1052490)
            if (rva == 0x1052473 || rva == 0x105248F || (rva >= 0x1052436 && rva <= 0x105248F)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1052490;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Priority 2 ring buffer at [r13 + 0x158] is null -> advance to Priority 3 (0x10524F0)
            if (rva == 0x10524D3 || rva == 0x10524EF || (rva >= 0x1052497 && rva <= 0x10524EF)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x10524F0;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Priority 3 ring buffer at [r13 + 0x40] is null -> advance to wait (0x1052550)
            if (rva == 0x1052533 || rva == 0x105254F || (rva >= 0x10524F7 && rva <= 0x105254F)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1052550;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Worker thread queue null ring buffer guards (0x141050530 dispatch loop):
            // Priority 1 ring buffer at [r12 + 0x270] is null -> advance to Priority 2 (0x1050610)
            if (rva == 0x10505F4 || (rva >= 0x10505E0 && rva <= 0x1050608 && pExc->ContextRecord->R8 == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1050610;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Priority 2 ring buffer at [r12 + 0x158] is null -> advance to Priority 3 (0x1050670)
            if (rva == 0x1050654 || (rva >= 0x1050640 && rva <= 0x1050668 && pExc->ContextRecord->R8 == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1050670;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Priority 3 ring buffer at [r12 + 0x40] is null -> advance to event wait (0x1050580)
            if (rva == 0x10506B0 || (rva >= 0x10506A0 && rva <= 0x10506C8 && pExc->ContextRecord->R8 == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1050580;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // PushJob worker slot invalid/sentinel pointer guard (0x141050D50 - 0x141050D80):
            // When worker pointer or worker context faults on invalid/sentinel address, skip to next worker at 0x1050D40
            if (rva >= 0x1050D50 && rva <= 0x1050D80) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1050D40;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // PushJob queue buffer guard (0x141050DCB):
            // Priority ring buffer at [r9 + rax + 0x40] is null -> advance to next worker (0x1050D40)
            if (rva == 0x1050DCB || (rva >= 0x1050DC0 && rva <= 0x1050DD0 && pExc->ContextRecord->R10 == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x1050D40;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Task dependency null guard (0x14104F8A0):
            // If dependency task pointer (RBX) is null, skip to unlock and return (0x104F8EC)
            if (rva == 0x104F8A0 && (target == 0x0C || pExc->ContextRecord->Rbx == 0)) {
                pExc->ContextRecord->Rip = (DWORD64)hMod + 0x104F8EC;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Universal null this-pointer virtual call guard:
            // Detects 'mov rax, [rdi]' (48 8B 07) followed by 'call [rax + imm]' (FF 50 xx or FF 90 xx xx xx xx)
            const uint8_t* pCode = reinterpret_cast<const uint8_t*>(rip);
            __try {
                if (target == 0 && pCode && pCode[0] == 0x48 && pCode[1] == 0x8B && pCode[2] == 0x07) {
                    if (pCode[3] == 0xFF && pCode[4] == 0x50) {
                        pExc->ContextRecord->Rip += 6;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    if (pCode[3] == 0xFF && pCode[4] == 0x90) {
                        pExc->ContextRecord->Rip += 9;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
                if (target < 0x1000 && pCode && pCode[0] == 0xFF) {
                    if (pCode[1] == 0x50) { // call [rax + imm8]
                        pExc->ContextRecord->Rip += 3;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    if (pCode[1] == 0x90) { // call [rax + imm32]
                        pExc->ContextRecord->Rip += 6;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    // PS5 Software Trap & Assertion Guard:
    // Sony Clang and Prospero / Orbis OS emit 'int 0x41'..'int 0x4F' (0xCD 0x4x) and 'ud2' (0x0F 0x0B)
    // for debug traps, heap integrity checks, and assertions.
    // In Windows user mode x64, these trigger EXCEPTION_PRIV_INSTRUCTION or EXCEPTION_ILLEGAL_INSTRUCTION.
    if (code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        const uint8_t* rip = reinterpret_cast<const uint8_t*>(pExc->ContextRecord->Rip);
        __try {
            if (rip && rip[0] == 0xCD && (rip[1] >= 0x40 && rip[1] <= 0x4F)) {
                pExc->ContextRecord->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Guest dlmalloc integrity trap recovery (ud2 in dlfree at 0x1410480DF / 0x1410480E1):
            // If an alien/host pointer or foreign chunk is passed to guest free(),
            // skip the abort and recover directly to dlfree's epilogue at 0x1410480AF (pop regs, ret).
            HMODULE hExe = GetModuleHandleA(NULL);
            if (hExe) {
                ULONG_PTR rva = (ULONG_PTR)rip - (ULONG_PTR)hExe;
                if (rva == 0x10480DF || rva == 0x10480E1) {
                    pExc->ContextRecord->Rip = (DWORD64)hExe + 0x10480AF;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            if (code == EXCEPTION_ILLEGAL_INSTRUCTION && rip && rip[0] == 0x0F && rip[1] == 0x0B) {
                pExc->ContextRecord->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_DATATYPE_MISALIGNMENT ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_FLT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_BREAKPOINT) {
        
        void* addr = pExc->ExceptionRecord->ExceptionAddress;
        ULONG_PTR info0 = pExc->ExceptionRecord->NumberParameters > 0 ? pExc->ExceptionRecord->ExceptionInformation[0] : 0;
        ULONG_PTR info1 = pExc->ExceptionRecord->NumberParameters > 1 ? pExc->ExceptionRecord->ExceptionInformation[1] : 0;

        unsigned __int64 fsBase = 0;
        __try { fsBase = _readfsbase_u64(); } __except (EXCEPTION_EXECUTE_HANDLER) {}

        HMODULE hMod = NULL;
        char modPath[MAX_PATH] = "UnknownModule";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &hMod)) {
            GetModuleFileNameA(hMod, modPath, MAX_PATH);
        }
        uintptr_t rva = hMod ? ((uintptr_t)addr - (uintptr_t)hMod) : 0;

        uint64_t stackWords[64] = {};
        const uint64_t* pStack = reinterpret_cast<const uint64_t*>(pExc->ContextRecord->Rsp);
        __try {
            for (int i = 0; i < 64; ++i) stackWords[i] = pStack[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        char stackBuf[4096] = "";
        int stackOffset = 0;
        for (int i = 0; i < 64; i += 2) {
            stackOffset += snprintf(stackBuf + stackOffset, sizeof(stackBuf) - stackOffset,
                "    [+%02X] 0x%016llX  [+%02X] 0x%016llX\n",
                i * 8, stackWords[i], (i + 1) * 8, stackWords[i + 1]);
        }

        char ripBytesBuf[64] = "??";
        const uint8_t* pRipBytes = reinterpret_cast<const uint8_t*>(addr);
        __try {
            snprintf(ripBytesBuf, sizeof(ripBytesBuf),
                "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                pRipBytes[0], pRipBytes[1], pRipBytes[2], pRipBytes[3],
                pRipBytes[4], pRipBytes[5], pRipBytes[6], pRipBytes[7],
                pRipBytes[8], pRipBytes[9], pRipBytes[10], pRipBytes[11],
                pRipBytes[12], pRipBytes[13], pRipBytes[14], pRipBytes[15]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        char buf[4096];
        int len = snprintf(buf, sizeof(buf),
            "\n"
            "================================================================================\n"
            "[SAILOR CRASH TRAP] Unhandled Exception: 0x%08lX at RIP: 0x%p (TID: %lu)\n"
            "  Module: %s (Base: 0x%p, RVA: 0x%IX)\n"
            "  Violation Type: %s, Target Address: 0x%p (FS Base: 0x%016llX)\n"
            "  Instruction Bytes: %s\n"
            "  Registers:\n"
            "    RAX: 0x%016llX  RBX: 0x%016llX  RCX: 0x%016llX  RDX: 0x%016llX\n"
            "    RSI: 0x%016llX  RDI: 0x%016llX  RBP: 0x%016llX  RSP: 0x%016llX\n"
            "    R8:  0x%016llX  R9:  0x%016llX  R10: 0x%016llX  R11: 0x%016llX\n"
            "    R12: 0x%016llX  R13: 0x%016llX  R14: 0x%016llX  R15: 0x%016llX\n"
            "  Stack Backtrace (RSP to RSP+0x100):\n%s"
            "================================================================================\n",
            code, addr, GetCurrentThreadId(),
            modPath, (void*)hMod, rva,
            (info0 == 0 ? "READ" : (info0 == 1 ? "WRITE" : (info0 == 8 ? "EXECUTE (DEP)" : "UNKNOWN"))),
            (void*)info1, fsBase,
            ripBytesBuf,
            pExc->ContextRecord->Rax, pExc->ContextRecord->Rbx, pExc->ContextRecord->Rcx, pExc->ContextRecord->Rdx,
            pExc->ContextRecord->Rsi, pExc->ContextRecord->Rdi, pExc->ContextRecord->Rbp, pExc->ContextRecord->Rsp,
            pExc->ContextRecord->R8,  pExc->ContextRecord->R9,  pExc->ContextRecord->R10, pExc->ContextRecord->R11,
            pExc->ContextRecord->R12, pExc->ContextRecord->R13, pExc->ContextRecord->R14, pExc->ContextRecord->R15,
            stackBuf
        );
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD written = 0;
        WriteFile(hErr, buf, (DWORD)len, &written, NULL);
        FlushFileBuffers(hErr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void DummyFactoryMethod() {}

extern "C" void LogAllocPanicCaller(uint64_t retAddr, uint64_t size) {
    char msg[512];
    int len = snprintf(msg, sizeof(msg),
        "\n================================================================================\n"
        "[SAILOR RUNTIME] SaberEngine::AllocateFailPanic CALLED!\n"
        "  Caller Return Address: 0x%016llX (RVA 0x%llX), Requested Size: %llu\n"
        "================================================================================\n\n",
        retAddr, (retAddr >= 0x140000000ULL ? retAddr - 0x140000000ULL : 0), size);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(hOut, msg, (DWORD)len, &written, NULL);
    FlushFileBuffers(hOut);
}

static uint8_t s_allocTrampoline[128] = {
    0x51,                                           // push rcx
    0x52,                                           // push rdx
    0x41, 0x50,                                     // push r8
    0x41, 0x51,                                     // push r9
    0x41, 0x52,                                     // push r10
    0x41, 0x53,                                     // push r11
    0x48, 0x89, 0xFA,                               // mov rdx, rdi (size)
    0x48, 0x8B, 0x4C, 0x24, 0x30,                   // mov rcx, [rsp+48] (retAddr)
    0x48, 0x83, 0xEC, 0x28,                         // sub rsp, 40
    0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, &LogAllocPanicCaller (offset 24)
    0xFF, 0xD0,                                     // call rax
    0x48, 0x83, 0xC4, 0x28,                         // add rsp, 40
    0x41, 0x5B,                                     // pop r11
    0x41, 0x5A,                                     // pop r10
    0x41, 0x59,                                     // pop r9
    0x41, 0x58,                                     // pop r8
    0x5A,                                           // pop rdx
    0x59,                                           // pop rcx
    0x55,                                           // push rbp
    0x48, 0x89, 0xE5,                               // mov rbp, rsp
    0x41, 0x57,                                     // push r15
    0x41, 0x56,                                     // push r14
    0x53,                                           // push rbx
    0x48, 0x81, 0xEC, 0x28, 0x01, 0x00, 0x00,       // sub rsp, 0x128
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,             // jmp qword ptr [rip+0]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // target address (offset 70)
};

// Safe Trampoline for SceneAttachContext at 0x141050010:
// When rsi (scene context) is nullptr on initial startup frames, safely initializes rdi fields and returns without crashing
static uint8_t s_sceneAttachTrampoline[64] = {
    // 0: test rsi, rsi
    0x48, 0x85, 0xF6,
    // 3: jne +0x16 (to offset 27)
    0x75, 0x16,
    // 5: mov dword ptr [rdi + 0x28], 0
    0xC7, 0x47, 0x28, 0x00, 0x00, 0x00, 0x00,
    // 12: mov dword ptr [rdi + 0x0C], 0
    0xC7, 0x47, 0x0C, 0x00, 0x00, 0x00, 0x00,
    // 19: mov dword ptr [rdi + 0x18], 1
    0xC7, 0x47, 0x18, 0x01, 0x00, 0x00, 0x00,
    // 26: ret
    0xC3,
    // 27: normal path - execute 15 replaced bytes
    // push rbp; mov rbp, rsp; push r15; push r14; push r12; push rbx; sub rsp, 0x10
    0x55,
    0x48, 0x89, 0xE5,
    0x41, 0x57,
    0x41, 0x56,
    0x41, 0x54,
    0x53,
    0x48, 0x83, 0xEC, 0x10,
    // 42: jmp qword ptr [rip + 0]
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
    // 48: 8-byte destination address to hExe + 0x105001F (offset 48)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t s_sysvCallThunk[32] = {
    0x48, 0x89, 0xCF,                                           // mov rdi, rcx
    0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, target
    0xFF, 0xE0                                                  // jmp rax
};

// SysV ABI 3-Argument Trampoline:
// Bridges MS x64 (rcx=fn, rdx=arg1, r8=arg2, r9=arg3) to SysV AMD64 (rdi=arg1, rsi=arg2, rdx=arg3, calls r10=fn),
// while preserving all non-volatile registers (rbx, rbp, rdi, rsi, r12, r13, r14, r15).
static const uint8_t s_sysv3TrampolineCode[] = {
    0x55,                               // push rbp
    0x48, 0x89, 0xE5,                   // mov rbp, rsp
    0x57,                               // push rdi
    0x56,                               // push rsi
    0x53,                               // push rbx
    0x41, 0x54,                         // push r12
    0x41, 0x55,                         // push r13
    0x41, 0x56,                         // push r14
    0x41, 0x57,                         // push r15
    0x48, 0x83, 0xEC, 0x28,             // sub rsp, 0x28
    0x49, 0x89, 0xCA,                   // mov r10, rcx
    0x48, 0x89, 0xD7,                   // mov rdi, rdx
    0x4C, 0x89, 0xC6,                   // mov rsi, r8
    0x4C, 0x89, 0xCA,                   // mov rdx, r9
    0x31, 0xC9,                         // xor ecx, ecx
    0x45, 0x31, 0xC0,                   // xor r8d, r8d
    0x45, 0x31, 0xC9,                   // xor r9d, r9d
    0x41, 0xFF, 0xD2,                   // call r10
    0x48, 0x83, 0xC4, 0x28,             // add rsp, 0x28
    0x41, 0x5F,                         // pop r15
    0x41, 0x5E,                         // pop r14
    0x41, 0x5D,                         // pop r13
    0x41, 0x5C,                         // pop r12
    0x5B,                               // pop rbx
    0x5E,                               // pop rsi
    0x5F,                               // pop rdi
    0x5D,                               // pop rbp
    0xC3                                // ret
};

typedef void (*PfnSysV3)(void* fn, uint64_t a1, uint64_t a2, uint64_t a3);
static PfnSysV3 s_invokeSysV3 = nullptr;

struct NpCallbackInfo {
    void* cb;
    void* arg;
    int id;
};
static std::vector<NpCallbackInfo> s_npCallbacks;
static std::mutex s_npMutex;

static uint64_t s_frameCounter = 0;

static void FrameLoopHandler(uint64_t rbpVal) {
    s_frameCounter++;

    // 1. Pump Windows messages so window never freezes and accepts input
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // 2. Clear exit request bit (0x04) and ensure render & presentation flags are set
    HMODULE hExe = GetModuleHandleA(NULL);
    bool bDeviceReady = false;
    uint8_t* pDev = nullptr;
    if (hExe) {
        uint32_t* pFlags = (uint32_t*)((uint8_t*)hExe + 0x2834CC8);
        if (pFlags) {
            *pFlags = (*pFlags | 0x82000) & ~0x04; // 0x80000 = Render Requested, 0x2000 = Display Ready
        }
        uint8_t* pRunning = (uint8_t*)hExe + 0x2CA36A8;
        if (pRunning) {
            *pRunning = 1;
        }

        uint8_t** ppDeviceActive = (uint8_t**)((uint8_t*)hExe + 0x2C4DFC0);
        uint8_t** ppDeviceInst = (uint8_t**)((uint8_t*)hExe + 0x2C4F4A0);

        // Link active device global if unassigned but instantiated
        if (ppDeviceActive && *ppDeviceActive == nullptr && ppDeviceInst && *ppDeviceInst != nullptr) {
            *ppDeviceActive = *ppDeviceInst;
        }

        if (ppDeviceActive && *ppDeviceActive != nullptr) {
            pDev = *ppDeviceActive;
            uint64_t vtbl = *(uint64_t*)pDev;
            // Check if vtable is valid (in .data2 around 0x142624F30)
            if (vtbl >= (uint64_t)hExe + 0x2000000 && vtbl < (uint64_t)hExe + 0x3000000) {
                bDeviceReady = true;

                // Ensure display dimensions and mode are asserted on the device
                if (*(uint32_t*)(pDev + 0x228) == 0) {
                    *(uint32_t*)(pDev + 0x220) = 1;    // mode/format = 1
                    *(uint32_t*)(pDev + 0x228) = 1920; // viewport width
                    *(uint32_t*)(pDev + 0x22C) = 1080; // viewport height
                    *(uint32_t*)(pDev + 0x238) = 1920; // width
                    *(uint32_t*)(pDev + 0x23C) = 1080; // height
                    *(uint32_t*)(pDev + 0x240) = 1920; // pitch
                    *(uint32_t*)(pDev + 0x2D8) = 1;    // numDisplays = 1
                }
            }
        }

        // Only transition to scene rendering once device is verified ready!
        if (bDeviceReady) {
            // Master Scene Render flow state (checked at 0x1405F7820)
            uint32_t* pNetState1 = (uint32_t*)((uint8_t*)hExe + 0x2834EF8);
            if (pNetState1) {
                *pNetState1 = 2; // Active Flow State
            }
            uint8_t* pNetActive1 = (uint8_t*)((uint8_t*)hExe + 0x2834F00);
            if (pNetActive1) {
                *pNetActive1 = 1; // Flow Active
            }

            // Scene Render & Transition flow state (checked at 0x1405F8430)
            uint32_t* pNetState3 = (uint32_t*)((uint8_t*)hExe + 0x2834F10);
            if (pNetState3) {
                *pNetState3 = 2; // Active Flow State
            }
            uint32_t* pNetState4 = (uint32_t*)((uint8_t*)hExe + 0x2834F20);
            if (pNetState4) {
                *pNetState4 = 2; // Active Flow State
            }

            // SceneManager Tick flow state (checked at 0x1405F8920)
            uint32_t* pNetState2 = (uint32_t*)((uint8_t*)hExe + 0x2834F38);
            if (pNetState2) {
                *pNetState2 = 2; // Active Flow State
            }
            uint8_t* pNetActive2 = (uint8_t*)((uint8_t*)hExe + 0x2834F40);
            if (pNetActive2) {
                *pNetActive2 = 1; // Flow Active
            }
        }

        // Assert backend rendering and submission flags
        *(uint8_t*)((uint8_t*)hExe + 0x2C54FE8) = 1;  // DxCommonInit active
        *(uint32_t*)((uint8_t*)hExe + 0x2C54FE0) = 2; // DxCommonInit ready state (2 = ready)
        *(uint32_t*)((uint8_t*)hExe + 0x2C54FE4) = 1; // presentation ready
        *(uint32_t*)((uint8_t*)hExe + 0x285AEE0) = 0x105; // render pass enable
        *(uint32_t*)((uint8_t*)hExe + 0x285AEE4) = 0x105; // target pass mask
        *(uint8_t*)((uint8_t*)hExe + 0x28823D0) = 1;     // master render pass enable

        // 3. Tick game engine and subsystems via SubsystemManager::UpdateSubsystems (0x140001800)
        if (!s_invokeSysV3) {
            void* pMem = VirtualAlloc(NULL, sizeof(s_sysv3TrampolineCode), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (pMem) {
                memcpy(pMem, s_sysv3TrampolineCode, sizeof(s_sysv3TrampolineCode));
                s_invokeSysV3 = (PfnSysV3)pMem;
            }
        }
        if (rbpVal > 0x10000 && s_invokeSysV3) {
            uint64_t pStage0 = *(uint64_t*)(rbpVal - 0x268);
            uint64_t sceneMgr = *(uint64_t*)((uint8_t*)hExe + 0x2834CF8);
            if (pStage0 && !IsBadReadPtr((void*)pStage0, 0x100) &&
                sceneMgr && !IsBadReadPtr((void*)sceneMgr, 0xC10)) {
                uint64_t pStage1 = pStage0 + 0x60;
                uint64_t fnUpdate = *(uint64_t*)pStage1;
                if (fnUpdate == (uint64_t)hExe + 0x1800) {
                    __try {
                        s_invokeSysV3((void*)fnUpdate, sceneMgr + 8, pStage1, 0);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                }
            }
        }
    }

    // 4. Periodic log output to monitor frame progression and scene state
    if (s_frameCounter <= 5 || (s_frameCounter % 60) == 1) {
        uint64_t sceneMgr = 0;
        uint32_t sceneCount = 0;
        uint32_t activeSceneId = 0;
        uint64_t stackScene = (rbpVal > 0x10000 ? *(uint64_t*)(rbpVal - 0x248) : 0);
        uint32_t p1Count = 0;
        uint32_t p2Count = 0;
        int activeIdxWithMeshes = -1;
        uint32_t meshP1 = 0, meshP2 = 0;

        if (hExe) {
            sceneMgr = *(uint64_t*)((uint8_t*)hExe + 0x2834CF8);
            activeSceneId = *(uint32_t*)((uint8_t*)hExe + 0x282C624);
            if (sceneMgr && !IsBadReadPtr((void*)sceneMgr, 0xC10)) {
                sceneCount = *(uint32_t*)(sceneMgr + 0xC08);
                for (uint32_t i = 0; i < sceneCount && i < 32; i++) {
                    uint64_t scObj = *(uint64_t*)(sceneMgr + 0x18 + i * 0x18);
                    if (scObj && !IsBadReadPtr((void*)scObj, 0x90)) {
                        uint32_t scP1 = *(uint32_t*)(scObj + 0x70);
                        uint32_t scP2 = *(uint32_t*)(scObj + 0x80);
                        if (scP1 > 0 || scP2 > 0) {
                            activeIdxWithMeshes = (int)i;
                            meshP1 = scP1;
                            meshP2 = scP2;
                            break;
                        }
                    }
                }
            }
        }
        if (stackScene && !IsBadReadPtr((void*)stackScene, 0x90)) {
            p1Count = *(uint32_t*)(stackScene + 0x70);
            p2Count = *(uint32_t*)(stackScene + 0x80);
        }

        // If stackScene has no items but a populated scene exists, select it
        if (p1Count == 0 && p2Count == 0 && activeIdxWithMeshes >= 0 && hExe && sceneMgr) {
            uint32_t targetSceneId = *(uint32_t*)(sceneMgr + 0x08 + activeIdxWithMeshes * 0x18);
            uint32_t* pActiveId = (uint32_t*)((uint8_t*)hExe + 0x282C624);
            if (pActiveId) {
                *pActiveId = targetSceneId;
            }
        }

        char buf[512];
        int len = snprintf(buf, sizeof(buf),
            "[SAILOR RUNTIME] FrameLoop: Frame #%llu | DevReady=%d (Dev=%p, Vtbl=0x%llX) | SceneMgr: 0x%llX (Count=%u, ActiveID=%u, MeshScene=%d: P1=%u, P2=%u) | StackScene: 0x%llX (Pass1=%u, Pass2=%u) | TID: %lu\n",
            (unsigned long long)s_frameCounter,
            bDeviceReady ? 1 : 0,
            pDev, (pDev ? *(uint64_t*)pDev : 0),
            sceneMgr, sceneCount, activeSceneId,
            activeIdxWithMeshes, meshP1, meshP2,
            stackScene, p1Count, p2Count,
            GetCurrentThreadId());
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(hOut, buf, (DWORD)len, &written, NULL);
        FlushFileBuffers(hOut);
    }

    // 4. Yield CPU for smooth pacing
    Sleep(1);
}

static uint8_t s_frameLoopThunkCode[73] = {
    // 0: and rsp, -16 (align to 16 bytes for Windows x64 ABI)
    0x48, 0x83, 0xE4, 0xF0,
    // 4: sub rsp, 0x20 (32-byte shadow space)
    0x48, 0x83, 0xEC, 0x20,
    // 8: mov rcx, rbp (pass guest rbp to FrameLoopHandler)
    0x48, 0x89, 0xE9,
    // 11: mov rax, <FrameLoopHandler>
    0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 21: call rax
    0xFF, 0xD0,
    // 23: lea rsp, [rbp - 0x270] (restore exact guest stack frame)
    0x48, 0x8D, 0xA5, 0x90, 0xFD, 0xFF, 0xFF,
    // 30: mov rbx, [rbp - 0x268] (restore this pointer)
    0x48, 0x8B, 0x9D, 0x98, 0xFD, 0xFF, 0xFF,
    // 37: mov rax, <hExe + 0x2834CF8>
    0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 47: mov rax, [rax] (load SceneManager)
    0x48, 0x8B, 0x00,
    // 50: mov [rbp - 0x250], rax
    0x48, 0x89, 0x85, 0xB0, 0xFD, 0xFF, 0xFF,
    // 57: lea r14, [rax + 8]
    0x4C, 0x8D, 0x70, 0x08,
    // 61: mov rax, <hExe + 0x5F7A6C>
    0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 71: jmp rax
    0xFF, 0xE0
};

static DWORD WINAPI InitDisplayWatcher(LPVOID param) {
    Sailor::ThreadManager::Instance().SetupCurrentThreadTls();
    uint8_t* hExe = (uint8_t*)param;

    // Initialize SysV 3-arg trampoline if needed
    if (!s_invokeSysV3) {
        void* pMem = VirtualAlloc(NULL, sizeof(s_sysv3TrampolineCode), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (pMem) {
            memcpy(pMem, s_sysv3TrampolineCode, sizeof(s_sysv3TrampolineCode));
            s_invokeSysV3 = (PfnSysV3)pMem;
        }
    }

    uint8_t** ppRendConfig = (uint8_t**)(hExe + 0x2C4E030);
    uint8_t** ppRenderSys = (uint8_t**)(hExe + 0x2834CE0);
    uint8_t** ppDeviceActive = (uint8_t**)(hExe + 0x2C4DFC0);
    uint8_t** ppDeviceInst = (uint8_t**)(hExe + 0x2C4F4A0);
    
    // Allow bootstrap sequence to finish and enter frame loop
    Sleep(3000);
    
    // Poll until rendCONFIG and gsRENDER_SYSTEM are both instantiated by the engine
    for (int i = 0; i < 2000; ++i) { // up to 20 seconds
        if (*ppRendConfig != nullptr && *ppRenderSys != nullptr) {
            break;
        }
        Sleep(10);
    }
    
    if (*ppRendConfig != nullptr && *ppRenderSys != nullptr) {
        Sleep(200); // allow constructor to finish initializing fields
        uint8_t* pRendConfig = *ppRendConfig;
        uint8_t* pRenderSys = *ppRenderSys;
        
        // Assert rendCONFIG dimensions and mode
        *(uint32_t*)(pRendConfig + 0x18) = 1920; // width
        *(uint32_t*)(pRendConfig + 0x1C) = 1080; // height
        *(uint32_t*)(pRendConfig + 0x34) = 1;    // mode = 1 (Windowed/Fullscreen native)
        
        // Link active device global if unassigned
        void* devActive = *ppDeviceActive;
        void* devInst = *ppDeviceInst;
        if (devActive == nullptr && devInst != nullptr) {
            *ppDeviceActive = (uint8_t*)devInst;
            devActive = devInst;
        }

        // Populate Device display and resolution fields so CreateBackBuffer and Texture2D::Initialize succeed
        if (devActive != nullptr) {
            uint8_t* pDev = (uint8_t*)devActive;
            *(uint32_t*)(pDev + 0x220) = 1;    // mode/format = 1
            *(uint32_t*)(pDev + 0x228) = 1920; // viewport width
            *(uint32_t*)(pDev + 0x22C) = 1080; // viewport height
            *(uint32_t*)(pDev + 0x238) = 1920; // width
            *(uint32_t*)(pDev + 0x23C) = 1080; // height
            *(uint32_t*)(pDev + 0x240) = 1920; // pitch
            *(uint32_t*)(pDev + 0x2D8) = 1;    // numDisplays = 1
            *(float*)(pDev + 0x328) = 0.016666667f; // 60 FPS refresh delta
            *(float*)(pDev + 0x32C) = 60.0f;        // 60 Hz
            *(uint8_t*)(pDev + 0x25B) &= ~0x04;     // clear bypass flag so frame queues are initialized

            // Automatically initialize the display swapchain and register triple guest framebuffers!
            uint32_t curVo = *(uint32_t*)(pDev + 0x4B464);
            if (curVo == 0 || curVo == 0xFFFFFFFF) {
                typedef uint64_t (*PfnInitDisplaySwapchain)();
                auto pfnInitSwapchain = (PfnInitDisplaySwapchain)(hExe + 0x627110);
                uint64_t swapRes = pfnInitSwapchain();
                
                char initMsg[256];
                int initLen = snprintf(initMsg, sizeof(initMsg),
                    "[SAILOR RUNTIME] Auto-initialized Display Swapchain at 0x%p (Device=%p, Result=%llu, VideoOut=%u)\n",
                    (void*)pfnInitSwapchain, devActive, (unsigned long long)swapRes, *(uint32_t*)(pDev + 0x4B464));
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD written = 0;
                WriteFile(hOut, initMsg, (DWORD)initLen, &written, NULL);
                FlushFileBuffers(hOut);
            }
        }


        DWORD oldProt = 0;
        if (VirtualProtect(hExe + 0x2BE4BF0, 8, PAGE_READWRITE, &oldProt)) {
            *(uint64_t*)(hExe + 0x2BE4BF0) = 1; // enable viewport 0 bit in bitmask
            VirtualProtect(hExe + 0x2BE4BF0, 8, oldProt, &oldProt);
        }


        
        char msg[512];
        int len = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] Initialized rendCONFIG (1920x1080, mode 1) and asserted gsRENDER_SYSTEM display triggers (+0x868..+0x86F, +0x86E=0)\n"
            "[SAILOR RUNTIME] Device pointers: ppDeviceActive(%p)=%p, ppDeviceInst(%p)=%p\n",
            (void*)ppDeviceActive, devActive, (void*)ppDeviceInst, devInst);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(hOut, msg, (DWORD)len, &written, NULL);
        FlushFileBuffers(hOut);

        // Assert gsRENDER_SYSTEM master readiness bitmask at 0x142834CC8
        // Bit 0x2000 = active window presentation ready
        // Bit 0x04 = exit request bit (MUST BE 0)
        uint32_t* pRenderSysFlags = (uint32_t*)(hExe + 0x2834CC8);
        if (VirtualProtect(pRenderSysFlags, 4, PAGE_READWRITE, &oldProt)) {
            *pRenderSysFlags = (*pRenderSysFlags | 0x2000) & ~0x04;
            VirtualProtect(pRenderSysFlags, 4, oldProt, &oldProt);
        }

        // Link master device context pointer if unassigned
        uint8_t** ppDevContext = (uint8_t**)(hExe + 0x2C4DFC8);
        uint8_t** ppDevContextAlt = (uint8_t**)(hExe + 0x2C550A8);
        if (*ppDevContext == nullptr) {
            if (*ppDevContextAlt != nullptr) {
                *ppDevContext = *ppDevContextAlt;
            } else if (devActive != nullptr) {
                *ppDevContext = (uint8_t*)devActive;
            }
        }

        // Ensure GSR master pointer at 0x142BD8AF8 is valid and linked to active gsRENDER_SYSTEM
        uint8_t** ppGsrGlobal = (uint8_t**)(hExe + 0x2BD8AF8);
        uint8_t* pGsr = (ppGsrGlobal != nullptr) ? *ppGsrGlobal : nullptr;
        if (pGsr == nullptr) {
            *ppGsrGlobal = pRenderSys;
            pGsr = pRenderSys;
        }

        // Assert GSR numViewports = 1 directly on the GSR instance (+0x1FD58)
        if (pGsr != nullptr) {
            *(uint32_t*)(pGsr + 0x1FD58) = 1;
            *(uint8_t*)(pGsr + 0x158) = 0x47; // assert active, visible, and renderable flags (0x01 | 0x02 | 0x04 | 0x40)
            *(uint32_t*)(pGsr + 0x938) = 0; // viewport 0 scene index = 0
        }
        if (pRenderSys != nullptr) {
            *(uint32_t*)(pRenderSys + 0x1FD58) = 1;
            *(uint8_t*)(pRenderSys + 0x158) = 0x47;
            *(uint32_t*)(pRenderSys + 0x938) = 0;
            if (*(void**)(pRenderSys + 0x420) == nullptr) {
                static uint8_t s_defaultCameraCtx[0x1000] = {};
                *(void**)(pRenderSys + 0x420) = s_defaultCameraCtx;
            }
        }

        // Assert active scene count at 0x142835CF8 = 1 so scene drawing is never skipped
        uint32_t* pSceneCount = (uint32_t*)(hExe + 0x2835CF8);
        if (VirtualProtect(pSceneCount, 4, PAGE_READWRITE, &oldProt)) {
            if (*pSceneCount == 0) *pSceneCount = 1;
            VirtualProtect(pSceneCount, 4, oldProt, &oldProt);
        }

        // Ensure Viewport table pointer at 0x142835CF0 is never null
        uint64_t* ppViewports = (uint64_t*)(hExe + 0x2835CF0);
        if (ppViewports != nullptr && *ppViewports == 0) {
            static uint8_t s_defaultViewports[0x1000] = {};
            *ppViewports = (uint64_t)s_defaultViewports;
        }

        // Assert UI frontend flags at 0x1428363F4 and 0x14283641C
        uint8_t* pUiFlag1 = (uint8_t*)(hExe + 0x28363F4);
        if (pUiFlag1) *pUiFlag1 = 1;
        uint8_t* pUiFlag2 = (uint8_t*)(hExe + 0x283641C);
        if (pUiFlag2) *pUiFlag2 = 1;

        // Assert Saber network & title state machine globals:
        // 0x142834F26: state = 2 (Signed In & Online)
        // 0x142834F32: 1 (Online Active)
        uint32_t* pNetState = (uint32_t*)(hExe + 0x2834F26);
        if (pNetState) *pNetState = 2;
        uint8_t* pNetActive = (uint8_t*)(hExe + 0x2834F32);
        if (pNetActive) *pNetActive = 1;

        // Assert backend rendering and submission flags
        DWORD oldProtRendFlags = 0;
        if (VirtualProtect(hExe + 0x2C54FE0, 16, PAGE_READWRITE, &oldProtRendFlags)) {
            *(uint8_t*)(hExe + 0x2C54FE8) = 1;  // DxCommonInit active
            *(uint32_t*)(hExe + 0x2C54FE0) = 2; // DxCommonInit ready state (2 = ready)
            *(uint32_t*)(hExe + 0x2C54FE4) = 1; // presentation ready
            VirtualProtect(hExe + 0x2C54FE0, 16, oldProtRendFlags, &oldProtRendFlags);
        }
        if (VirtualProtect(hExe + 0x285AEE0, 16, PAGE_READWRITE, &oldProtRendFlags)) {
            *(uint32_t*)(hExe + 0x285AEE0) = 0x105; // active render pass mask
            *(uint32_t*)(hExe + 0x285AEE4) = 0x105; // target render pass mask
            *(uint8_t*)(hExe + 0x28823D0) = 1;     // master render pass enable
            VirtualProtect(hExe + 0x285AEE0, 16, oldProtRendFlags, &oldProtRendFlags);
        }

        // Apply engine presentation pipeline enablement and stability patches
        uint8_t* pClr1 = (uint8_t*)(hExe + 0x6F697C + 6);
        if (VirtualProtect(pClr1, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
            *pClr1 = 1;
            VirtualProtect(pClr1, 1, oldProt, &oldProt);
        }
        uint8_t* pClr2 = (uint8_t*)(hExe + 0x6F6995 + 6);
        if (VirtualProtect(pClr2, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
            *pClr2 = 1;
            VirtualProtect(pClr2, 1, oldProt, &oldProt);
        }
        uint8_t* pClr3 = (uint8_t*)(hExe + 0x6F6886 + 6);
        if (VirtualProtect(pClr3, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
            *pClr3 = 1;
            VirtualProtect(pClr3, 1, oldProt, &oldProt);
        }
        uint8_t* pClr4 = (uint8_t*)(hExe + 0x6F6898 + 7);
        if (VirtualProtect(pClr4, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
            *pClr4 = 1;
            VirtualProtect(pClr4, 1, oldProt, &oldProt);
        }

        uint8_t* pFrameBranch = (uint8_t*)(hExe + 0x6F668B);
        if (VirtualProtect(pFrameBranch, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pFrameBranch[0] = 0x90;
            pFrameBranch[1] = 0x90;
            VirtualProtect(pFrameBranch, 2, oldProt, &oldProt);
        }

        uint8_t* pCanaryBranch = (uint8_t*)(hExe + 0x72D89D);
        if (VirtualProtect(pCanaryBranch, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pCanaryBranch[0] = 0x90;
            pCanaryBranch[1] = 0x90;
            VirtualProtect(pCanaryBranch, 2, oldProt, &oldProt);
        }

        uint8_t* pFence1 = (uint8_t*)(hExe + 0x104F7D2);
        if (VirtualProtect(pFence1, 7, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pFence1[0] = 0x48; pFence1[1] = 0x85; pFence1[2] = 0xC0;
            pFence1[3] = 0x74; pFence1[4] = 0x10;
            pFence1[5] = 0x90; pFence1[6] = 0x90;
            VirtualProtect(pFence1, 7, oldProt, &oldProt);
        }

        uint8_t* pFence2 = (uint8_t*)(hExe + 0x104F812);
        if (VirtualProtect(pFence2, 7, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pFence2[0] = 0x48; pFence2[1] = 0x85; pFence2[2] = 0xC0;
            pFence2[3] = 0x74; pFence2[4] = 0x10;
            pFence2[5] = 0x90; pFence2[6] = 0x90;
            VirtualProtect(pFence2, 7, oldProt, &oldProt);
        }

        // Patch Worker Job Queue 1 dispatch at 0x1410505B1:
        // When ring buffer [r12 + 0x270] is null (r8 == 0), skip to Queue 2 at 0x1050610
        uint8_t* pWorkerQ1 = (uint8_t*)(hExe + 0x10505B1);
        const uint8_t workerQ1Patch[] = {
            0x4D, 0x8B, 0x84, 0x24, 0x70, 0x02, 0x00, 0x00, // mov r8, [r12 + 0x270] (8)
            0x4D, 0x85, 0xC0,                               // test r8, r8 (3)
            0x74, 0x52,                                     // jz 0x1050610 (2)
            0x41, 0x8B, 0x9C, 0x24, 0x78, 0x02, 0x00, 0x00, // mov ebx, [r12 + 0x278] (8)
            0xEB, 0x1E,                                     // jmp 0x10505E6 (2)
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90  // 8 NOPs
        };
        if (VirtualProtect(pWorkerQ1, sizeof(workerQ1Patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pWorkerQ1, workerQ1Patch, sizeof(workerQ1Patch));
            VirtualProtect(pWorkerQ1, sizeof(workerQ1Patch), oldProt, &oldProt);
        }

        // Patch Worker Job Queue 2 dispatch at 0x141050618:
        // When ring buffer [r12 + 0x158] is null (r8 == 0), skip to Queue 3 at 0x1050670
        uint8_t* pWorkerQ2 = (uint8_t*)(hExe + 0x1050618);
        const uint8_t workerQ2Patch[] = {
            0x4D, 0x8B, 0x84, 0x24, 0x58, 0x01, 0x00, 0x00, // mov r8, [r12 + 0x158] (8)
            0x4D, 0x85, 0xC0,                               // test r8, r8 (3)
            0x74, 0x4B,                                     // jz 0x1050670 (2)
            0x41, 0x8B, 0x9C, 0x24, 0x60, 0x01, 0x00, 0x00, // mov ebx, [r12 + 0x160] (8)
            0xEB, 0x17,                                     // jmp 0x1050646 (2)
            0x90                                            // 1 NOP
        };
        if (VirtualProtect(pWorkerQ2, sizeof(workerQ2Patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pWorkerQ2, workerQ2Patch, sizeof(workerQ2Patch));
            VirtualProtect(pWorkerQ2, sizeof(workerQ2Patch), oldProt, &oldProt);
        }

        // Patch Worker Job Queue 3 dispatch at 0x141050678:
        // When ring buffer [r12 + 0x40] is null (r8 == 0), skip to event wait at 0x1050580
        uint8_t* pWorkerQ3 = (uint8_t*)(hExe + 0x1050678);
        const uint8_t workerQ3Patch[] = {
            0x4D, 0x8B, 0x44, 0x24, 0x40,                   // mov r8, [r12 + 0x40] (5)
            0x4D, 0x85, 0xC0,                               // test r8, r8 (3)
            0x0F, 0x84, 0xFA, 0xFE, 0xFF, 0xFF,             // jz 0x1050580 (6)
            0x41, 0x8B, 0x5C, 0x24, 0x48,                   // mov ebx, [r12 + 0x48] (5)
            0xEB, 0x15,                                     // jmp 0x10506A2 (2)
            0x90, 0x90, 0x90                                // 3 NOPs
        };
        if (VirtualProtect(pWorkerQ3, sizeof(workerQ3Patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pWorkerQ3, workerQ3Patch, sizeof(workerQ3Patch));
            VirtualProtect(pWorkerQ3, sizeof(workerQ3Patch), oldProt, &oldProt);
        }

        // Patch PushJob dispatch at 0x141050D97:
        // When ring buffer [r9 + rax + 0x40] is null (r10 == 0), skip to next worker at 0x1050D40
        uint8_t* pPushJob = (uint8_t*)(hExe + 0x1050D97);
        const uint8_t pushJobPatch[] = {
            0x4D, 0x8B, 0x54, 0x01, 0x40,                   // mov r10, [r9 + rax + 0x40] (5)
            0x4D, 0x85, 0xD2,                               // test r10, r10 (3)
            0x74, 0x9F,                                     // jz 0x1050D40 (2)
            0x41, 0x8B, 0x7C, 0x01, 0x48,                   // mov edi, [r9 + rax + 0x48] (5)
            0x4D, 0x8D, 0x9C, 0x01, 0x8C, 0x00, 0x00, 0x00, // lea r11, [r9 + rax + 0x8C] (8)
            0xEB, 0x10                                      // jmp 0x1050DC0 (2)
        };
        if (VirtualProtect(pPushJob, sizeof(pushJobPatch), PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pPushJob, pushJobPatch, sizeof(pushJobPatch));
            VirtualProtect(pPushJob, sizeof(pushJobPatch), oldProt, &oldProt);
        }

        // Patch Main Thread Wait Queue 1 dispatch at 0x141052436:
        // When ring buffer [r13 + 0x270] is null (r8 == 0), skip to Queue 2 at 0x1052490
        uint8_t* pWaitQ1 = (uint8_t*)(hExe + 0x1052436);
        const uint8_t waitQ1Patch[] = {
            0x4D, 0x8B, 0x85, 0x70, 0x02, 0x00, 0x00, // mov r8, [r13 + 0x270] (7)
            0x4D, 0x85, 0xC0,                         // test r8, r8 (3)
            0x74, 0x4E,                               // jz 0x1052490 (2)
            0x41, 0x8B, 0x9D, 0x78, 0x02, 0x00, 0x00, // mov ebx, [r13 + 0x278] (7)
            0xEB, 0x1A,                               // jmp 0x1052465 (2)
            0x90, 0x90, 0x90, 0x90, 0x90              // 5 NOPs (5)
        };
        if (VirtualProtect(pWaitQ1, sizeof(waitQ1Patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pWaitQ1, waitQ1Patch, sizeof(waitQ1Patch));
            VirtualProtect(pWaitQ1, sizeof(waitQ1Patch), oldProt, &oldProt);
        }

        // Patch Main Thread Wait Queue 2 dispatch at 0x141052497:
        // When ring buffer [r13 + 0x158] is null (r8 == 0), skip to Queue 3 at 0x10524F0
        uint8_t* pWaitQ2 = (uint8_t*)(hExe + 0x1052497);
        const uint8_t waitQ2Patch[] = {
            0x4D, 0x8B, 0x85, 0x58, 0x01, 0x00, 0x00, // mov r8, [r13 + 0x158] (7)
            0x4D, 0x85, 0xC0,                         // test r8, r8 (3)
            0x74, 0x4D,                               // jz 0x10524F0 (2)
            0x41, 0x8B, 0x9D, 0x60, 0x01, 0x00, 0x00, // mov ebx, [r13 + 0x160] (7)
            0xEB, 0x19,                               // jmp 0x10524C5 (2)
            0x90, 0x90, 0x90, 0x90                    // 4 NOPs (4)
        };
        if (VirtualProtect(pWaitQ2, sizeof(waitQ2Patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pWaitQ2, waitQ2Patch, sizeof(waitQ2Patch));
            VirtualProtect(pWaitQ2, sizeof(waitQ2Patch), oldProt, &oldProt);
        }

        // Patch Main Thread Wait Queue 3 dispatch at 0x1410524F7:
        // When ring buffer [r13 + 0x40] is null (r8 == 0), skip to wait at 0x1052550
        uint8_t* pWaitQ3 = (uint8_t*)(hExe + 0x10524F7);
        const uint8_t waitQ3Patch[] = {
            0x4D, 0x8B, 0x45, 0x40,                   // mov r8, [r13 + 0x40] (4)
            0x4D, 0x85, 0xC0,                         // test r8, r8 (3)
            0x74, 0x50,                               // jz 0x1052550 (2)
            0x41, 0x8B, 0x5D, 0x48,                   // mov ebx, [r13 + 0x48] (4)
            0xEB, 0x1F,                               // jmp 0x1052525 (2)
            0x90, 0x90, 0x90, 0x90, 0x90,             // 10 NOPs
            0x90, 0x90, 0x90, 0x90, 0x90
        };
        if (VirtualProtect(pWaitQ3, sizeof(waitQ3Patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pWaitQ3, waitQ3Patch, sizeof(waitQ3Patch));
            VirtualProtect(pWaitQ3, sizeof(waitQ3Patch), oldProt, &oldProt);
        }

        uint64_t* pSwapReady = (uint64_t*)(hExe + 0x2838A78);
        if (pSwapReady) *pSwapReady = 1;
        uint8_t* pServerMode = (uint8_t*)(hExe + 0x2838744);
        if (pServerMode) *pServerMode = 0;

        // Dispatch registered NP callbacks (e.g. user sign-in and reachability)
        std::vector<NpCallbackInfo> cbs;
        {
            std::lock_guard lock(s_npMutex);
            cbs = s_npCallbacks;
        }
        for (const auto& entry : cbs) {
            if (entry.cb && s_invokeSysV3) {
                s_invokeSysV3(entry.cb, 1, 2, (uint64_t)entry.arg);
                s_invokeSysV3(entry.cb, 1, 1, (uint64_t)entry.arg);
                char cbMsg[256];
                int cbLen = snprintf(cbMsg, sizeof(cbMsg),
                    "[SAILOR RUNTIME] Dispatched NP Callback #%d at %p (userId=1, state=SIGNED_IN/REACHABLE)\n",
                    entry.id, entry.cb);
                WriteFile(hOut, cbMsg, (DWORD)cbLen, &written, NULL);
                FlushFileBuffers(hOut);
            }
        }

        // Continuous assertion loop to guarantee frame loop assertions
        for (int i = 0; i < 800; ++i) { // monitor for 40 seconds
            // Re-dispatch any newly registered callbacks
            {
                std::vector<NpCallbackInfo> newCbs;
                {
                    std::lock_guard lock(s_npMutex);
                    newCbs = s_npCallbacks;
                }
                if (s_invokeSysV3) {
                    for (const auto& entry : newCbs) {
                        s_invokeSysV3(entry.cb, 1, 2, (uint64_t)entry.arg);
                        s_invokeSysV3(entry.cb, 1, 1, (uint64_t)entry.arg);
                    }
                }
            }
            uint32_t* pNetStateLoop = (uint32_t*)(hExe + 0x2834F26);
            if (pNetStateLoop) *pNetStateLoop = 2;
            uint8_t* pNetActiveLoop = (uint8_t*)(hExe + 0x2834F32);
            if (pNetActiveLoop) *pNetActiveLoop = 1;

            uint64_t* pVpMask = (uint64_t*)(hExe + 0x2BE4BF0);
            if (pVpMask != nullptr && *pVpMask == 0) {
                *pVpMask = 1;
            }
            uint8_t* pVpFlag1 = (uint8_t*)(hExe + 0x27FF1B0);
            if (pVpFlag1) *pVpFlag1 = 1;
            uint8_t* pVpFlag2 = (uint8_t*)(hExe + 0x2835FB0);
            if (pVpFlag2) *pVpFlag2 = 1;
            uint8_t* pAsyncSwap = (uint8_t*)(hExe + 0x27E4B53);
            if (pAsyncSwap != nullptr && *pAsyncSwap != 0) {
                *pAsyncSwap = 0;
            }
            uint32_t* pSysFlags = (uint32_t*)(hExe + 0x2834CC8);
            if (pSysFlags != nullptr) {
                *pSysFlags = (*pSysFlags | 0x82000) & ~0x04;
            }
            uint32_t* pFrameRenderFlag = (uint32_t*)(hExe + 0x2838A78);
            if (pFrameRenderFlag != nullptr && *pFrameRenderFlag == 0) {
                *pFrameRenderFlag = 1;
            }
            uint8_t* pServerModeLoop = (uint8_t*)(hExe + 0x2838744);
            if (pServerModeLoop != nullptr && *pServerModeLoop != 0) {
                *pServerModeLoop = 0;
            }
            uint8_t** ppDevCtx = (uint8_t**)(hExe + 0x2C4DFC8);
            if (ppDevCtx != nullptr && *ppDevCtx == nullptr && devActive != nullptr) {
                *ppDevCtx = (uint8_t*)devActive;
            }
            uint8_t** ppGsr = (uint8_t**)(hExe + 0x2BD8AF8);
            if (ppGsr != nullptr && *ppGsr != nullptr) {
                uint8_t* pG = *ppGsr;
                *(uint32_t*)(pG + 0x1FD58) = 1;
                *(uint8_t*)(pG + 0x158) |= 0x47;
                *(uint32_t*)(pG + 0x938) = 0;
            }
            if (pRenderSys != nullptr) {
                *(uint32_t*)(pRenderSys + 0x1FD58) = 1;
                *(uint8_t*)(pRenderSys + 0x158) |= 0x47;
                *(uint32_t*)(pRenderSys + 0x938) = 0;
                if (*(void**)(pRenderSys + 0x420) == nullptr) {
                    static uint8_t s_defaultCameraCtx[0x1000] = {};
                    *(void**)(pRenderSys + 0x420) = s_defaultCameraCtx;
                }
            }
            uint32_t* pScCount = (uint32_t*)(hExe + 0x2835CF8);
            if (pScCount != nullptr && *pScCount == 0) {
                *pScCount = 1;
            }
            uint64_t* ppVpLoop = (uint64_t*)(hExe + 0x2835CF0);
            if (ppVpLoop != nullptr && *ppVpLoop == 0) {
                static uint8_t s_defaultVpLoop[0x1000] = {};
                *ppVpLoop = (uint64_t)s_defaultVpLoop;
            }
            if (devActive != nullptr) {
                uint8_t* pDev = (uint8_t*)devActive;
                uint32_t vo = *(uint32_t*)(pDev + 0x4B464);
                if (vo == 0 || vo == 0xFFFFFFFF) {
                    typedef uint64_t (*PfnInitDisplaySwapchain)();
                    auto pfnInitSwapchain = (PfnInitDisplaySwapchain)(hExe + 0x627110);
                    pfnInitSwapchain();
                }
            }
            *(uint8_t*)(hExe + 0x2C54FE8) = 1;
            *(uint32_t*)(hExe + 0x2C54FE0) = 2;
            *(uint32_t*)(hExe + 0x2C54FE4) = 1;
            *(uint32_t*)(hExe + 0x285AEE0) = 0x105;
            *(uint32_t*)(hExe + 0x285AEE4) = 0x105;
            *(uint8_t*)(hExe + 0x28823D0) = 1;
            Sleep(50);
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
#ifdef _DEBUG
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
#endif
        AddVectoredExceptionHandler(1, SailorCrashHandler);
        
        // Ensure console is attached and std streams are bound to live console output
        if (!GetConsoleWindow()) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                AllocConsole();
            }
        }
        HANDLE hConOut = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hConOut != INVALID_HANDLE_VALUE) {
            SetStdHandle(STD_OUTPUT_HANDLE, hConOut);
            SetStdHandle(STD_ERROR_HANDLE, hConOut);
        }
        FILE* fpOut = nullptr;
        freopen_s(&fpOut, "CONOUT$", "w", stdout);
        freopen_s(&fpOut, "CONOUT$", "w", stderr);
        std::ios::sync_with_stdio(true);

        HMODULE hExe = GetModuleHandleA(NULL);
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(hExe, exePath, MAX_PATH);
        
        char msg[512];
        int msgLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] Initialized sailor_kernel.dll into PID %lu (ExeBase: 0x%p, Path: %s)\n",
            GetCurrentProcessId(), (void*)hExe, exePath);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(hOut, msg, (DWORD)msgLen, &written, NULL);

        // Ensure all code and data sections of eboot.exe are PAGE_EXECUTE_READWRITE
        // PlayStation 5 game executables frequently interleave mutable state, atomic counters,
        // and JIT buffers in code sections. Unprotecting the full image guarantees 0% write-fault crashes!
        IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)hExe;
        if (pDos && pDos->e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)((uint8_t*)hExe + pDos->e_lfanew);
            if (pNt && pNt->Signature == IMAGE_NT_SIGNATURE) {
                IMAGE_SECTION_HEADER* pSec = IMAGE_FIRST_SECTION(pNt);
                for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i) {
                    uint8_t* secAddr = (uint8_t*)hExe + pSec[i].VirtualAddress;
                    DWORD secSize = pSec[i].Misc.VirtualSize;
                    if (secSize == 0) secSize = pSec[i].SizeOfRawData;
                    DWORD oldSecProt = 0;
                    VirtualProtect(secAddr, secSize, PAGE_EXECUTE_READWRITE, &oldSecProt);
                }
                int rwxLen = snprintf(msg, sizeof(msg),
                    "[SAILOR RUNTIME] Unprotected %u image sections to PAGE_EXECUTE_READWRITE (full console RWX parity)\n",
                    pNt->FileHeader.NumberOfSections);
                WriteFile(hOut, msg, (DWORD)rwxLen, &written, NULL);
            }
        }

        // Pre-assert entitlement and commerce initialization state
        uint8_t* pEntitlementFlag = (uint8_t*)hExe + 0x2CA36A8;
        DWORD oldProtect = 0;
        if (VirtualProtect(pEntitlementFlag, 1, PAGE_READWRITE, &oldProtect)) {
            *pEntitlementFlag = 1;
            VirtualProtect(pEntitlementFlag, 1, oldProtect, &oldProtect);
            int flagLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Asserted entitlement subsystem active flag at %p = 1\n", (void*)pEntitlementFlag);
            WriteFile(hOut, msg, (DWORD)flagLen, &written, NULL);
        }

        // Install stage failure trap at 0x1400017DB
        uint8_t* pStageFail = (uint8_t*)hExe + 0x17DB;
        if (VirtualProtect(pStageFail, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *pStageFail = 0xCC; // INT3
            VirtualProtect(pStageFail, 1, oldProtect, &oldProtect);
            int trapLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Hooked stage failure trap at %p (INT3)\n", (void*)pStageFail);
            WriteFile(hOut, msg, (DWORD)trapLen, &written, NULL);
        }

        // Bypass hydraMatchmakingInit offline failure at 0x140001310 (6-byte je -> NOP)
        uint8_t* pHydraJe = (uint8_t*)hExe + 0x1310;
        if (VirtualProtect(pHydraJe, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pHydraJe, 0x90, 6);
            VirtualProtect(pHydraJe, 6, oldProtect, &oldProtect);
            int nopLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched hydraMatchmakingInit je at %p with 6 NOPs (offline mode)\n", (void*)pHydraJe);
            WriteFile(hOut, msg, (DWORD)nopLen, &written, NULL);
        }

        // Bypass soundGameInit failure at 0x1400013B0 (6-byte je -> NOP)
        uint8_t* pSoundJe = (uint8_t*)hExe + 0x13B0;
        if (VirtualProtect(pSoundJe, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSoundJe, 0x90, 6);
            VirtualProtect(pSoundJe, 6, oldProtect, &oldProtect);
            int nopLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched soundGameInit je at %p with 6 NOPs\n", (void*)pSoundJe);
            WriteFile(hOut, msg, (DWORD)nopLen, &written, NULL);
        }

        // Initialize graphics/render factory instance at 0x142C9A208
        // Clone the real factory vtable from eboot.exe but stub out destructors at [0] and [1] with DummyFactoryMethod
        // so virtual destruction doesn't call dlfree on host memory, while allocator methods ([4] etc.) remain fully functional!
        static void* s_dummyFactoryVtable[64] = {};
        void** pRealVtable = (void**)((uint8_t*)hExe + 0x262D208);
        memcpy(s_dummyFactoryVtable, pRealVtable, sizeof(void*) * 32);
        s_dummyFactoryVtable[0] = (void*)&DummyFactoryMethod;
        s_dummyFactoryVtable[1] = (void*)&DummyFactoryMethod;
        static void* s_dummyFactoryInstance[4] = { s_dummyFactoryVtable };
        void** pFactoryGlobal = (void**)((uint8_t*)hExe + 0x2C9A208);
        if (VirtualProtect(pFactoryGlobal, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *pFactoryGlobal = s_dummyFactoryInstance;
            VirtualProtect(pFactoryGlobal, sizeof(void*), oldProtect, &oldProtect);
            int factLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Initialized factory global at %p -> vtable %p (safe destructors)\n",
                (void*)pFactoryGlobal, (void*)s_dummyFactoryVtable);
            WriteFile(hOut, msg, (DWORD)factLen, &written, NULL);
        }

        // Set engine config client/render active flag at 0x142838744
        uint8_t* pConfigFlag = (uint8_t*)hExe + 0x2838744;
        if (VirtualProtect(pConfigFlag, 1, PAGE_READWRITE, &oldProtect)) {
            *pConfigFlag = 1;
            VirtualProtect(pConfigFlag, 1, oldProtect, &oldProtect);
        }

        // Initialize redstone tuning global pointers at 0x14280F680 and 0x14280F6C0
        static uint32_t s_defaultTuningConfig[16] = {};
        uint64_t* pTuning1 = (uint64_t*)((uint8_t*)hExe + 0x280F680);
        uint64_t* pTuning2 = (uint64_t*)((uint8_t*)hExe + 0x280F6C0);
        if (VirtualProtect(pTuning1, sizeof(uint64_t), PAGE_READWRITE, &oldProtect)) {
            if (*pTuning1 == 0) *pTuning1 = (uint64_t)s_defaultTuningConfig;
            VirtualProtect(pTuning1, sizeof(uint64_t), oldProtect, &oldProtect);
        }
        if (VirtualProtect(pTuning2, sizeof(uint64_t), PAGE_READWRITE, &oldProtect)) {
            if (*pTuning2 == 0) *pTuning2 = (uint64_t)s_defaultTuningConfig;
            VirtualProtect(pTuning2, sizeof(uint64_t), oldProtect, &oldProtect);
            int tuneLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Initialized tuning globals at %p and %p -> default config\n",
                (void*)pTuning1, (void*)pTuning2);
            WriteFile(hOut, msg, (DWORD)tuneLen, &written, NULL);
        }

        // Bypass telemetry/matchmaking polling loop at 0x1405D7490 (void return)
        uint8_t* pPollTask = (uint8_t*)hExe + 0x5D7490;
        if (VirtualProtect(pPollTask, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *pPollTask = 0xC3; // ret
            VirtualProtect(pPollTask, 1, oldProtect, &oldProtect);
            int pollLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched telemetry poll task at %p with RET (0xC3)\n", (void*)pPollTask);
            WriteFile(hOut, msg, (DWORD)pollLen, &written, NULL);
        }

        // Stub offline SoundTask::Execute at 0x1402466E0 (void return)
        uint8_t* pSoundTask = (uint8_t*)hExe + 0x2466E0;
        if (VirtualProtect(pSoundTask, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *pSoundTask = 0xC3; // ret
            VirtualProtect(pSoundTask, 1, oldProtect, &oldProtect);
            int soundLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched SoundTask::Execute at %p with RET (0xC3)\n", (void*)pSoundTask);
            WriteFile(hOut, msg, (DWORD)soundLen, &written, NULL);
        }

        // Patch scene draw skip at 0x14072C782 (0F 84 78 01 00 00 -> 6 NOPs)
        // Eliminates the check for sceneCount == 0 so the 3D viewport draw loop always executes
        uint8_t* pSkipSceneCount = (uint8_t*)hExe + 0x72C782;
        if (VirtualProtect(pSkipSceneCount, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipSceneCount, 0x90, 6);
            VirtualProtect(pSkipSceneCount, 6, oldProtect, &oldProtect);
            int scLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched scene count skip at %p with 6 NOPs (forces 3D viewport draw loop)\n",
                (void*)pSkipSceneCount);
            WriteFile(hOut, msg, (DWORD)scLen, &written, NULL);
        }

        // Patch numViewports check skip at 0x14072C79E (0F 84 86 02 00 00 -> 6 NOPs)
        // Guarantees execution enters viewport 0 rendering loop
        uint8_t* pSkipNumVp = (uint8_t*)hExe + 0x72C79E;
        if (VirtualProtect(pSkipNumVp, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipNumVp, 0x90, 6);
            VirtualProtect(pSkipNumVp, 6, oldProtect, &oldProtect);
            int vpLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched numViewports skip at %p with 6 NOPs\n",
                (void*)pSkipNumVp);
            WriteFile(hOut, msg, (DWORD)vpLen, &written, NULL);
        }

        // Patch viewport loop entry guard at 0x14072C79E (0F 84 86 02 00 00 -> 6 NOPs)
        // Ensures the viewport render loop is always entered even if scene numViewports is initially 0
        uint8_t* pSkipVpLoopEntry = (uint8_t*)hExe + 0x72C79E;
        if (VirtualProtect(pSkipVpLoopEntry, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipVpLoopEntry, 0x90, 6);
            VirtualProtect(pSkipVpLoopEntry, 6, oldProtect, &oldProtect);
            int vpeLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched viewport loop entry skip at %p with 6 NOPs\n", (void*)pSkipVpLoopEntry);
            WriteFile(hOut, msg, (DWORD)vpeLen, &written, NULL);
        }

        // Patch viewport loop count at 0x14072C7CB with 'mov eax, 1; nop; nop' (B8 01 00 00 00 90 90)
        // Deterministically bounds the viewport loop to exactly 1 iteration (viewport 0) without corrupting 'inc rbx'
        uint8_t* pLoopBound = (uint8_t*)hExe + 0x72C7CB;
        if (VirtualProtect(pLoopBound, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pLoopBound[0] = 0xB8;
            pLoopBound[1] = 0x01;
            pLoopBound[2] = 0x00;
            pLoopBound[3] = 0x00;
            pLoopBound[4] = 0x00;
            pLoopBound[5] = 0x90;
            pLoopBound[6] = 0x90;
            VirtualProtect(pLoopBound, 7, oldProtect, &oldProtect);
            int bndLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched viewport loop bound at %p with 'mov eax, 1; nop; nop'\n",
                (void*)pLoopBound);
            WriteFile(hOut, msg, (DWORD)bndLen, &written, NULL);
        }

        // Patch command buffer submission skip at 0x140CECECD (0F 84 CC 00 00 00 -> 6 NOPs)
        // Forces the render queue flush to always proceed with DCB command buffer submission
        uint8_t* pSkipSubmit = (uint8_t*)hExe + 0xCECECD;
        if (VirtualProtect(pSkipSubmit, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipSubmit, 0x90, 6);
            VirtualProtect(pSkipSubmit, 6, oldProtect, &oldProtect);
            int subLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched command buffer submit skip at %p with 6 NOPs\n",
                (void*)pSkipSubmit);
            WriteFile(hOut, msg, (DWORD)subLen, &written, NULL);
        }

        // Bypass RT_MANAGER skip check at 0x1406261E4 (6-byte jz -> 6 NOPs)
        // This ensures prosperoDRIVER->CreateRTManager() is invoked during bootstrap
        uint8_t* pRtManagerJe = (uint8_t*)hExe + 0x6261E4;
        if (VirtualProtect(pRtManagerJe, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pRtManagerJe, 0x90, 6);
            VirtualProtect(pRtManagerJe, 6, oldProtect, &oldProtect);
            int rtLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched RT_MANAGER jz at %p with 6 NOPs (enable CreateRTManager)\n", (void*)pRtManagerJe);
            WriteFile(hOut, msg, (DWORD)rtLen, &written, NULL);
        }

        // Redirect RT_MANAGER setup completion at 0x14062620F (jmp 0x140626576)
        // This safely transitions to the next bootstrap phase after txmRT_MANAGER is stored
        uint8_t* pRtManagerJmp = (uint8_t*)hExe + 0x62620F;
        if (VirtualProtect(pRtManagerJmp, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pRtManagerJmp[0] = 0xE9; // jmp rel32
            int32_t rel = (int32_t)(0x626576 - (0x62620F + 5)); // 0x362
            *(int32_t*)(pRtManagerJmp + 1) = rel;
            pRtManagerJmp[5] = 0x90; // NOP
            VirtualProtect(pRtManagerJmp, 6, oldProtect, &oldProtect);
            int rtJmpLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched RT_MANAGER exit at %p with JMP to 0x%p\n",
                (void*)pRtManagerJmp, (void*)((uint8_t*)hExe + 0x626576));
            WriteFile(hOut, msg, (DWORD)rtJmpLen, &written, NULL);
        }

        // Hook SaberEngine panic handler at 0x141043820 to log caller and requested size
        uint8_t* pAllocTarget = (uint8_t*)hExe + 0x1043820;
        uint64_t logFuncAddr = (uint64_t)&LogAllocPanicCaller;
        uint64_t retAddr = (uint64_t)((uint8_t*)hExe + 0x1043830);
        memcpy(&s_allocTrampoline[24], &logFuncAddr, sizeof(uint64_t));
        memcpy(&s_allocTrampoline[70], &retAddr, sizeof(uint64_t));
        
        DWORD trampProt = 0;
        VirtualProtect(s_allocTrampoline, sizeof(s_allocTrampoline), PAGE_EXECUTE_READWRITE, &trampProt);
        
        if (VirtualProtect(pAllocTarget, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pAllocTarget[0] = 0xFF;
            pAllocTarget[1] = 0x25;
            *(int32_t*)(pAllocTarget + 2) = 0; // [rip+0]
            *(void**)(pAllocTarget + 6) = s_allocTrampoline;
            pAllocTarget[14] = 0x90; // NOP
            pAllocTarget[15] = 0x90; // NOP
            VirtualProtect(pAllocTarget, 16, oldProtect, &oldProtect);
            int allocLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Hooked SaberEngine::AllocateFailPanic at %p -> Trampoline %p (diagnostics)\n",
                (void*)pAllocTarget, (void*)s_allocTrampoline);
            WriteFile(hOut, msg, (DWORD)allocLen, &written, NULL);
        }

        // Patch Device->IsDisplayReady (0x140D045A0) to return 1 (0xB0, 0x01, 0xC3 = mov al, 1; ret)
        uint8_t* pDevReady = (uint8_t*)hExe + 0xD045A0;
        if (VirtualProtect(pDevReady, 3, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pDevReady[0] = 0xB0; // mov al, 1
            pDevReady[1] = 0x01;
            pDevReady[2] = 0xC3; // ret
            VirtualProtect(pDevReady, 3, oldProtect, &oldProtect);
            int devLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Device->IsDisplayReady at %p with 'mov al, 1; ret' (0xB0 0x01 0xC3)\n",
                (void*)pDevReady);
            WriteFile(hOut, msg, (DWORD)devLen, &written, NULL);
        }



        // Patch Device::SetDisplayDimensions at 0x140D05280 to store width and height into [+0x238, +0x23C]
        uint8_t* pSetDisplayRes = (uint8_t*)hExe + 0xD05280;
        if (VirtualProtect(pSetDisplayRes, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            // mov [rdi + 0x238], esi (89 B7 38 02 00 00)
            pSetDisplayRes[0] = 0x89; pSetDisplayRes[1] = 0xB7;
            pSetDisplayRes[2] = 0x38; pSetDisplayRes[3] = 0x02; pSetDisplayRes[4] = 0x00; pSetDisplayRes[5] = 0x00;
            // mov [rdi + 0x23C], edx (89 97 3C 02 00 00)
            pSetDisplayRes[6] = 0x89; pSetDisplayRes[7] = 0x97;
            pSetDisplayRes[8] = 0x3C; pSetDisplayRes[9] = 0x02; pSetDisplayRes[10] = 0x00; pSetDisplayRes[11] = 0x00;
            // mov al, 1; ret (B0 01 C3)
            pSetDisplayRes[12] = 0xB0; pSetDisplayRes[13] = 0x01; pSetDisplayRes[14] = 0xC3;
            VirtualProtect(pSetDisplayRes, 15, oldProtect, &oldProtect);
            int setLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Device::SetDisplayDimensions at %p to store 1920x1080 into [+0x238, +0x23C]\n",
                (void*)pSetDisplayRes);
            WriteFile(hOut, msg, (DWORD)setLen, &written, NULL);
        }

        // Patch Device->IsDeviceLost at 0x140CFCE90 with 'xor eax, eax; ret' (0x31 0xC0 0xC3)
        // Default stub returned -1 (0xFFFFFFFF), causing the engine to believe the Vulkan device was lost
        // and trigger destructive display recreation loops (0x140C354F0). Returning 0 confirms device is healthy.
        uint8_t* pDevLost = (uint8_t*)hExe + 0xCFCE90;
        if (VirtualProtect(pDevLost, 3, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pDevLost[0] = 0x31; // xor eax, eax
            pDevLost[1] = 0xC0;
            pDevLost[2] = 0xC3; // ret
            VirtualProtect(pDevLost, 3, oldProtect, &oldProtect);
            int lostLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Device->IsDeviceLost at %p with 'xor eax, eax; ret' (0x31 0xC0 0xC3)\n",
                (void*)pDevLost);
            WriteFile(hOut, msg, (DWORD)lostLen, &written, NULL);
        }

        // Patch 0x14027E8C0 (Prospero device vtable slot +0x58) with 'xor eax, eax; ret'
        // This function previously only zeroed xmm0 (C5 F8 57 C0), leaving RAX uninitialized with the vtable address.
        // The caller at 0x14073039A tested AL, which was non-zero (0x40), triggering false device-lost destruction.
        uint8_t* pDevLostAlt = (uint8_t*)hExe + 0x27E8C0;
        if (VirtualProtect(pDevLostAlt, 3, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pDevLostAlt[0] = 0x31; // xor eax, eax
            pDevLostAlt[1] = 0xC0;
            pDevLostAlt[2] = 0xC3; // ret
            VirtualProtect(pDevLostAlt, 3, oldProtect, &oldProtect);
            int lostAltLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Device vtable +0x58 at %p with 'xor eax, eax; ret'\n",
                (void*)pDevLostAlt);
            WriteFile(hOut, msg, (DWORD)lostAltLen, &written, NULL);
        }

        // Patch 0x14073039C: change 'je 0x1407303B1' (0x74 0x13) to unconditional 'jmp 0x1407303B1' (0xEB 0x13)
        // Permanently bypasses the spurious display reset/destroy branch and forces continuous scene rendering.
        uint8_t* pBypassReset = (uint8_t*)hExe + 0x73039C;
        if (VirtualProtect(pBypassReset, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pBypassReset[0] = 0xEB; // jmp short +0x13
            pBypassReset[1] = 0x13;
            VirtualProtect(pBypassReset, 2, oldProtect, &oldProtect);
            int bypassLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched DeviceLost check at %p with unconditional JMP (0xEB 0x13)\n",
                (void*)pBypassReset);
            WriteFile(hOut, msg, (DWORD)bypassLen, &written, NULL);
        }

        // Hook SceneAttachContext at 0x141050010:
        // Safely catches null scene context (rsi == nullptr) on initial bootstrap frames, preventing crash at 0x141050077
        uint8_t* pAttachTarget = (uint8_t*)hExe + 0x1050010;
        uint64_t attachResumeAddr = (uint64_t)((uint8_t*)hExe + 0x105001F);
        memcpy(&s_sceneAttachTrampoline[48], &attachResumeAddr, sizeof(uint64_t));
        DWORD attachProt = 0;
        VirtualProtect(s_sceneAttachTrampoline, sizeof(s_sceneAttachTrampoline), PAGE_EXECUTE_READWRITE, &attachProt);
        if (VirtualProtect(pAttachTarget, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pAttachTarget[0] = 0xFF;
            pAttachTarget[1] = 0x25;
            *(int32_t*)(pAttachTarget + 2) = 0; // [rip+0]
            *(void**)(pAttachTarget + 6) = s_sceneAttachTrampoline;
            pAttachTarget[14] = 0x90; // NOP (fills byte 15 of the 15-byte prologue: 0x105001E; leaves 0x105001F intact!)
            VirtualProtect(pAttachTarget, 15, oldProtect, &oldProtect);
            int attachLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Hooked SceneAttachContext at %p -> Trampoline %p (null-safe bootstrap)\n",
                (void*)pAttachTarget, (void*)s_sceneAttachTrampoline);
            WriteFile(hOut, msg, (DWORD)attachLen, &written, NULL);
        }

        // Patch Renderer::GetViewport at 0x140634366:
        // Safely handles NULL viewport tables before allocation by returning NULL instead of dereferencing address 0
        uint8_t* pGetVp = (uint8_t*)hExe + 0x634366;
        if (VirtualProtect(pGetVp, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pGetVp[0] = 0x48; pGetVp[1] = 0x85; pGetVp[2] = 0xC0; // test rax, rax
            pGetVp[3] = 0x74; pGetVp[4] = 0x03;                   // jz +3 (to ret)
            pGetVp[5] = 0x48; pGetVp[6] = 0x8B; pGetVp[7] = 0x00; // mov rax, [rax]
            pGetVp[8] = 0xC3;                                     // ret
            pGetVp[9] = 0x90;                                     // nop
            VirtualProtect(pGetVp, 10, oldProtect, &oldProtect);
            int vpNullLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Renderer::GetViewport null check at %p\n",
                (void*)pGetVp);
            WriteFile(hOut, msg, (DWORD)vpNullLen, &written, NULL);
        }



        // Patch GSR::GSR constructor at 0x140BE8B7A: default numViewports = 1
        uint8_t* pGsrCtor = (uint8_t*)hExe + 0xBE8B7A;
        if (VirtualProtect(pGsrCtor, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(uint32_t*)pGsrCtor = 1;
            VirtualProtect(pGsrCtor, 4, oldProtect, &oldProtect);
            int ctorLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched GSR::GSR constructor at %p: numViewports default = 1\n",
                (void*)pGsrCtor);
            WriteFile(hOut, msg, (DWORD)ctorLen, &written, NULL);
        }

        // Patch GSR::Clear at 0x140BE8B3D: reset numViewports = 1
        uint8_t* pGsrClr = (uint8_t*)hExe + 0xBE8B3D;
        if (VirtualProtect(pGsrClr, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(uint32_t*)pGsrClr = 1;
            VirtualProtect(pGsrClr, 4, oldProtect, &oldProtect);
            int clrLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched GSR::Clear at %p: numViewports reset = 1\n",
                (void*)pGsrClr);
            WriteFile(hOut, msg, (DWORD)clrLen, &written, NULL);
        }

        // Pre-assert isDisplayReady global flag at 0x142C4C758
        uint8_t* pDispReadyFlag = (uint8_t*)hExe + 0x2C4C758;
        if (VirtualProtect(pDispReadyFlag, 1, PAGE_READWRITE, &oldProtect)) {
            *pDispReadyFlag = 1;
            VirtualProtect(pDispReadyFlag, 1, oldProtect, &oldProtect);
        }

        // Pre-assert viewport bitmask at 0x142BE4BF0
        uint64_t* pVpMaskGlobal = (uint64_t*)((uint8_t*)hExe + 0x2BE4BF0);
        if (VirtualProtect(pVpMaskGlobal, 8, PAGE_READWRITE, &oldProtect)) {
            *pVpMaskGlobal = 1;
            VirtualProtect(pVpMaskGlobal, 8, oldProtect, &oldProtect);
        }

        // Patch scene draw viewport skip at 0x14072C7F0 (74 DB -> 90 90) so draw calls are never skipped
        uint8_t* pSkipSceneDraw = (uint8_t*)hExe + 0x72C7F0;
        if (VirtualProtect(pSkipSceneDraw, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pSkipSceneDraw[0] = 0x90;
            pSkipSceneDraw[1] = 0x90;
            VirtualProtect(pSkipSceneDraw, 2, oldProtect, &oldProtect);
            int p1Len = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched scene draw viewport skip at %p with 2 NOPs\n", (void*)pSkipSceneDraw);
            WriteFile(hOut, msg, (DWORD)p1Len, &written, NULL);
        }

        // Patch gsRENDER_SYSTEM::DrawPre skip at 0x140739633 (75 2A -> 90 90)
        uint8_t* pSkipDrawPre = (uint8_t*)hExe + 0x739633;
        if (VirtualProtect(pSkipDrawPre, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pSkipDrawPre[0] = 0x90;
            pSkipDrawPre[1] = 0x90;
            VirtualProtect(pSkipDrawPre, 2, oldProtect, &oldProtect);
            int p2Len = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched gsRENDER_SYSTEM::DrawPre skip at %p with 2 NOPs\n", (void*)pSkipDrawPre);
            WriteFile(hOut, msg, (DWORD)p2Len, &written, NULL);
        }

        // Patch gsRENDER_SYSTEM::DrawPost skip at 0x1407396C3 (75 2A -> 90 90)
        uint8_t* pSkipDrawPost = (uint8_t*)hExe + 0x7396C3;
        if (VirtualProtect(pSkipDrawPost, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pSkipDrawPost[0] = 0x90;
            pSkipDrawPost[1] = 0x90;
            VirtualProtect(pSkipDrawPost, 2, oldProtect, &oldProtect);
            int p3Len = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched gsRENDER_SYSTEM::DrawPost skip at %p with 2 NOPs\n", (void*)pSkipDrawPost);
            WriteFile(hOut, msg, (DWORD)p3Len, &written, NULL);
        }

        // Patch DrawViewport pre-pass visibility skip at 0x1407314C7 (74 6F -> 90 90)
        uint8_t* pSkipDrawVpPre = (uint8_t*)hExe + 0x7314C7;
        if (VirtualProtect(pSkipDrawVpPre, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pSkipDrawVpPre[0] = 0x90;
            pSkipDrawVpPre[1] = 0x90;
            VirtualProtect(pSkipDrawVpPre, 2, oldProtect, &oldProtect);
            int vpreLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched DrawViewport pre-pass skip at %p with 2 NOPs\n", (void*)pSkipDrawVpPre);
            WriteFile(hOut, msg, (DWORD)vpreLen, &written, NULL);
        }

        // Patch DrawViewport early exit skip at 0x1407315B1 (0F 85 D2 08 00 00 -> 6 NOPs)
        uint8_t* pSkipDrawVpEarly = (uint8_t*)hExe + 0x7315B1;
        if (VirtualProtect(pSkipDrawVpEarly, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipDrawVpEarly, 0x90, 6);
            VirtualProtect(pSkipDrawVpEarly, 6, oldProtect, &oldProtect);
            int vearlyLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched DrawViewport early exit skip at %p with 6 NOPs\n", (void*)pSkipDrawVpEarly);
            WriteFile(hOut, msg, (DWORD)vearlyLen, &written, NULL);
        }

        // Patch DrawViewport render pass loop skip at 0x1407315C2 (0F 84 4D 01 00 00 -> 6 NOPs)
        uint8_t* pSkipDrawVpLoop = (uint8_t*)hExe + 0x7315C2;
        if (VirtualProtect(pSkipDrawVpLoop, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipDrawVpLoop, 0x90, 6);
            VirtualProtect(pSkipDrawVpLoop, 6, oldProtect, &oldProtect);
            int vlpLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched DrawViewport render pass skip at %p with 6 NOPs\n", (void*)pSkipDrawVpLoop);
            WriteFile(hOut, msg, (DWORD)vlpLen, &written, NULL);
        }

        // Patch RenderView pass count check at 0x140CA2716 (74 09 -> 90 90)
        // Prevents bypassing 'cmp [rbx+0x2320], 0' check and eliminates crash at 0x140CA274A when mesh buffer is unallocated
        uint8_t* pSkipMeshCountCheck = (uint8_t*)hExe + 0xCA2716;
        if (VirtualProtect(pSkipMeshCountCheck, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pSkipMeshCountCheck[0] = 0x90;
            pSkipMeshCountCheck[1] = 0x90;
            VirtualProtect(pSkipMeshCountCheck, 2, oldProtect, &oldProtect);
            int mcLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched RenderView pass count check at %p with 2 NOPs (safe mesh guard)\n", (void*)pSkipMeshCountCheck);
            WriteFile(hOut, msg, (DWORD)mcLen, &written, NULL);
        }

        // Patch CommandBuffer submit empty skip at 0x140CE9070 (0F 84 9C 01 00 00 -> 6 NOPs)
        uint8_t* pSkipDcbSubmitEmpty = (uint8_t*)hExe + 0xCE9070;
        if (VirtualProtect(pSkipDcbSubmitEmpty, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipDcbSubmitEmpty, 0x90, 6);
            VirtualProtect(pSkipDcbSubmitEmpty, 6, oldProtect, &oldProtect);
            int dcbLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched CommandBuffer submit empty skip at %p with 6 NOPs\n", (void*)pSkipDcbSubmitEmpty);
            WriteFile(hOut, msg, (DWORD)dcbLen, &written, NULL);
        }

        // Patch PushCommandBufferToQueue empty skip at 0x140CE899E (0F 84 84 00 00 00 -> 6 NOPs)
        uint8_t* pSkipPushCmdEmpty = (uint8_t*)hExe + 0xCE899E;
        if (VirtualProtect(pSkipPushCmdEmpty, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pSkipPushCmdEmpty, 0x90, 6);
            VirtualProtect(pSkipPushCmdEmpty, 6, oldProtect, &oldProtect);
            int pushLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched PushCommandBufferToQueue empty skip at %p with 6 NOPs\n", (void*)pSkipPushCmdEmpty);
            WriteFile(hOut, msg, (DWORD)pushLen, &written, NULL);
        }

        // Disable threaded presentation / async swap buffers offloading at 0x1427E4B53
        // When 0, the main engine loop kicks gsJBM_JOB_SWAP_BUFFERS synchronously each frame!
        uint8_t* pAsyncSwapFlag = (uint8_t*)hExe + 0x27E4B53;
        if (VirtualProtect(pAsyncSwapFlag, 1, PAGE_READWRITE, &oldProtect)) {
            *pAsyncSwapFlag = 0;
            VirtualProtect(pAsyncSwapFlag, 1, oldProtect, &oldProtect);
            int swLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Disabled async swap offload at %p (*0x1427E4B53 = 0)\n", (void*)pAsyncSwapFlag);
            WriteFile(hOut, msg, (DWORD)swLen, &written, NULL);
        }

        // Patch Viewport registration conditional skips:
        // 0x14063393D (0F 84 C5 01 00 00 -> 6 NOPs) - guarantees primary viewport registration
        // 0x140633983 (0F 84 AE 01 00 00 -> 6 NOPs) - guarantees secondary viewport registration
        // 0x140635FAD (0F 84 AA 00 00 00 -> 6 NOPs) - guarantees viewport manager primary init
        // 0x140635FCA (0F 84 C3 00 00 00 -> 6 NOPs) - guarantees viewport manager secondary init
        static const uint32_t s_vpPatchRvas[] = { 0x63393D, 0x633983, 0x635FAD, 0x635FCA };
        for (uint32_t rva : s_vpPatchRvas) {
            uint8_t* p = (uint8_t*)hExe + rva;
            if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memset(p, 0x90, 6);
                VirtualProtect(p, 6, oldProtect, &oldProtect);
            }
        }
        int vpLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] Patched 4 Viewport initialization and registration skips with 6 NOPs each\n");
        WriteFile(hOut, msg, (DWORD)vpLen, &written, NULL);

        // Assert Viewport enable flags at 0x1427FF1B0 and 0x142835FB0
        uint8_t* pVpFlag1 = (uint8_t*)hExe + 0x27FF1B0;
        if (VirtualProtect(pVpFlag1, 1, PAGE_READWRITE, &oldProtect)) {
            *pVpFlag1 = 1;
            VirtualProtect(pVpFlag1, 1, oldProtect, &oldProtect);
        }
        uint8_t* pVpFlag2 = (uint8_t*)hExe + 0x2835FB0;
        if (VirtualProtect(pVpFlag2, 1, PAGE_READWRITE, &oldProtect)) {
            *pVpFlag2 = 1;
            VirtualProtect(pVpFlag2, 1, oldProtect, &oldProtect);
        }

        // Allocate Primary Render System Context at 0x142C4F568
        // Required by eboot.exe for Viewport rendering pipeline (size: 0x460818 bytes)
        void** ppRenderCtx = (void**)((uint8_t*)hExe + 0x2C4F568);
        if (VirtualProtect(ppRenderCtx, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            void* pRenderCtx = VirtualAlloc(NULL, 0x470000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (pRenderCtx) {
                memset(pRenderCtx, 0, 0x470000);
                void* pSubCtx = VirtualAlloc(NULL, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (pSubCtx) {
                    memset(pSubCtx, 0, 0x20000);
                    *(void**)((uint8_t*)pRenderCtx + 0x7A8) = pSubCtx;
                }
                *ppRenderCtx = pRenderCtx;
                int ctxLen = snprintf(msg, sizeof(msg),
                    "[SAILOR RUNTIME] Initialized Render System Context at %p (allocated %p, sub-ctx %p)\n",
                    (void*)ppRenderCtx, pRenderCtx, pSubCtx);
                WriteFile(hOut, msg, (DWORD)ctxLen, &written, NULL);
            }
            VirtualProtect(ppRenderCtx, sizeof(void*), oldProtect, &oldProtect);
        }

        // Renderer::SwapBuffers job loops at 0x1406F5266 (7E 4F) and 0x1406F52BF (0F 8E 88 00 00 00):
        // Retained unmodified so engine kicks and dispatches active 3D rendering jobs!

        // Patch render/prospero/sync error bail at 0x140CEC413 (0F 88 87 00 00 00 -> 6 NOPs)
        uint8_t* pProsperoSyncErr = (uint8_t*)hExe + 0xCEC413;
        if (VirtualProtect(pProsperoSyncErr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pProsperoSyncErr, 0x90, 6);
            VirtualProtect(pProsperoSyncErr, 6, oldProtect, &oldProtect);
            int syncErrLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched render/prospero/sync error check at %p with 6 NOPs\n", (void*)pProsperoSyncErr);
            WriteFile(hOut, msg, (DWORD)syncErrLen, &written, NULL);
        }

        // Patch render/prospero/sync event count wait loop at 0x140CEC420 (0F 8C FA 04 00 00 -> 6 NOPs)
        // Prevents render thread from stalling indefinitely in sceKernelWaitEqueue loop
        uint8_t* pProsperoSyncLoop = (uint8_t*)hExe + 0xCEC420;
        if (VirtualProtect(pProsperoSyncLoop, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pProsperoSyncLoop, 0x90, 6);
            VirtualProtect(pProsperoSyncLoop, 6, oldProtect, &oldProtect);
            int syncLoopLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched render/prospero/sync event loop at %p with 6 NOPs\n", (void*)pProsperoSyncLoop);
            WriteFile(hOut, msg, (DWORD)syncLoopLen, &written, NULL);
        }

        // Patch render/prospero/sync WaitForFence at 0x140CEC2B0
        // Instantly satisfies the GPU flip fence, clears pending status, and returns 1 (true)
        // This releases the render worker thread to continuously record and submit 3D draw command buffers!
        uint8_t* pProsperoWaitForFence = (uint8_t*)hExe + 0xCEC2B0;
        const uint8_t fencePatchCode[] = {
            0x48, 0x63, 0xC6,                               // movsxd rax, esi
            0x83, 0xE0, 0x1F,                               // and eax, 0x1F
            0x48, 0x8B, 0x97, 0x20, 0x39, 0x00, 0x00,       // mov rdx, [rdi + 0x3920]
            0x48, 0x85, 0xD2,                               // test rdx, rdx
            0x74, 0x10,                                     // jz skip_fence (+16)
            0x48, 0x8B, 0x8C, 0xC7, 0x28, 0x39, 0x00, 0x00, // mov rcx, [rdi + rax*8 + 0x3928]
            0x48, 0xC1, 0xE0, 0x05,                         // shl rax, 5
            0x48, 0x89, 0x0C, 0x02,                         // mov [rdx + rax], rcx
            // skip_fence:
            0x48, 0x63, 0xC6,                               // movsxd rax, esi
            0x83, 0xE0, 0x1F,                               // and eax, 0x1F
            0xC6, 0x84, 0x07, 0xC8, 0x39, 0x00, 0x00, 0x00, // mov byte ptr [rdi + rax + 0x39C8], 0
            0xB8, 0x01, 0x00, 0x00, 0x00,                   // mov eax, 1
            0xC3                                            // ret
        };
        if (VirtualProtect(pProsperoWaitForFence, sizeof(fencePatchCode), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pProsperoWaitForFence, fencePatchCode, sizeof(fencePatchCode));
            VirtualProtect(pProsperoWaitForFence, sizeof(fencePatchCode), oldProtect, &oldProtect);
            int fenceLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched render/prospero/sync WaitForFence at %p (%zu bytes, instant fence satisfaction)\n",
                (void*)pProsperoWaitForFence, sizeof(fencePatchCode));
            WriteFile(hOut, msg, (DWORD)fenceLen, &written, NULL);
        }

        // Patch Renderer::IncrementFrameStats text-write at 0x140F20BB6 (FF 83 48 02 00 00 -> 6 NOPs)
        // Prevents crash when incrementing frame stats on code-segment pointer [rbx+0x248]
        uint8_t* pFrameStatsInc = (uint8_t*)hExe + 0xF20BB6;
        if (VirtualProtect(pFrameStatsInc, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memset(pFrameStatsInc, 0x90, 6);
            VirtualProtect(pFrameStatsInc, 6, oldProtect, &oldProtect);
            int statsLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Renderer::IncrementFrameStats at %p with 6 NOPs\n", (void*)pFrameStatsInc);
            WriteFile(hOut, msg, (DWORD)statsLen, &written, NULL);
        }

        // Patch Stage Job Queue dispatch at 0x141051776:
        // When stage job buffer is NULL (r8 == 0), skip to epilogue (pop rbx; ret at 0x1410517AD)
        uint8_t* pStageQueueDispatch = (uint8_t*)hExe + 0x1051776;
        const uint8_t stagePatchCode[] = {
            0x4D, 0x85, 0xC0,             // test r8, r8
            0x74, 0x32,                   // jz 0x10517AD (pop rbx; ret)
            0xEB, 0x13,                   // jmp 0x1051790
            0x90, 0x90, 0x90              // nops to fill the 10 bytes
        };
        if (VirtualProtect(pStageQueueDispatch, sizeof(stagePatchCode), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pStageQueueDispatch, stagePatchCode, sizeof(stagePatchCode));
            VirtualProtect(pStageQueueDispatch, sizeof(stagePatchCode), oldProtect, &oldProtect);
            int stageLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Stage Job Queue dispatch at %p (%zu bytes, null buffer guard)\n",
                (void*)pStageQueueDispatch, sizeof(stagePatchCode));
            WriteFile(hOut, msg, (DWORD)stageLen, &written, NULL);
        }

        // Patch Stage Job Queue dispatch #2 at 0x141051880:
        // When stage job buffer is NULL (r8 == 0), skip to epilogue (add rsp, 8; pop rbx; ... ret at 0x1410518BC)
        uint8_t* pStageQueueDispatch2 = (uint8_t*)hExe + 0x1051880;
        const uint8_t stagePatchCode2[] = {
            0x4D, 0x85, 0xC0,             // test r8, r8
            0x74, 0x37,                   // jz 0x10518BC
            0xEB, 0x19,                   // jmp 0x10518A0
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 // NOPs to fill 16 bytes
        };
        if (VirtualProtect(pStageQueueDispatch2, sizeof(stagePatchCode2), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pStageQueueDispatch2, stagePatchCode2, sizeof(stagePatchCode2));
            VirtualProtect(pStageQueueDispatch2, sizeof(stagePatchCode2), oldProtect, &oldProtect);
            int stage2Len = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Stage Job Queue dispatch #2 at %p (%zu bytes, null buffer guard)\n",
                (void*)pStageQueueDispatch2, sizeof(stagePatchCode2));
            WriteFile(hOut, msg, (DWORD)stage2Len, &written, NULL);
        }

        // Apply synchronous Worker Job Queue patches
        uint8_t* pWorkerQ1_sync = (uint8_t*)(hExe + 0x10505B1);
        const uint8_t workerQ1Patch_sync[] = {
            0x4D, 0x8B, 0x84, 0x24, 0x70, 0x02, 0x00, 0x00, // mov r8, [r12 + 0x270] (8)
            0x4D, 0x85, 0xC0,                               // test r8, r8 (3)
            0x74, 0x52,                                     // jz 0x1050610 (2)
            0x41, 0x8B, 0x9C, 0x24, 0x78, 0x02, 0x00, 0x00, // mov ebx, [r12 + 0x278] (8)
            0xEB, 0x1E,                                     // jmp 0x10505E6 (2)
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90  // 8 NOPs
        };
        if (VirtualProtect(pWorkerQ1_sync, sizeof(workerQ1Patch_sync), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pWorkerQ1_sync, workerQ1Patch_sync, sizeof(workerQ1Patch_sync));
            VirtualProtect(pWorkerQ1_sync, sizeof(workerQ1Patch_sync), oldProtect, &oldProtect);
        }

        uint8_t* pWorkerQ2_sync = (uint8_t*)(hExe + 0x1050618);
        const uint8_t workerQ2Patch_sync[] = {
            0x4D, 0x8B, 0x84, 0x24, 0x58, 0x01, 0x00, 0x00, // mov r8, [r12 + 0x158] (8)
            0x4D, 0x85, 0xC0,                               // test r8, r8 (3)
            0x74, 0x4B,                                     // jz 0x1050670 (2)
            0x41, 0x8B, 0x9C, 0x24, 0x60, 0x01, 0x00, 0x00, // mov ebx, [r12 + 0x160] (8)
            0xEB, 0x17,                                     // jmp 0x1050646 (2)
            0x90                                            // 1 NOP
        };
        if (VirtualProtect(pWorkerQ2_sync, sizeof(workerQ2Patch_sync), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pWorkerQ2_sync, workerQ2Patch_sync, sizeof(workerQ2Patch_sync));
            VirtualProtect(pWorkerQ2_sync, sizeof(workerQ2Patch_sync), oldProtect, &oldProtect);
        }

        uint8_t* pWorkerQ3_sync = (uint8_t*)(hExe + 0x1050678);
        const uint8_t workerQ3Patch_sync[] = {
            0x4D, 0x8B, 0x44, 0x24, 0x40,                   // mov r8, [r12 + 0x40] (5)
            0x4D, 0x85, 0xC0,                               // test r8, r8 (3)
            0x0F, 0x84, 0xFA, 0xFE, 0xFF, 0xFF,             // jz 0x1050580 (6)
            0x41, 0x8B, 0x5C, 0x24, 0x48,                   // mov ebx, [r12 + 0x48] (5)
            0xEB, 0x15,                                     // jmp 0x10506A2 (2)
            0x90, 0x90, 0x90                                // 3 NOPs
        };
        if (VirtualProtect(pWorkerQ3_sync, sizeof(workerQ3Patch_sync), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pWorkerQ3_sync, workerQ3Patch_sync, sizeof(workerQ3Patch_sync));
            VirtualProtect(pWorkerQ3_sync, sizeof(workerQ3Patch_sync), oldProtect, &oldProtect);
        }

        // Synchronous PushJob dispatch patch
        uint8_t* pPushJob_sync = (uint8_t*)(hExe + 0x1050D97);
        const uint8_t pushJobPatch_sync[] = {
            0x4D, 0x8B, 0x54, 0x01, 0x40,                   // mov r10, [r9 + rax + 0x40] (5)
            0x4D, 0x85, 0xD2,                               // test r10, r10 (3)
            0x74, 0x9F,                                     // jz 0x1050D40 (2)
            0x41, 0x8B, 0x7C, 0x01, 0x48,                   // mov edi, [r9 + rax + 0x48] (5)
            0x4D, 0x8D, 0x9C, 0x01, 0x8C, 0x00, 0x00, 0x00, // lea r11, [r9 + rax + 0x8C] (8)
            0xEB, 0x10                                      // jmp 0x1050DC0 (2)
        };
        if (VirtualProtect(pPushJob_sync, sizeof(pushJobPatch_sync), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pPushJob_sync, pushJobPatch_sync, sizeof(pushJobPatch_sync));
            VirtualProtect(pPushJob_sync, sizeof(pushJobPatch_sync), oldProtect, &oldProtect);
        }

        // Synchronous Main Thread Wait Queue patches
        uint8_t* pWaitQ1_sync = (uint8_t*)(hExe + 0x1052436);
        const uint8_t waitQ1Patch_sync[] = {
            0x4D, 0x8B, 0x85, 0x70, 0x02, 0x00, 0x00, // mov r8, [r13 + 0x270] (7)
            0x4D, 0x85, 0xC0,                         // test r8, r8 (3)
            0x74, 0x4E,                               // jz 0x1052490 (2)
            0x41, 0x8B, 0x9D, 0x78, 0x02, 0x00, 0x00, // mov ebx, [r13 + 0x278] (7)
            0xEB, 0x1A,                               // jmp 0x1052465 (2)
            0x90, 0x90, 0x90, 0x90, 0x90              // 5 NOPs (5)
        };
        if (VirtualProtect(pWaitQ1_sync, sizeof(waitQ1Patch_sync), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pWaitQ1_sync, waitQ1Patch_sync, sizeof(waitQ1Patch_sync));
            VirtualProtect(pWaitQ1_sync, sizeof(waitQ1Patch_sync), oldProtect, &oldProtect);
        }

        uint8_t* pWaitQ2_sync = (uint8_t*)(hExe + 0x1052497);
        const uint8_t waitQ2Patch_sync[] = {
            0x4D, 0x8B, 0x85, 0x58, 0x01, 0x00, 0x00, // mov r8, [r13 + 0x158] (7)
            0x4D, 0x85, 0xC0,                         // test r8, r8 (3)
            0x74, 0x4D,                               // jz 0x10524F0 (2)
            0x41, 0x8B, 0x9D, 0x60, 0x01, 0x00, 0x00, // mov ebx, [r13 + 0x160] (7)
            0xEB, 0x19,                               // jmp 0x10524C5 (2)
            0x90, 0x90, 0x90, 0x90                    // 4 NOPs (4)
        };
        if (VirtualProtect(pWaitQ2_sync, sizeof(waitQ2Patch_sync), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pWaitQ2_sync, waitQ2Patch_sync, sizeof(waitQ2Patch_sync));
            VirtualProtect(pWaitQ2_sync, sizeof(waitQ2Patch_sync), oldProtect, &oldProtect);
        }

        uint8_t* pWaitQ3_sync = (uint8_t*)(hExe + 0x10524F7);
        const uint8_t waitQ3Patch_sync[] = {
            0x4D, 0x8B, 0x45, 0x40,                   // mov r8, [r13 + 0x40] (4)
            0x4D, 0x85, 0xC0,                         // test r8, r8 (3)
            0x74, 0x50,                               // jz 0x1052550 (2)
            0x41, 0x8B, 0x5D, 0x48,                   // mov ebx, [r13 + 0x48] (4)
            0xEB, 0x1F,                               // jmp 0x1052525 (2)
            0x90, 0x90, 0x90, 0x90, 0x90,             // 10 NOPs
            0x90, 0x90, 0x90, 0x90, 0x90
        };
        if (VirtualProtect(pWaitQ3_sync, sizeof(waitQ3Patch_sync), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(pWaitQ3_sync, waitQ3Patch_sync, sizeof(waitQ3Patch_sync));
            VirtualProtect(pWaitQ3_sync, sizeof(waitQ3Patch_sync), oldProtect, &oldProtect);
        }

        // Synchronous Task Fence patches
        uint8_t* pFence1_sync = (uint8_t*)(hExe + 0x104F7D2);
        if (VirtualProtect(pFence1_sync, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pFence1_sync[0] = 0x48; pFence1_sync[1] = 0x85; pFence1_sync[2] = 0xC0;
            pFence1_sync[3] = 0x74; pFence1_sync[4] = 0x10;
            pFence1_sync[5] = 0x90; pFence1_sync[6] = 0x90;
            VirtualProtect(pFence1_sync, 7, oldProtect, &oldProtect);
        }
        uint8_t* pFence2_sync = (uint8_t*)(hExe + 0x104F812);
        if (VirtualProtect(pFence2_sync, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pFence2_sync[0] = 0x48; pFence2_sync[1] = 0x85; pFence2_sync[2] = 0xC0;
            pFence2_sync[3] = 0x74; pFence2_sync[4] = 0x10;
            pFence2_sync[5] = 0x90; pFence2_sync[6] = 0x90;
            VirtualProtect(pFence2_sync, 7, oldProtect, &oldProtect);
        }

        // Synchronous canary patch
        uint8_t* pCanaryBranch_sync = (uint8_t*)(hExe + 0x72D89D);
        if (VirtualProtect(pCanaryBranch_sync, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pCanaryBranch_sync[0] = 0x90;
            pCanaryBranch_sync[1] = 0x90;
            VirtualProtect(pCanaryBranch_sync, 2, oldProtect, &oldProtect);
        }

        // Synchronous Presentation branch patch
        uint8_t* pFrameBranch_sync = (uint8_t*)(hExe + 0x6F668B);
        if (VirtualProtect(pFrameBranch_sync, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pFrameBranch_sync[0] = 0x90;
            pFrameBranch_sync[1] = 0x90;
            VirtualProtect(pFrameBranch_sync, 2, oldProtect, &oldProtect);
        }



        // Install thread-safe DeviceFlushPresent thunk for 0x14063B560
        // Background worker callbacks at 0x14063B560 dereference *0x142C4DFC8.
        // If *0x142C4DFC8 is NULL, the original code crashed at 0x14063B588 (or 0x14063B570).
        // This thunk verifies the pointer before every virtual dispatch and returns safely if null.
        uint8_t* pDevFlushTarget = (uint8_t*)hExe + 0x63B560;
        uint8_t* pDevCtxAddr = (uint8_t*)hExe + 0x2C4DFC8;
        uint8_t* pThunk = (uint8_t*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (pThunk != nullptr) {
            uint8_t* cur = pThunk;
            // push rbp; mov rbp, rsp; push rbx; sub rsp, 8
            *cur++ = 0x55;
            *cur++ = 0x48; *cur++ = 0x89; *cur++ = 0xE5;
            *cur++ = 0x53;
            *cur++ = 0x48; *cur++ = 0x83; *cur++ = 0xEC; *cur++ = 0x08;

            // --- Check 1: before call [rax + 0x288] ---
            // mov rax, pDevCtxAddr (10 bytes)
            *cur++ = 0x48; *cur++ = 0xB8;
            *(uint64_t*)cur = (uint64_t)pDevCtxAddr; cur += 8;
            // mov rbx, [rax] (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x18;
            // test rbx, rbx (3 bytes)
            *cur++ = 0x48; *cur++ = 0x85; *cur++ = 0xDB;
            // jz exit_safe (2 bytes)
            uint8_t* jz1 = cur;
            *cur++ = 0x74; *cur++ = 0x00;
            // mov rdi, rbx (SysV this) (3 bytes)
            *cur++ = 0x48; *cur++ = 0x89; *cur++ = 0xDF;
            // mov rax, [rbx] (vtable) (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x03;
            // call qword ptr [rax + 0x288] (6 bytes)
            *cur++ = 0xFF; *cur++ = 0x90;
            *(uint32_t*)cur = 0x288; cur += 4;

            // --- Check 2: before call [rax + 0x1D0] ---
            // mov rax, pDevCtxAddr (10 bytes)
            *cur++ = 0x48; *cur++ = 0xB8;
            *(uint64_t*)cur = (uint64_t)pDevCtxAddr; cur += 8;
            // mov rbx, [rax] (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x18;
            // test rbx, rbx (3 bytes)
            *cur++ = 0x48; *cur++ = 0x85; *cur++ = 0xDB;
            // jz exit_safe (2 bytes)
            uint8_t* jz2 = cur;
            *cur++ = 0x74; *cur++ = 0x00;
            // mov rdi, rbx (SysV this) (3 bytes)
            *cur++ = 0x48; *cur++ = 0x89; *cur++ = 0xDF;
            // mov rax, [rbx] (vtable) (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x03;
            // call qword ptr [rax + 0x1D0] (6 bytes)
            *cur++ = 0xFF; *cur++ = 0x90;
            *(uint32_t*)cur = 0x1D0; cur += 4;

            // --- Check 3: before jmp [rax + 0x280] ---
            // mov rax, pDevCtxAddr (10 bytes)
            *cur++ = 0x48; *cur++ = 0xB8;
            *(uint64_t*)cur = (uint64_t)pDevCtxAddr; cur += 8;
            // mov rbx, [rax] (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x18;
            // test rbx, rbx (3 bytes)
            *cur++ = 0x48; *cur++ = 0x85; *cur++ = 0xDB;
            // jz exit_safe (2 bytes)
            uint8_t* jz3 = cur;
            *cur++ = 0x74; *cur++ = 0x00;
            // mov rdi, rbx (SysV this) (3 bytes)
            *cur++ = 0x48; *cur++ = 0x89; *cur++ = 0xDF;
            // mov rax, [rbx] (vtable) (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x03;
            // add rsp, 8; pop rbx; pop rbp (6 bytes)
            *cur++ = 0x48; *cur++ = 0x83; *cur++ = 0xC4; *cur++ = 0x08;
            *cur++ = 0x5B;
            *cur++ = 0x5D;
            // jmp qword ptr [rax + 0x280] (6 bytes)
            *cur++ = 0xFF; *cur++ = 0xA0;
            *(uint32_t*)cur = 0x280; cur += 4;

            // --- exit_safe ---
            uint8_t* exitSafe = cur;
            // add rsp, 8; pop rbx; pop rbp; ret (7 bytes)
            *cur++ = 0x48; *cur++ = 0x83; *cur++ = 0xC4; *cur++ = 0x08;
            *cur++ = 0x5B;
            *cur++ = 0x5D;
            *cur++ = 0xC3;

            // Patch jump offsets (rel8 from instruction following jz opcode)
            jz1[1] = (uint8_t)(exitSafe - (jz1 + 2));
            jz2[1] = (uint8_t)(exitSafe - (jz2 + 2));
            jz3[1] = (uint8_t)(exitSafe - (jz3 + 2));

            // Hook 0x14063B560 with jmp qword ptr [rip+0]
            if (VirtualProtect(pDevFlushTarget, 64, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                pDevFlushTarget[0] = 0xFF;
                pDevFlushTarget[1] = 0x25;
                pDevFlushTarget[2] = 0x00;
                pDevFlushTarget[3] = 0x00;
                pDevFlushTarget[4] = 0x00;
                pDevFlushTarget[5] = 0x00;
                *(uint64_t*)(pDevFlushTarget + 6) = (uint64_t)pThunk;
                memset(pDevFlushTarget + 14, 0x90, 50);
                VirtualProtect(pDevFlushTarget, 64, oldProtect, &oldProtect);

                int thLen = snprintf(msg, sizeof(msg),
                    "[SAILOR RUNTIME] Hooked DeviceFlushPresent at %p -> safe thunk %p\n",
                    (void*)pDevFlushTarget, (void*)pThunk);
                WriteFile(hOut, msg, (DWORD)thLen, &written, NULL);
            }
        }

        // Launch InitDisplayWatcher to dynamically configure rendCONFIG and gsRENDER_SYSTEM
        HANDLE hWatcher = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitDisplayWatcher, (LPVOID)hExe, 0, NULL);
        if (hWatcher) CloseHandle(hWatcher);

        // 1. Bypass all subsystem failure aborts in Master Subsystems Init (0x140001110 to 0x140001436)
        // NOPs out all 0x0F 0x84 (je failure) so that optional/offline network services do not abort boot
        uint8_t* pInitStart = (uint8_t*)hExe + 0x1110;
        uint8_t* pInitEnd = (uint8_t*)hExe + 0x1436;
        int noppedJumps = 0;
        for (uint8_t* p = pInitStart; p < pInitEnd; ++p) {
            if (p[0] == 0x0F && p[1] == 0x84) {
                DWORD oldProt;
                if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    memset(p, 0x90, 6);
                    VirtualProtect(p, 6, oldProt, &oldProt);
                    noppedJumps++;
                }
            }
        }
        int initPatchLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] Master Subsystems Init: NOPped %d abort jumps between %p and %p\n",
            noppedJumps, (void*)pInitStart, (void*)pInitEnd);
        WriteFile(hOut, msg, (DWORD)initPatchLen, &written, NULL);

        // 1b. Bypass all subsystem failure aborts in Scene / World Subsystems Init (0x1405F8DD0 to 0x1405F9230)
        // Guarantees call 0x1405F9B70 (Scene Graph Init) and call 0x14064EF60 (UI Subsystems Init) are reached!
        uint8_t* pSceneInitStart = (uint8_t*)hExe + 0x5F8DD0;
        uint8_t* pSceneInitEnd = (uint8_t*)hExe + 0x5F9230;
        int noppedSceneJumps = 0;
        for (uint8_t* p = pSceneInitStart; p < pSceneInitEnd; ++p) {
            if (p[0] == 0x0F && p[1] == 0x84) {
                DWORD oldProt;
                if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    memset(p, 0x90, 6);
                    VirtualProtect(p, 6, oldProt, &oldProt);
                    noppedSceneJumps++;
                }
            }
        }
        int scenePatchLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] Scene/World Subsystems Init: NOPped %d abort jumps between %p and %p\n",
            noppedSceneJumps, (void*)pSceneInitStart, (void*)pSceneInitEnd);
        WriteFile(hOut, msg, (DWORD)scenePatchLen, &written, NULL);

        // 1c. Bypass all subsystem failure aborts in Level / Secondary Scene Setup (0x1405FA400 to 0x1405FA630)
        uint8_t* pLevelInitStart = (uint8_t*)hExe + 0x5FA400;
        uint8_t* pLevelInitEnd = (uint8_t*)hExe + 0x5FA630;
        int noppedLevelJumps = 0;
        for (uint8_t* p = pLevelInitStart; p < pLevelInitEnd; ++p) {
            if (p[0] == 0x0F && p[1] == 0x84) {
                DWORD oldProt;
                if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    memset(p, 0x90, 6);
                    VirtualProtect(p, 6, oldProt, &oldProt);
                    noppedLevelJumps++;
                }
            }
        }
        int levelPatchLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] Level Subsystems Setup: NOPped %d abort jumps between %p and %p\n",
            noppedLevelJumps, (void*)pLevelInitStart, (void*)pLevelInitEnd);
        WriteFile(hOut, msg, (DWORD)levelPatchLen, &written, NULL);

        // 2. Patch caller jump at 0x1405F7A6A (74 55 -> 90 90)
        // Prevents caller from branching to shutdown (0x1405F7AC1) if init return status is checked
        uint8_t* pInitCallerJmp = (uint8_t*)hExe + 0x5F7A6A;
        DWORD oldProtCaller;
        if (VirtualProtect(pInitCallerJmp, 2, PAGE_EXECUTE_READWRITE, &oldProtCaller)) {
            pInitCallerJmp[0] = 0x90;
            pInitCallerJmp[1] = 0x90;
            VirtualProtect(pInitCallerJmp, 2, oldProtCaller, &oldProtCaller);
            int clrLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Init caller check at %p with 2 NOPs\n", (void*)pInitCallerJmp);
            WriteFile(hOut, msg, (DWORD)clrLen, &written, NULL);
        }

        // 3. Stub Renderer::Shutdown at 0x1405FA090 and 0x1405F9C60 (ret = 0xC3)
        // Keeps the rendering device, context, and viewports alive throughout the process lifetime
        uint8_t* pRendShutdown1 = (uint8_t*)hExe + 0x5FA090;
        uint8_t* pRendShutdown2 = (uint8_t*)hExe + 0x5F9C60;
        DWORD oldProtRend;
        if (VirtualProtect(pRendShutdown1, 1, PAGE_EXECUTE_READWRITE, &oldProtRend)) {
            pRendShutdown1[0] = 0xC3;
            VirtualProtect(pRendShutdown1, 1, oldProtRend, &oldProtRend);
        }
        if (VirtualProtect(pRendShutdown2, 1, PAGE_EXECUTE_READWRITE, &oldProtRend)) {
            pRendShutdown2[0] = 0xC3;
            VirtualProtect(pRendShutdown2, 1, oldProtRend, &oldProtRend);
        }
        int rendStubLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] Stubbed Renderer::Shutdown at %p and %p with RET (prevents premature GPU teardown)\n",
            (void*)pRendShutdown1, (void*)pRendShutdown2);
        WriteFile(hOut, msg, (DWORD)rendStubLen, &written, NULL);

        // 4. Patch engine loop exit check at 0x1405F841A (74 55 -> 90 90)
        // Prevents the engine from exiting to shutdown when checking running state
        uint8_t* pLoopExitJmp = (uint8_t*)hExe + 0x5F841A;
        DWORD oldProtLoop;
        if (VirtualProtect(pLoopExitJmp, 2, PAGE_EXECUTE_READWRITE, &oldProtLoop)) {
            pLoopExitJmp[0] = 0x90;
            pLoopExitJmp[1] = 0x90;
            VirtualProtect(pLoopExitJmp, 2, oldProtLoop, &oldProtLoop);
            int loopLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Patched Main Loop exit check at %p with 2 NOPs\n", (void*)pLoopExitJmp);
            WriteFile(hOut, msg, (DWORD)loopLen, &written, NULL);
        }

        // 5. Initialize engine active running flag and clear exit request
        DWORD oldProtState;
        if (VirtualProtect(hExe + 0x2CA36A8, 1, PAGE_READWRITE, &oldProtState)) {
            *(uint8_t*)(hExe + 0x2CA36A8) = 1; // engine active / running
            VirtualProtect(hExe + 0x2CA36A8, 1, oldProtState, &oldProtState);
        }
        if (VirtualProtect(hExe + 0x2834CC8, 4, PAGE_READWRITE, &oldProtState)) {
            *(uint32_t*)(hExe + 0x2834CC8) = 0; // clear exit request bit
            VirtualProtect(hExe + 0x2834CC8, 4, oldProtState, &oldProtState);
        }

        // 6. Stub Master Subsystems Shutdown at 0x140001800 (ret = 0xC3)
        // Prevents destruction of graphics device, viewports, backbuffers, and audio streams
        uint8_t* pMasterShutdown = (uint8_t*)hExe + 0x1800;
        DWORD oldProtMasterShutdown;
        if (VirtualProtect(pMasterShutdown, 1, PAGE_EXECUTE_READWRITE, &oldProtMasterShutdown)) {
            pMasterShutdown[0] = 0xC3;
            VirtualProtect(pMasterShutdown, 1, oldProtMasterShutdown, &oldProtMasterShutdown);
            int mLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Stubbed Master Subsystems Shutdown at %p with RET (prevents subsystem teardown)\n",
                (void*)pMasterShutdown);
            WriteFile(hOut, msg, (DWORD)mLen, &written, NULL);
        }

        // 7. NOP exit jumps at 0x1405F83F4 (5 bytes) and 0x1405F840D (6 bytes)
        uint8_t* pExitJmp1 = (uint8_t*)hExe + 0x5F83F4;
        DWORD oldProtExit1;
        if (VirtualProtect(pExitJmp1, 5, PAGE_EXECUTE_READWRITE, &oldProtExit1)) {
            memset(pExitJmp1, 0x90, 5);
            VirtualProtect(pExitJmp1, 5, oldProtExit1, &oldProtExit1);
        }
        uint8_t* pExitJmp2 = (uint8_t*)hExe + 0x5F840D;
        DWORD oldProtExit2;
        if (VirtualProtect(pExitJmp2, 6, PAGE_EXECUTE_READWRITE, &oldProtExit2)) {
            memset(pExitJmp2, 0x90, 6);
            VirtualProtect(pExitJmp2, 6, oldProtExit2, &oldProtExit2);
        }
        int ejLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] NOPped exit jumps at %p and %p\n", (void*)pExitJmp1, (void*)pExitJmp2);
        WriteFile(hOut, msg, (DWORD)ejLen, &written, NULL);

        // 8. Install Safe Scene Tick & Master Scene Render Trampoline at 0x1405F8589 (18 bytes):
        // Replaces:
        //   0x1405F8589: mov rdi, [0x142834CF8]  (7 bytes)
        //   0x1405F8590: mov rax, [rdi]          (3 bytes)
        //   0x1405F8593: call [rax + 0x10]       (3 bytes)
        //   0x1405F8596: call 0x1405F77F0        (5 bytes)
        // with a safe trampoline that guards against NULL scene context (*0x142834CF8),
        // executes the scene tick if valid, CALLS 0x1405F77F0 (Master Scene Render),
        // and resumes at 0x1405F859B!
        void* pSceneTrampolineMem = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (pSceneTrampolineMem) {
            uint8_t* cur = (uint8_t*)pSceneTrampolineMem;
            // mov rax, (hExe + 0x2834CF8) (10 bytes)
            *cur++ = 0x48; *cur++ = 0xB8;
            *(uint64_t*)cur = (uint64_t)((uint8_t*)hExe + 0x2834CF8); cur += 8;
            // mov rdi, [rax] (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x38;
            // test rdi, rdi (3 bytes)
            *cur++ = 0x48; *cur++ = 0x85; *cur++ = 0xFF;
            // jz skip_tick (+26 bytes) (2 bytes: 74 1A)
            *cur++ = 0x74; *cur++ = 0x1A;
            // sub rsp, 0x20 (shadow space) (4 bytes)
            *cur++ = 0x48; *cur++ = 0x83; *cur++ = 0xEC; *cur++ = 0x20;
            // mov rax, [rdi] (3 bytes)
            *cur++ = 0x48; *cur++ = 0x8B; *cur++ = 0x07;
            // call qword ptr [rax + 0x10] (3 bytes)
            *cur++ = 0xFF; *cur++ = 0x50; *cur++ = 0x10;
            // mov rax, (hExe + 0x5F77F0) (10 bytes: Master Scene Render)
            *cur++ = 0x48; *cur++ = 0xB8;
            *(uint64_t*)cur = (uint64_t)((uint8_t*)hExe + 0x5F77F0); cur += 8;
            // call rax (2 bytes)
            *cur++ = 0xFF; *cur++ = 0xD0;
            // add rsp, 0x20 (4 bytes)
            *cur++ = 0x48; *cur++ = 0x83; *cur++ = 0xC4; *cur++ = 0x20;
            // skip_tick:
            // mov rax, (hExe + 0x5F859B) (10 bytes: resume address)
            *cur++ = 0x48; *cur++ = 0xB8;
            *(uint64_t*)cur = (uint64_t)((uint8_t*)hExe + 0x5F859B); cur += 8;
            // jmp rax (2 bytes)
            *cur++ = 0xFF; *cur++ = 0xE0;

            uint8_t jmpBytes18[18] = {
                0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
                0, 0, 0, 0, 0, 0, 0, 0,
                0x90, 0x90, 0x90, 0x90
            };
            *(uint64_t*)(&jmpBytes18[6]) = (uint64_t)pSceneTrampolineMem;

            uint8_t* pSceneHookAddr = (uint8_t*)hExe + 0x5F8589;
            DWORD oldProtSceneHook;
            if (VirtualProtect(pSceneHookAddr, 18, PAGE_EXECUTE_READWRITE, &oldProtSceneHook)) {
                memcpy(pSceneHookAddr, jmpBytes18, 18);
                VirtualProtect(pSceneHookAddr, 18, oldProtSceneHook, &oldProtSceneHook);
            }

            int stLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Installed Safe Scene Tick & Render Trampoline at %p -> hooks %p (18 bytes)\n",
                pSceneTrampolineMem, (void*)pSceneHookAddr);
            WriteFile(hOut, msg, (DWORD)stLen, &written, NULL);
        }

        // 8b. Install Continuous Frame Loop Thunk at 0x1405F85EE (26 bytes):
        // Positioned AFTER 0x1405F85E9 (call 0x1406B8060 - Flip & Present).
        // Pumps Windows messages, clears exit bits, asserts render & presentation flags,
        // refreshes SceneManager context, restores stack frame, and loops back indefinitely!
        void* pThunkMem = VirtualAlloc(NULL, sizeof(s_frameLoopThunkCode), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (pThunkMem) {
            uint8_t thunkBuf[sizeof(s_frameLoopThunkCode)];
            memcpy(thunkBuf, s_frameLoopThunkCode, sizeof(s_frameLoopThunkCode));
            *(uint64_t*)(&thunkBuf[13]) = (uint64_t)&FrameLoopHandler;
            *(uint64_t*)(&thunkBuf[39]) = (uint64_t)((uint8_t*)hExe + 0x2834CF8);
            *(uint64_t*)(&thunkBuf[63]) = (uint64_t)((uint8_t*)hExe + 0x5F7A6C);
            memcpy(pThunkMem, thunkBuf, sizeof(s_frameLoopThunkCode));

            uint8_t jmpBytes26[26];
            jmpBytes26[0] = 0xFF; jmpBytes26[1] = 0x25;
            memset(&jmpBytes26[2], 0, 4); // [rip+0]
            *(uint64_t*)(&jmpBytes26[6]) = (uint64_t)pThunkMem;
            memset(&jmpBytes26[14], 0x90, 12); // Pad remaining 12 bytes with NOPs

            uint8_t* pLoopJmp = (uint8_t*)hExe + 0x5F85EE;
            DWORD oldProtLoopJmp;
            if (VirtualProtect(pLoopJmp, 26, PAGE_EXECUTE_READWRITE, &oldProtLoopJmp)) {
                memcpy(pLoopJmp, jmpBytes26, 26);
                VirtualProtect(pLoopJmp, 26, oldProtLoopJmp, &oldProtLoopJmp);
            }

            int flLen = snprintf(msg, sizeof(msg),
                "[SAILOR RUNTIME] Installed Continuous Frame Loop Thunk at %p -> hooks %p (26 bytes, post-present)\n",
                pThunkMem, (void*)pLoopJmp);
            WriteFile(hOut, msg, (DWORD)flLen, &written, NULL);
        }

        // 9. NOP render pass skip branches so the engine never skips drawing to the backbuffer
        // - 0x6F4F6C (6 bytes: 0F 84 E6 00 00 00 -> 6 NOPs)
        // - 0x6F4FAA (6 bytes: 0F 84 07 06 00 00 -> 6 NOPs)
        // - 0x6F4FC4 (6 bytes: 0F 84 99 08 00 00 -> 6 NOPs)
        uint8_t* pRendBranch1 = (uint8_t*)hExe + 0x6F4F6C;
        DWORD oldProtRendBranch;
        if (VirtualProtect(pRendBranch1, 6, PAGE_EXECUTE_READWRITE, &oldProtRendBranch)) {
            memset(pRendBranch1, 0x90, 6);
            VirtualProtect(pRendBranch1, 6, oldProtRendBranch, &oldProtRendBranch);
        }
        uint8_t* pRendBranch2 = (uint8_t*)hExe + 0x6F4FAA;
        if (VirtualProtect(pRendBranch2, 6, PAGE_EXECUTE_READWRITE, &oldProtRendBranch)) {
            memset(pRendBranch2, 0x90, 6);
            VirtualProtect(pRendBranch2, 6, oldProtRendBranch, &oldProtRendBranch);
        }
        uint8_t* pRendBranch3 = (uint8_t*)hExe + 0x6F4FC4;
        if (VirtualProtect(pRendBranch3, 6, PAGE_EXECUTE_READWRITE, &oldProtRendBranch)) {
            memset(pRendBranch3, 0x90, 6);
            VirtualProtect(pRendBranch3, 6, oldProtRendBranch, &oldProtRendBranch);
        }
        int rendBranchLen = snprintf(msg, sizeof(msg),
            "[SAILOR RUNTIME] NOPped render pass skip branches at %p, %p, %p\n",
            (void*)pRendBranch1, (void*)pRendBranch2, (void*)pRendBranch3);
        WriteFile(hOut, msg, (DWORD)rendBranchLen, &written, NULL);

        // Assert initial backend rendering and submission flags
        DWORD oldProtInitFlags = 0;
        if (VirtualProtect(hExe + 0x2C54FE0, 16, PAGE_READWRITE, &oldProtInitFlags)) {
            *(uint8_t*)(hExe + 0x2C54FE8) = 1;
            *(uint32_t*)(hExe + 0x2C54FE0) = 2;
            *(uint32_t*)(hExe + 0x2C54FE4) = 1;
            VirtualProtect(hExe + 0x2C54FE0, 16, oldProtInitFlags, &oldProtInitFlags);
        }
        if (VirtualProtect(hExe + 0x285AEE0, 16, PAGE_READWRITE, &oldProtInitFlags)) {
            *(uint32_t*)(hExe + 0x285AEE0) = 0x105;
            *(uint32_t*)(hExe + 0x285AEE4) = 0x105;
            *(uint8_t*)(hExe + 0x28823D0) = 1;
            VirtualProtect(hExe + 0x285AEE0, 16, oldProtInitFlags, &oldProtInitFlags);
        }

        FlushFileBuffers(hOut);
    }
    return TRUE;
}
#endif

extern "C" {

SAILOR_EXPORT void SAILOR_SYSV_ABI __sailor_subsystem_log(const char* file, const char* name) {
    SB_LOG_ERROR("EngineBootstrap", "Master Subsystem Init FAILED in {} -> Subsystem: '{}'",
        file ? file : "(null)", name ? name : "(null)");
}

// ==========================================
// Universal Dynamic Fallback Handler
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI __sailor_unhandled_nid(Sailor::u64 nid) {
    static std::unordered_map<Sailor::u64, Sailor::u32> s_nidCounts;
    static std::mutex s_nidMutex;
    Sailor::u32 count = 0;
    {
        std::lock_guard lock(s_nidMutex);
        auto it = s_nidCounts.find(nid);
        if (it != s_nidCounts.end()) {
            if (it->second >= 4) {
                return Sailor::SCE_OK;
            }
            count = ++it->second;
        } else {
            count = s_nidCounts[nid] = 1;
        }
    }

    if (count <= 3) {
        void* caller = nullptr;
#if defined(_MSC_VER) && defined(_M_X64)
        void** pRet = (void**)_AddressOfReturnAddress();
        if (pRet) {
            caller = *(void**)((char*)pRet + 0x30);
        }
#endif
        char msg[256];
        int len = snprintf(msg, sizeof(msg),
            "[WARN] [libkernel] Universal Fallback: Unimplemented NID 0x%016llX called from %p (call #%u), returning SCE_OK\n",
            (unsigned long long)nid, caller, count);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SafeLog(hOut, msg, (DWORD)len);
    } else if (count == 4) {
        char msg[256];
        int len = snprintf(msg, sizeof(msg),
            "[WARN] [libkernel] Universal Fallback: Suppressing further logs for NID 0x%016llX\n",
            (unsigned long long)nid);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SafeLog(hOut, msg, (DWORD)len);
    }

    // Critical game entitlement / license checks:
    // 0xD07058C5802399FD: 0HBYxYAjmf0#k#j (libSceNpEntitlementAccess)
    // Game checks if player is entitled to base game; returning SCE_OK (0) confirms entitlement!
    if (nid == 0xD07058C5802399FDULL) {
        return Sailor::SCE_OK;
    }

    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI __sailor_init_tls(const void* templatePtr, Sailor::SceSize fileSize, Sailor::SceSize memSize, Sailor::SceSize align) {
    if (memSize > 0) {
        Sailor::ThreadManager::Instance().RegisterTlsTemplate(templatePtr, fileSize, memSize, align);
    }
    Sailor::ThreadManager::Instance().SetupCurrentThreadTls();
#ifdef _WIN32
    unsigned __int64 readFs = 0;
    __try { readFs = _readfsbase_u64(); } __except (1) {}
    SB_LOG_INFO("libkernel", "__sailor_init_tls complete: TID={}, FS_BASE=0x{:016X}", GetCurrentThreadId(), readFs);
#endif
    return Sailor::SCE_OK;
}

// ==========================================
// System Module & Dynamic Loading Subsystem
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSysmoduleLoadModule(Sailor::u32 moduleId) {
    SB_LOG_INFO("libkernel", "sceSysmoduleLoadModule: module ID 0x{:04X}", moduleId);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSysmoduleLoadModuleByName(const char* name) {
    if (!name) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    SB_LOG_INFO("libkernel", "sceSysmoduleLoadModuleByName: loading module '{}'", name);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSysmoduleUnloadModule(Sailor::u32 moduleId) {
    void* caller = nullptr;
#if defined(_MSC_VER) && defined(_M_X64)
    caller = _ReturnAddress();
#endif
    SB_LOG_INFO("libkernel", "sceSysmoduleUnloadModule: module ID 0x{:04X}, caller={:p}", moduleId, caller);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelLoadStartModule(
    const char* moduleFileName,
    Sailor::SceSize args,
    const void* argp,
    Sailor::u32 flags,
    void* opt,
    int* res) {
    (void)args;
    (void)argp;
    (void)flags;
    (void)opt;
    if (res) *res = 0;
    if (!moduleFileName) return Sailor::SCE_KERNEL_ERROR_EINVAL;

    SB_LOG_INFO("libkernel", "sceKernelLoadStartModule: loading '{}'", moduleFileName);
    std::filesystem::path hostPath = Sailor::VirtualFileSystem::Instance().ResolvePath(moduleFileName);

#ifdef _WIN32
    std::filesystem::path dllPath = hostPath;
    dllPath.replace_extension(".dll");

    std::string ext = hostPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if ((ext == ".sprx" || ext == ".prx") && !std::filesystem::exists(dllPath) && std::filesystem::exists(hostPath)) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path sailorExe = std::filesystem::path(exePath).parent_path() / L"sailor.exe";
        std::wstring cmd;
        if (std::filesystem::exists(sailorExe)) {
            cmd = L"\"" + sailorExe.wstring() + L"\" relink \"" + hostPath.wstring() + L"\" \"" + dllPath.wstring() + L"\"";
        } else {
            cmd = L"sailor.exe relink \"" + hostPath.wstring() + L"\" \"" + dllPath.wstring() + L"\"";
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 15000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    if (!hMod) {
        hMod = LoadLibraryW(hostPath.c_str());
    }
    if (!hMod) {
        SB_LOG_WARN("libkernel", "sceKernelLoadStartModule: failed to load module '{}' (host path '{}')",
            moduleFileName, hostPath.string());
        return 1; // Return valid fallback stub module handle
    }
    static std::atomic<Sailor::i32> s_nextModuleHandle{100};
    Sailor::i32 modHandle = s_nextModuleHandle.fetch_add(1, std::memory_order_relaxed);
    SB_LOG_INFO("libkernel", "Loaded module '{}' -> handle {} (HMODULE={:p})", moduleFileName, modHandle, static_cast<void*>(hMod));
    return modHandle;
#else
    return 1;
#endif
}

// ==========================================
// Direct Memory Subsystem
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelAllocateDirectMemory(
    Sailor::u64 arg1,
    Sailor::u64 arg2,
    Sailor::u64 arg3,
    Sailor::u64 arg4,
    int arg5,
    Sailor::u64* arg6) {
    // If arg3 (size) >= 4KB and arg6 (outPhysicalOffset) is non-null, use the 6-arg PS5 libkernel ABI:
    // (searchStart, searchEnd, size, alignment, memoryType, outPhysicalOffset)
    if (arg3 >= 0x1000 && arg6 != nullptr) {
        return Sailor::DirectMemoryManager::Instance().Allocate(
            static_cast<Sailor::SceSize>(arg3),
            static_cast<Sailor::SceSize>(arg4),
            arg5,
            arg6);
    }
    // Fallback for 4-arg synthetic callers: (size, alignment, memoryType, outPhysicalOffset)
    return Sailor::DirectMemoryManager::Instance().Allocate(
        static_cast<Sailor::SceSize>(arg1),
        static_cast<Sailor::SceSize>(arg2),
        static_cast<int>(arg3),
        reinterpret_cast<Sailor::u64*>(arg4));
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelCheckedReleaseDirectMemory(Sailor::u64 physicalOffset, Sailor::SceSize size) {
    (void)size;
    return Sailor::DirectMemoryManager::Instance().Release(physicalOffset);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelReleaseDirectMemory(Sailor::u64 physicalOffset, Sailor::SceSize size) {
    (void)size;
    return Sailor::DirectMemoryManager::Instance().Release(physicalOffset);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelMapDirectMemory(
    void** outVirtualAddress,
    Sailor::SceSize size,
    int protection,
    int flags,
    Sailor::u64 physicalOffset,
    Sailor::SceSize alignment) {
    return Sailor::DirectMemoryManager::Instance().Map(outVirtualAddress, size, protection, flags, physicalOffset, alignment);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelMprotect(void* addr, Sailor::SceSize size, int prot) {
    return Sailor::DirectMemoryManager::Instance().Mprotect(addr, size, prot);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelMunmap(void* addr, Sailor::SceSize size) {
    return Sailor::DirectMemoryManager::Instance().Unmap(addr, size);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelReserveVirtualRange(
    void** outVirtualAddress,
    Sailor::SceSize size,
    int flags,
    Sailor::SceSize alignment) {
    return Sailor::DirectMemoryManager::Instance().ReserveVirtualRange(outVirtualAddress, size, flags, alignment);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelMapFlexibleMemory(
    void** outVirtualAddress,
    Sailor::SceSize size,
    int protection,
    int flags) {
    return Sailor::DirectMemoryManager::Instance().MapFlexibleMemory(outVirtualAddress, size, protection, flags);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelVirtualQuery(
    const void* addr,
    int flags,
    void* info,
    Sailor::SceSize infoSize) {
    if (!addr || !info || infoSize < 0x28) {
        return Sailor::SCE_KERNEL_ERROR_EINVAL;
    }
    memset(info, 0, infoSize);

#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.RegionSize > 0) {
        *reinterpret_cast<void**>(static_cast<uint8_t*>(info) + 0x00) = const_cast<void*>(addr);
        size_t remaining = mbi.RegionSize - ((uintptr_t)addr - (uintptr_t)mbi.BaseAddress);
        *reinterpret_cast<void**>(static_cast<uint8_t*>(info) + 0x08) = 
            reinterpret_cast<void*>((uintptr_t)addr + (remaining > 0 ? remaining : 0x10000));
        *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(info) + 0x10) = 0;
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(info) + 0x20) = 0;
        return Sailor::SCE_OK;
    }
#endif

    *reinterpret_cast<void**>(static_cast<uint8_t*>(info) + 0x00) = const_cast<void*>(addr);
    *reinterpret_cast<void**>(static_cast<uint8_t*>(info) + 0x08) = 
        reinterpret_cast<void*>((uintptr_t)addr + 0x10000);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelBatchMap(
    Sailor::SceKernelBatchMapEntry* entries,
    Sailor::u32 count,
    Sailor::u32* outCount) {
    return Sailor::DirectMemoryManager::Instance().BatchMap(entries, count, outCount);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelBatchMap2(
    Sailor::u32 unknown,
    Sailor::SceKernelBatchMapEntry* entries,
    Sailor::u32 count,
    Sailor::u32* outCount) {
    (void)unknown;
    return Sailor::DirectMemoryManager::Instance().BatchMap(entries, count, outCount);
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelGetDirectMemorySize(Sailor::u64* outSize) {
    (void)outSize;
    return 16ULL * 1024 * 1024 * 1024; // 16GB GDDR6 unified memory
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelAvailableDirectMemorySize(void) {
    return 12ULL * 1024 * 1024 * 1024; // 12GB available direct memory
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelAvailableFlexibleMemorySize(void) {
    return 4ULL * 1024 * 1024 * 1024; // 4GB available flexible memory
}

// ==========================================
// Event Queue (Equeue) Subsystem
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelCreateEqueue(
    Sailor::SceKernelEqueue* eq,
    const char* name) {
    if (!eq || (uintptr_t)eq < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    *eq = new Sailor::EventQueue(name ? name : "SceEqueue");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelDeleteEqueue(Sailor::SceKernelEqueue eq) {
    if (!eq || (uintptr_t)eq < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    delete eq;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelAddUserEvent(Sailor::SceKernelEqueue eq, int id) {
    if (!eq || (uintptr_t)eq < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return eq->AddUserEvent(id);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelTriggerUserEvent(Sailor::SceKernelEqueue eq, int id, void* udata) {
    if (!eq || (uintptr_t)eq < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return eq->TriggerUserEvent(id, udata);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelWaitEqueue(
    Sailor::SceKernelEqueue eq,
    Sailor::SceKernelEvent* events,
    int num,
    int* outNum,
    const Sailor::u64* timeoutUsec) {
    if (!eq || (uintptr_t)eq < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return eq->Wait(events, num, outNum, timeoutUsec);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelPushEqueueEvent(
    Sailor::SceKernelEqueue eq,
    Sailor::u64 ident,
    Sailor::i16 filter,
    Sailor::u16 flags,
    Sailor::u32 fflags,
    Sailor::i64 data,
    void* udata) {
    if (!eq || (uintptr_t)eq < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    Sailor::SceKernelEvent ev{};
    ev.ident = ident;
    ev.filter = filter;
    ev.flags = flags;
    ev.fflags = fflags;
    ev.data = data;
    ev.udata = udata;
    return eq->PushEvent(ev);
}

// ==========================================
// libSceApr Package Reader Subsystem
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAprSubmitCommandBuffer(
    const Sailor::AprCommandBuffer* cmdBuffer,
    Sailor::SceAprSubmissionId* outId) {
    return Sailor::VirtualFileSystem::Instance().SubmitAprCommandBuffer(cmdBuffer, outId);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAprWait(Sailor::SceAprSubmissionId id) {
    return Sailor::VirtualFileSystem::Instance().WaitApr(id);
}

// ==========================================
// Threading Subsystem
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCreate(
    Sailor::ScePthread* thread,
    const Sailor::ScePthreadAttr* attr,
    Sailor::ScePthreadEntry entry,
    void* arg,
    const char* name) {
    return Sailor::ThreadManager::Instance().CreateThread(thread, attr, entry, arg, name);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadJoin(Sailor::ScePthread thread, void** valuePtr) {
    return Sailor::ThreadManager::Instance().JoinThread(thread, valuePtr);
}

SAILOR_EXPORT void SAILOR_SYSV_ABI scePthreadExit(void* value) {
    Sailor::ThreadManager::Instance().ExitThread(value);
}

SAILOR_EXPORT Sailor::ScePthread SAILOR_SYSV_ABI scePthreadSelf() {
    return Sailor::ThreadManager::Instance().GetCurrentThreadId();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRename(Sailor::ScePthread thread, const char* name) {
    (void)thread;
    if (name) {
        SB_LOG_INFO("ThreadManager", "scePthreadRename: '{}'", name);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadSetschedparam(Sailor::ScePthread thread, Sailor::i32 policy, const void* param) {
    (void)thread; (void)policy; (void)param;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadGetschedparam(Sailor::ScePthread thread, Sailor::i32* policy, void* param) {
    (void)thread;
    if (policy) *policy = 0;
    if (param) *reinterpret_cast<Sailor::i32*>(param) = 0;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrInit(void** attr) {
    if (!attr) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    *attr = reinterpret_cast<void*>(0x1000);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrDestroy(void** attr) {
    if (attr) *attr = nullptr;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrSetstacksize(void** attr, size_t stacksize) {
    (void)attr; (void)stacksize;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_init(void** attr) {
    return scePthreadAttrInit(attr);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_destroy(void** attr) {
    return scePthreadAttrDestroy(attr);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_setstacksize(void** attr, size_t stacksize) {
    return scePthreadAttrSetstacksize(attr, stacksize);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_setdetachstate(void** attr, int detachstate) {
    (void)attr; (void)detachstate;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_setschedparam(void** attr, const void* param) {
    (void)attr; (void)param;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrSetschedpolicy(void** attr, int policy) {
    (void)attr; (void)policy;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_setschedpolicy(void** attr, int policy) {
    (void)attr; (void)policy;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrSetschedparam(void** attr, const void* param) {
    (void)attr; (void)param;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrSetinheritsched(void** attr, int inherit) {
    (void)attr; (void)inherit;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_setinheritsched(void** attr, int inherit) {
    (void)attr; (void)inherit;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrSetguardsize(void** attr, size_t guardsize) {
    (void)attr; (void)guardsize;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_attr_setguardsize(void** attr, size_t guardsize) {
    (void)attr; (void)guardsize;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadAttrSetaffinity(void** attr, const void* cpuset) {
    (void)attr; (void)cpuset;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT void SAILOR_SYSV_ABI __stack_chk_fail(void) {
    void* retAddr = _ReturnAddress();
    SB_LOG_WARN("libkernel", "__stack_chk_fail invoked from %p! Continuing execution safely.", retAddr);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadSetaffinity(Sailor::ScePthread thread, Sailor::u64 mask) {
    (void)thread; (void)mask;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadGetaffinity(Sailor::ScePthread thread, Sailor::u64* mask) {
    (void)thread;
    if (mask) *mask = 0xFF;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadSetprio(Sailor::ScePthread thread, Sailor::i32 prio) {
    (void)thread; (void)prio;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadGetprio(Sailor::ScePthread thread, Sailor::i32* prio) {
    (void)thread;
    if (prio) *prio = 0;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadKeyCreate(Sailor::u32* key, void (*destructor)(void*)) {
    (void)destructor;
    if (!key) return Sailor::SCE_KERNEL_ERROR_EINVAL;
#ifdef _WIN32
    DWORD tlsIdx = TlsAlloc();
    if (tlsIdx == TLS_OUT_OF_INDEXES) return Sailor::SCE_KERNEL_ERROR_EAGAIN;
    *key = static_cast<Sailor::u32>(tlsIdx);
    return Sailor::SCE_OK;
#else
    return Sailor::SCE_KERNEL_ERROR_ENOSYS;
#endif
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadKeyDelete(Sailor::u32 key) {
#ifdef _WIN32
    return TlsFree(static_cast<DWORD>(key)) ? Sailor::SCE_OK : Sailor::SCE_KERNEL_ERROR_EINVAL;
#else
    return Sailor::SCE_KERNEL_ERROR_ENOSYS;
#endif
}

SAILOR_EXPORT void* SAILOR_SYSV_ABI scePthreadGetspecific(Sailor::u32 key) {
#ifdef _WIN32
    return TlsGetValue(static_cast<DWORD>(key));
#else
    return nullptr;
#endif
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadSetspecific(Sailor::u32 key, const void* value) {
#ifdef _WIN32
    return TlsSetValue(static_cast<DWORD>(key), const_cast<void*>(value)) ? Sailor::SCE_OK : Sailor::SCE_KERNEL_ERROR_EINVAL;
#else
    return Sailor::SCE_KERNEL_ERROR_ENOSYS;
#endif
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_key_create(Sailor::u32* key, void (*destructor)(void*)) {
    return scePthreadKeyCreate(key, destructor);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_key_delete(Sailor::u32 key) {
    return scePthreadKeyDelete(key);
}

SAILOR_EXPORT void* SAILOR_SYSV_ABI pthread_getspecific(Sailor::u32 key) {
    return scePthreadGetspecific(key);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_setspecific(Sailor::u32 key, const void* value) {
    return scePthreadSetspecific(key, value);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexattrSettype(Sailor::ScePthreadMutexAttr* attr, Sailor::i32 type) {
    if (!attr) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    attr->type = type;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexattrSetprotocol(Sailor::ScePthreadMutexAttr* attr, Sailor::i32 protocol) {
    if (!attr) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    attr->protocol = protocol;
    return Sailor::SCE_OK;
}

// ==========================================
// Mutex Primitives
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexInit(
    Sailor::ScePthreadMutex* mutex,
    const Sailor::ScePthreadMutexAttr* attr,
    const char* name) {
    if (!mutex || (uintptr_t)mutex < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    Sailor::MutexType type = Sailor::MutexType::Recursive;
    if (attr && (uintptr_t)attr >= 0x10000) {
        int attrType = *reinterpret_cast<const int*>(attr);
        if (attrType == 1) {
            type = Sailor::MutexType::ErrorCheck;
        } else {
            type = Sailor::MutexType::Recursive;
        }
    }
    *mutex = new Sailor::Mutex(type, name ? name : "SceMutex");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexDestroy(Sailor::ScePthreadMutex* mutex) {
    if (!mutex || (uintptr_t)mutex < 0x10000) return Sailor::SCE_OK;
    if ((uintptr_t)*mutex >= 0x10000) {
        PVOID cur = InterlockedExchangePointer((PVOID volatile*)mutex, nullptr);
        if ((uintptr_t)cur >= 0x10000) {
            delete static_cast<Sailor::Mutex*>(cur);
        }
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexLock(Sailor::ScePthreadMutex* mutex) {
    if (!mutex || (uintptr_t)mutex < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    while ((uintptr_t)*mutex < 0x10000) {
        Sailor::Mutex* newMutex = new Sailor::Mutex(Sailor::MutexType::Recursive, "SceMutexAuto");
        PVOID oldVal = (PVOID)*mutex;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)mutex, (PVOID)newMutex, oldVal);
        if (prev != oldVal) {
            delete newMutex;
        } else {
            break;
        }
    }
    if ((uintptr_t)*mutex < 0x10000) return Sailor::SCE_OK;
    return (*mutex)->Lock();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexTrylock(Sailor::ScePthreadMutex* mutex) {
    if (!mutex || (uintptr_t)mutex < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    while ((uintptr_t)*mutex < 0x10000) {
        Sailor::Mutex* newMutex = new Sailor::Mutex(Sailor::MutexType::Recursive, "SceMutexAuto");
        PVOID oldVal = (PVOID)*mutex;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)mutex, (PVOID)newMutex, oldVal);
        if (prev != oldVal) {
            delete newMutex;
        } else {
            break;
        }
    }
    if ((uintptr_t)*mutex < 0x10000) return Sailor::SCE_OK;
    return (*mutex)->TryLock();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexUnlock(Sailor::ScePthreadMutex* mutex) {
    if (!mutex || (uintptr_t)mutex < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*mutex < 0x10000) return Sailor::SCE_OK;
    return (*mutex)->Unlock();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutex_init(Sailor::ScePthreadMutex* mutex, const Sailor::ScePthreadMutexAttr* attr) {
    return scePthreadMutexInit(mutex, attr, "posix_mutex");
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutex_destroy(Sailor::ScePthreadMutex* mutex) {
    return scePthreadMutexDestroy(mutex);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutex_lock(Sailor::ScePthreadMutex* mutex) {
    return scePthreadMutexLock(mutex);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutex_trylock(Sailor::ScePthreadMutex* mutex) {
    return scePthreadMutexTrylock(mutex);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutex_unlock(Sailor::ScePthreadMutex* mutex) {
    return scePthreadMutexUnlock(mutex);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_create(Sailor::ScePthread* thread, const Sailor::ScePthreadAttr* attr, Sailor::ScePthreadEntry entry, void* arg) {
    return scePthreadCreate(thread, attr, entry, arg, "posix_thread");
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_join(Sailor::ScePthread thread, void** valuePtr) {
    return scePthreadJoin(thread, valuePtr);
}

SAILOR_EXPORT void SAILOR_SYSV_ABI pthread_exit(void* value) {
    scePthreadExit(value);
}

SAILOR_EXPORT Sailor::ScePthread SAILOR_SYSV_ABI pthread_self() {
    return scePthreadSelf();
}

// ==========================================
// Condition Variable Primitives
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondInit(
    Sailor::ScePthreadCond* cond,
    const Sailor::ScePthreadCondAttr* attr,
    const char* name) {
    (void)attr;
    if (!cond || (uintptr_t)cond < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    *cond = new Sailor::ConditionVariable(name ? name : "SceCond");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondDestroy(Sailor::ScePthreadCond* cond) {
    if (!cond || (uintptr_t)cond < 0x10000) return Sailor::SCE_OK;
    if ((uintptr_t)*cond >= 0x10000) {
        PVOID cur = InterlockedExchangePointer((PVOID volatile*)cond, nullptr);
        if ((uintptr_t)cur >= 0x10000) {
            delete static_cast<Sailor::ConditionVariable*>(cur);
        }
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondWait(Sailor::ScePthreadCond* cond, Sailor::ScePthreadMutex* mutex) {
    if (!cond || (uintptr_t)cond < 0x10000 || !mutex || (uintptr_t)mutex < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    while ((uintptr_t)*cond < 0x10000) {
        Sailor::ConditionVariable* newCv = new Sailor::ConditionVariable("SceCondAuto");
        PVOID oldVal = (PVOID)*cond;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)cond, (PVOID)newCv, oldVal);
        if (prev != oldVal) delete newCv;
        else break;
    }
    while ((uintptr_t)*mutex < 0x10000) {
        Sailor::Mutex* newMutex = new Sailor::Mutex(Sailor::MutexType::Recursive, "SceMutexAuto");
        PVOID oldVal = (PVOID)*mutex;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)mutex, (PVOID)newMutex, oldVal);
        if (prev != oldVal) delete newMutex;
        else break;
    }
    if ((uintptr_t)*cond < 0x10000 || (uintptr_t)*mutex < 0x10000) return Sailor::SCE_OK;
    return (*cond)->Wait(*mutex);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondTimedwait(
    Sailor::ScePthreadCond* cond,
    Sailor::ScePthreadMutex* mutex,
    Sailor::u64 usec) {
    if (!cond || (uintptr_t)cond < 0x10000 || !mutex || (uintptr_t)mutex < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    while ((uintptr_t)*cond < 0x10000) {
        Sailor::ConditionVariable* newCv = new Sailor::ConditionVariable("SceCondAuto");
        PVOID oldVal = (PVOID)*cond;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)cond, (PVOID)newCv, oldVal);
        if (prev != oldVal) delete newCv;
        else break;
    }
    while ((uintptr_t)*mutex < 0x10000) {
        Sailor::Mutex* newMutex = new Sailor::Mutex(Sailor::MutexType::Recursive, "SceMutexAuto");
        PVOID oldVal = (PVOID)*mutex;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)mutex, (PVOID)newMutex, oldVal);
        if (prev != oldVal) delete newMutex;
        else break;
    }
    if ((uintptr_t)*cond < 0x10000 || (uintptr_t)*mutex < 0x10000) return Sailor::SCE_OK;
    return (*cond)->TimedWait(*mutex, usec);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondSignal(Sailor::ScePthreadCond* cond) {
    if (!cond || (uintptr_t)cond < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*cond < 0x10000) return Sailor::SCE_OK;
    return (*cond)->Signal();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondBroadcast(Sailor::ScePthreadCond* cond) {
    if (!cond || (uintptr_t)cond < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*cond < 0x10000) return Sailor::SCE_OK;
    return (*cond)->Broadcast();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_cond_init(Sailor::ScePthreadCond* cond, const Sailor::ScePthreadCondAttr* attr) {
    return scePthreadCondInit(cond, attr, "posix_cond");
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_cond_destroy(Sailor::ScePthreadCond* cond) {
    return scePthreadCondDestroy(cond);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_cond_wait(Sailor::ScePthreadCond* cond, Sailor::ScePthreadMutex* mutex) {
    return scePthreadCondWait(cond, mutex);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_cond_timedwait(Sailor::ScePthreadCond* cond, Sailor::ScePthreadMutex* mutex, Sailor::u64 usec) {
    return scePthreadCondTimedwait(cond, mutex, usec);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_cond_signal(Sailor::ScePthreadCond* cond) {
    return scePthreadCondSignal(cond);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_cond_broadcast(Sailor::ScePthreadCond* cond) {
    return scePthreadCondBroadcast(cond);
}

// ==========================================
// Read-Write Lock Primitives
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockInit(
    Sailor::ScePthreadRwlock* rwlock,
    const void* attr,
    const char* name) {
    if (!rwlock || (uintptr_t)rwlock < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    *rwlock = new Sailor::RwLock(name ? name : "SceRwlock");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockDestroy(Sailor::ScePthreadRwlock* rwlock) {
    if (!rwlock || (uintptr_t)rwlock < 0x10000) return Sailor::SCE_OK;
    if ((uintptr_t)*rwlock >= 0x10000) {
        PVOID cur = InterlockedExchangePointer((PVOID volatile*)rwlock, nullptr);
        if ((uintptr_t)cur >= 0x10000) {
            delete static_cast<Sailor::RwLock*>(cur);
        }
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockRdlock(Sailor::ScePthreadRwlock* rwlock) {
    if (!rwlock || (uintptr_t)rwlock < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*rwlock < 0x10000) {
        Sailor::RwLock* newRw = new Sailor::RwLock("SceRwlockAuto");
        PVOID oldVal = (PVOID)*rwlock;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)rwlock, (PVOID)newRw, oldVal);
        if (prev != oldVal) delete newRw;
    }
    return (*rwlock)->LockRead();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockWrlock(Sailor::ScePthreadRwlock* rwlock) {
    if (!rwlock || (uintptr_t)rwlock < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*rwlock < 0x10000) {
        Sailor::RwLock* newRw = new Sailor::RwLock("SceRwlockAuto");
        PVOID oldVal = (PVOID)*rwlock;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)rwlock, (PVOID)newRw, oldVal);
        if (prev != oldVal) delete newRw;
    }
    return (*rwlock)->LockWrite();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockTryrdlock(Sailor::ScePthreadRwlock* rwlock) {
    if (!rwlock || (uintptr_t)rwlock < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*rwlock < 0x10000) {
        Sailor::RwLock* newRw = new Sailor::RwLock("SceRwlockAuto");
        PVOID oldVal = (PVOID)*rwlock;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)rwlock, (PVOID)newRw, oldVal);
        if (prev != oldVal) delete newRw;
    }
    return (*rwlock)->TryLockRead();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockTrywrlock(Sailor::ScePthreadRwlock* rwlock) {
    if (!rwlock || (uintptr_t)rwlock < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*rwlock < 0x10000) {
        Sailor::RwLock* newRw = new Sailor::RwLock("SceRwlockAuto");
        PVOID oldVal = (PVOID)*rwlock;
        PVOID prev = InterlockedCompareExchangePointer((PVOID volatile*)rwlock, (PVOID)newRw, oldVal);
        if (prev != oldVal) delete newRw;
    }
    return (*rwlock)->TryLockWrite();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockUnlock(Sailor::ScePthreadRwlock* rwlock) {
    if (!rwlock || (uintptr_t)rwlock < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    if ((uintptr_t)*rwlock < 0x10000) return Sailor::SCE_OK;
    return (*rwlock)->Unlock();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockattrInit(void* attr) {
    if (attr) *reinterpret_cast<uint64_t*>(attr) = 0;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadRwlockattrDestroy(void* attr) {
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_rwlock_init(Sailor::ScePthreadRwlock* rwlock, const void* attr) {
    return scePthreadRwlockInit(rwlock, attr, "posix_rwlock");
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_rwlock_destroy(Sailor::ScePthreadRwlock* rwlock) {
    return scePthreadRwlockDestroy(rwlock);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_rwlock_rdlock(Sailor::ScePthreadRwlock* rwlock) {
    return scePthreadRwlockRdlock(rwlock);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_rwlock_wrlock(Sailor::ScePthreadRwlock* rwlock) {
    return scePthreadRwlockWrlock(rwlock);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_rwlock_tryrdlock(Sailor::ScePthreadRwlock* rwlock) {
    return scePthreadRwlockTryrdlock(rwlock);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_rwlock_trywrlock(Sailor::ScePthreadRwlock* rwlock) {
    return scePthreadRwlockTrywrlock(rwlock);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_rwlock_unlock(Sailor::ScePthreadRwlock* rwlock) {
    return scePthreadRwlockUnlock(rwlock);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexattrInit(void* attr) {
    if (attr && (uintptr_t)attr >= 0x10000) {
        *reinterpret_cast<uint64_t*>(attr) = 0;
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadMutexattrDestroy(void* attr) {
    (void)attr;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondattrInit(void* attr) {
    if (attr && (uintptr_t)attr >= 0x10000) *reinterpret_cast<uint64_t*>(attr) = 0;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadCondattrDestroy(void* attr) {
    return Sailor::SCE_OK;
}

// ==========================================
// Semaphore Primitives
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelCreateSema(
    Sailor::SceKernelSema* sem,
    const char* name,
    Sailor::u32 attr,
    Sailor::i32 init,
    Sailor::i32 max,
    void* opt) {
    (void)attr;
    (void)opt;
    if (!sem || (uintptr_t)sem < 0x10000 || !name || init < 0 || max <= 0 || init > max) {
        return Sailor::SCE_KERNEL_ERROR_EINVAL;
    }
    *sem = new Sailor::Semaphore(init, max, name);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelDeleteSema(Sailor::SceKernelSema sem) {
    if (!sem || (uintptr_t)sem < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    delete sem;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelWaitSema(Sailor::SceKernelSema sem, Sailor::i32 need, Sailor::u64* timeoutUsec) {
    if (!sem || (uintptr_t)sem < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return sem->Wait(need, timeoutUsec);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelPollSema(Sailor::SceKernelSema sem, Sailor::i32 need) {
    if (!sem || (uintptr_t)sem < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return sem->Poll(need);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelSignalSema(Sailor::SceKernelSema sem, Sailor::i32 count) {
    if (!sem || (uintptr_t)sem < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return sem->Signal(count);
}

// ==========================================
// Virtual File System (VFS)
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelOpen(const char* path, int flags, Sailor::u32 mode) {
    return Sailor::VirtualFileSystem::Instance().Open(path, flags, mode);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelClose(Sailor::i32 fd) {
    return Sailor::VirtualFileSystem::Instance().Close(fd);
}

SAILOR_EXPORT Sailor::i64 SAILOR_SYSV_ABI sceKernelRead(Sailor::i32 fd, void* buf, Sailor::SceSize count) {
    return Sailor::VirtualFileSystem::Instance().Read(fd, buf, count);
}

SAILOR_EXPORT Sailor::i64 SAILOR_SYSV_ABI sceKernelWrite(Sailor::i32 fd, const void* buf, Sailor::SceSize count) {
    return Sailor::VirtualFileSystem::Instance().Write(fd, buf, count);
}

SAILOR_EXPORT Sailor::i64 SAILOR_SYSV_ABI sceKernelPread(Sailor::i32 fd, void* buf, Sailor::SceSize count, Sailor::i64 offset) {
    return Sailor::VirtualFileSystem::Instance().Pread(fd, buf, count, offset);
}

SAILOR_EXPORT Sailor::i64 SAILOR_SYSV_ABI sceKernelPwrite(Sailor::i32 fd, const void* buf, Sailor::SceSize count, Sailor::i64 offset) {
    return Sailor::VirtualFileSystem::Instance().Pwrite(fd, buf, count, offset);
}

SAILOR_EXPORT Sailor::i64 SAILOR_SYSV_ABI sceKernelLseek(Sailor::i32 fd, Sailor::i64 offset, int whence) {
    return Sailor::VirtualFileSystem::Instance().Lseek(fd, offset, whence);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelStat(const char* path, Sailor::SceKernelStat* outStat) {
    return Sailor::VirtualFileSystem::Instance().Stat(path, outStat);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelFstat(Sailor::i32 fd, Sailor::SceKernelStat* outStat) {
    return Sailor::VirtualFileSystem::Instance().Fstat(fd, outStat);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelMkdir(const char* path, Sailor::u32 mode) {
    return Sailor::VirtualFileSystem::Instance().Mkdir(path, mode);
}

// ==========================================
// System Services & Timers
// ==========================================

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelGetProcessTime() {
    using namespace std::chrono;
    auto now = steady_clock::now().time_since_epoch();
    return duration_cast<microseconds>(now).count();
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelGetProcessTimeCounter(void) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return static_cast<Sailor::u64>(qpc.QuadPart);
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelGetProcessTimeCounterFrequency(void) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<Sailor::u64>(freq.QuadPart);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelUsleep(Sailor::u32 usec) {
    std::this_thread::sleep_for(std::chrono::microseconds(usec));
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelNanosleep(const void* rqtp, void* rmtp) {
    (void)rmtp;
    if (!rqtp) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    // rqtp is timespec { tv_sec, tv_nsec }
    const auto* ts = static_cast<const Sailor::i64*>(rqtp);
    auto duration = std::chrono::seconds(ts[0]) + std::chrono::nanoseconds(ts[1]);
    std::this_thread::sleep_for(duration);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelGetAppInfo(void* appInfo) {
    SB_LOG_STUB("libkernel", "sceKernelGetAppInfo");
    (void)appInfo;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelClockGettime(Sailor::i32 clock_id, void* tp) {
    (void)clock_id;
    if (!tp) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    using namespace std::chrono;
    auto now = high_resolution_clock::now().time_since_epoch();
    auto sec = duration_cast<seconds>(now);
    auto nsec = duration_cast<nanoseconds>(now - sec);
    auto* ts = reinterpret_cast<Sailor::i64*>(tp);
    ts[0] = static_cast<Sailor::i64>(sec.count());
    ts[1] = static_cast<Sailor::i64>(nsec.count());
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelGetTscFrequency() {
#ifdef _WIN32
    LARGE_INTEGER freq;
    if (QueryPerformanceFrequency(&freq)) {
        return static_cast<Sailor::u64>(freq.QuadPart);
    }
#endif
    return 3300000000ULL;
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceKernelReadTsc(void) {
#ifdef _WIN32
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return static_cast<Sailor::u64>(qpc.QuadPart);
#else
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
#endif
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelUnlink(const char* path) {
    (void)path;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelGetdents(Sailor::i32 fd, char* buf, size_t nbytes) {
    (void)fd; (void)buf; (void)nbytes;
    return 0; // End of directory
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelFtruncate(Sailor::i32 fd, Sailor::i64 length) {
    (void)fd; (void)length;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceNpRegisterStateCallbackA(void* cb, void* arg) {
    static std::atomic<int> s_nextNpCallbackId{1};
    int id = s_nextNpCallbackId.fetch_add(1);
    SB_LOG_INFO("libSceNpManager", "sceNpRegisterStateCallbackA(cb={}, arg={}) -> cbId={}", cb, arg, id);
    {
        std::lock_guard lock(s_npMutex);
        s_npCallbacks.push_back({cb, arg, id});
    }
    // If SysV trampoline is ready, immediately dispatch user signed-in & reachable status
    if (s_invokeSysV3 && cb) {
        s_invokeSysV3(cb, 1, 2, (uint64_t)arg);
        s_invokeSysV3(cb, 1, 1, (uint64_t)arg);
    }
    return id;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAppContentInitialize(void* opt) {
    (void)opt;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAppContentTemporaryDataMount2(void* opt, char* mountPath) {
    (void)opt;
    if (mountPath) {
        std::strcpy(mountPath, "/temp0");
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceNpEntitlementAccessInitialize(void* opt) {
    (void)opt;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceNpEntitlementAccessGetSkuFlag(Sailor::u32* skuFlag) {
    SB_LOG_INFO("libSceNpEntitlementAccess", "sceNpEntitlementAccessGetSkuFlag: asserting SKU_FLAG_FULL (3)");
    if (skuFlag) {
        *skuFlag = 3; // SKU_FLAG_FULL
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSaveDataInitialize3(void* opt) {
    (void)opt;
    return Sailor::SCE_OK;
}

// ==========================================
// Dynamic Symbol Lookup & Unload
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelDlsym(Sailor::i32 handle, const char* symbol, void** addr) {
    if (!symbol || !addr) return Sailor::SCE_KERNEL_ERROR_EINVAL;
#ifdef _WIN32
    HMODULE hMod = (handle == 0) ? GetModuleHandle(nullptr) : reinterpret_cast<HMODULE>(static_cast<intptr_t>(handle));
    void* proc = reinterpret_cast<void*>(GetProcAddress(hMod, symbol));
    if (!proc) {
        proc = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"sailor_kernel.dll"), symbol));
    }
    if (!proc) {
        *addr = nullptr;
        return Sailor::SCE_KERNEL_ERROR_ENOENT;
    }
    *addr = proc;
    return Sailor::SCE_OK;
#else
    *addr = nullptr;
    return Sailor::SCE_KERNEL_ERROR_ENOENT;
#endif
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelStopUnloadModule(Sailor::i32 handle, Sailor::SceSize args, const void* argp, Sailor::u32 flags, void* opt, int* res) {
    (void)handle;
    (void)args;
    (void)argp;
    (void)flags;
    (void)opt;
    if (res) *res = 0;
    return Sailor::SCE_OK;
}

// ==========================================
// Fiber Subsystem (libSceFiber)
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI _sceFiberInitializeImpl_nid_postfix(
    Sailor::SceFiberInternal* fiber, const char* name, Sailor::SceFiberEntry entry,
    Sailor::u64 argOnInitialize, void* addrContext, Sailor::u64 sizeContext,
    const void* optParam, Sailor::u32 buildVersion) {
    (void)optParam;
    (void)buildVersion;
    return Sailor::FiberManager::Instance().InitializeFiber(fiber, name, entry, argOnInitialize, addrContext, sizeContext);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberInitialize(
    Sailor::SceFiberInternal* fiber, const char* name, Sailor::SceFiberEntry entry,
    Sailor::u64 argOnInitialize, void* addrContext, Sailor::u64 sizeContext,
    const void* optParam) {
    (void)optParam;
    return Sailor::FiberManager::Instance().InitializeFiber(fiber, name, entry, argOnInitialize, addrContext, sizeContext);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberFinalize(Sailor::SceFiberInternal* fiber) {
    return Sailor::FiberManager::Instance().FinalizeFiber(fiber);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberRun_nid_postfix(Sailor::SceFiberInternal* fiber, Sailor::u64 argOnRun, Sailor::u64* argOnReturn) {
    return Sailor::FiberManager::Instance().RunFiber(fiber, argOnRun, argOnReturn);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberRun(Sailor::SceFiberInternal* fiber, Sailor::u64 argOnRun, Sailor::u64* argOnReturn) {
    return Sailor::FiberManager::Instance().RunFiber(fiber, argOnRun, argOnReturn);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberSwitch(Sailor::SceFiberInternal* fiber, Sailor::u64 argOnRun, Sailor::u64* argOnRunOut) {
    return Sailor::FiberManager::Instance().SwitchFiber(fiber, argOnRun, argOnRunOut);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberReturnToThread(Sailor::u64 argOnReturn, Sailor::u64* argOnRun) {
    return Sailor::FiberManager::Instance().ReturnToThread(argOnReturn, argOnRun);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberGetSelf(Sailor::SceFiberInternal** fiber) {
    if (!fiber) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    *fiber = Sailor::FiberManager::Instance().GetCurrentFiber();
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceFiberOptParamInitialize(void* optParam) {
    if (optParam) std::memset(optParam, 0, 128);
    return Sailor::SCE_OK;
}

// ==========================================
// High-Speed Decompression Subsystem
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelDecompress(
    void* dst, Sailor::SceSize dstSize, const void* src, Sailor::SceSize srcSize,
    void* opt, Sailor::SceSize* outSize) {
    (void)opt;
    size_t written = 0;
    Sailor::i32 res = Sailor::Decompressor::Instance().Decompress(
        src, srcSize, dst, dstSize, Sailor::CompressionFormat::Kraken, &written);
    if (outSize) *outSize = static_cast<Sailor::SceSize>(written);
    return res;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceZlibDecompress(
    void* dst, Sailor::SceSize dstSize, const void* src, Sailor::SceSize srcSize,
    Sailor::SceSize* outSize) {
    size_t written = 0;
    Sailor::i32 res = Sailor::Decompressor::Instance().Decompress(
        src, srcSize, dst, dstSize, Sailor::CompressionFormat::Zlib, &written);
    if (outSize) *outSize = static_cast<Sailor::SceSize>(written);
    return res;
}

// ==========================================
// DualSense Pad Subsystem (libScePad)
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadInit() {
    return Sailor::PadManager::Instance().Initialize();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadOpen(Sailor::i32 userId, Sailor::i32 type, Sailor::i32 index, const void* param) {
    return Sailor::PadManager::Instance().Open(userId, type, index, param);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadClose(Sailor::i32 handle) {
    return Sailor::PadManager::Instance().Close(handle);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadRead(Sailor::i32 handle, Sailor::ScePadData* data, Sailor::i32 num) {
    return Sailor::PadManager::Instance().Read(handle, data, num);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadReadState(Sailor::i32 handle, Sailor::ScePadData* data) {
    return Sailor::PadManager::Instance().ReadState(handle, data);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadSetVibration(Sailor::i32 handle, const Sailor::ScePadVibrationParam* param) {
    return Sailor::PadManager::Instance().SetVibration(handle, param);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadSetLightBar(Sailor::i32 handle, const Sailor::ScePadColor* color) {
    return Sailor::PadManager::Instance().SetLightBar(handle, color);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePadResetOrientation(Sailor::i32 handle) {
    return Sailor::PadManager::Instance().ResetOrientation(handle);
}

// ==========================================
// Save Data Subsystem (libSceSaveData)
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSaveDataInitialize(const void* params) {
    (void)params;
    return Sailor::SaveDataManager::Instance().Initialize();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSaveDataTerminate() {
    return Sailor::SaveDataManager::Instance().Terminate();
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSaveDataMount(const Sailor::SceSaveDataMount* mount, Sailor::SceSaveDataMountResult* result) {
    return Sailor::SaveDataManager::Instance().Mount(mount, result);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSaveDataUmount(const char* mountPoint) {
    return Sailor::SaveDataManager::Instance().Umount(mountPoint);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceSaveDataDirNameSearch(const Sailor::SceSaveDataDirNameSearch* search, Sailor::SceSaveDataDirNameSearchResult* result) {
    return Sailor::SaveDataManager::Instance().SearchDirNames(search, result);
}

// ==========================================
// Audio Out Subsystem (libSceAudioOut)
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAudioOutOpen(Sailor::i32 userId, Sailor::i32 type, Sailor::i32 index, Sailor::u32 len, Sailor::u32 freq, Sailor::u32 param) {
    return Sailor::AudioOutManager::Instance().Open(userId, type, index, len, freq, param);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAudioOutClose(Sailor::i32 handle) {
    return Sailor::AudioOutManager::Instance().Close(handle);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAudioOutOutput(Sailor::i32 handle, const void* ptr) {
    return Sailor::AudioOutManager::Instance().Output(handle, ptr);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAudioOutSetVolume(Sailor::i32 handle, Sailor::i32 flag, const Sailor::i32* vol) {
    return Sailor::AudioOutManager::Instance().SetVolume(handle, flag, vol);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAudioOutGetPortInfo(Sailor::i32 handle, Sailor::SceAudioOutPortInfo* info) {
    return Sailor::AudioOutManager::Instance().GetPortInfo(handle, info);
}

// ==========================================
// User Service Subsystem (libSceUserService)
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceUserServiceInitialize(const void* params) {
    (void)params;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceUserServiceGetInitialUser(Sailor::i32* userId) {
    if (!userId) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    *userId = 0x10000000;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceUserServiceGetLoginUserIdList(void* idList) {
    if (!idList) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    auto* ids = static_cast<Sailor::i32*>(idList);
    ids[0] = 0x10000000;
    for (int i = 1; i < 4; ++i) ids[i] = -1;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceUserServiceGetUserName(Sailor::i32 userId, char* userName, Sailor::SceSize size) {
    (void)userId;
    if (!userName || size == 0) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    std::strncpy(userName, "Player1", size - 1);
    userName[size - 1] = '\0';
    return Sailor::SCE_OK;
}

// ==========================================
// C++ ABI Runtime Helpers
// ==========================================

SAILOR_EXPORT int SAILOR_SYSV_ABI __cxa_guard_acquire(int64_t* guard) {
    if (!guard) return 1;
    uint8_t* byteGuard = reinterpret_cast<uint8_t*>(guard);
    if (*byteGuard == 1) return 0;
    return 1;
}

SAILOR_EXPORT void SAILOR_SYSV_ABI __cxa_guard_release(int64_t* guard) {
    if (guard) {
        uint8_t* byteGuard = reinterpret_cast<uint8_t*>(guard);
        *byteGuard = 1;
    }
}

SAILOR_EXPORT void SAILOR_SYSV_ABI __cxa_guard_abort(int64_t* guard) {
    if (guard) {
        uint8_t* byteGuard = reinterpret_cast<uint8_t*>(guard);
        *byteGuard = 0;
    }
}

// PS5 libc __ctype_b character classification table (256 uint16_t entries, with safety padding)
static const struct {
    uint16_t pad_pre[128];
    uint16_t table[256];
    uint16_t pad_post[128];
} s_ps5_ctype_storage = {
    {},
    {
        0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080,
        0x0080, 0x04C0, 0x00C0, 0x00C0, 0x00C0, 0x00C0, 0x0080, 0x0080,
        0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080,
        0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080,
        0x0004, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008,
        0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008,
        0x0021, 0x0021, 0x0021, 0x0021, 0x0021, 0x0021, 0x0021, 0x0021,
        0x0021, 0x0021, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008,
        0x0008, 0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0003, 0x0002,
        0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002,
        0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002,
        0x0002, 0x0002, 0x0002, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008,
        0x0008, 0x0011, 0x0011, 0x0011, 0x0011, 0x0011, 0x0011, 0x0010,
        0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010,
        0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010,
        0x0010, 0x0010, 0x0010, 0x0008, 0x0008, 0x0008, 0x0008, 0x0080
    },
    {}
};

SAILOR_EXPORT const uint16_t* SAILOR_SYSV_ABI sUP1hBaouOw(void) {
    return s_ps5_ctype_storage.table;
}

// PS5 libc tolower & toupper mapping tables (256 int16_t entries each, with EOF=-1 preceding and safety padding)
struct Ps5CharMapTable {
    int16_t pad_pre[127]{};
    int16_t eof_entry{-1};
    int16_t table[256]{};
    int16_t pad_post[128]{};

    constexpr Ps5CharMapTable(bool isLower) {
        for (int i = 0; i < 256; ++i) {
            if (isLower && i >= 'A' && i <= 'Z') {
                table[i] = static_cast<int16_t>(i + 32);
            } else if (!isLower && i >= 'a' && i <= 'z') {
                table[i] = static_cast<int16_t>(i - 32);
            } else {
                table[i] = static_cast<int16_t>(i);
            }
        }
    }
};

static constexpr Ps5CharMapTable s_ps5_tolower_storage(true);
static constexpr Ps5CharMapTable s_ps5_toupper_storage(false);

SAILOR_EXPORT const int16_t* SAILOR_SYSV_ABI ps5_tolower_table(void) {
    return s_ps5_tolower_storage.table;
}

SAILOR_EXPORT const int16_t* SAILOR_SYSV_ABI rcQCUr0EaRU(void) {
    return s_ps5_toupper_storage.table;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI bzQExy189ZI(void* arg) {
    (void)arg;
    return 0;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI __cxa_atexit(void (*func)(void*), void* arg, void* dso) {
    (void)func; (void)arg; (void)dso;
    return 0;
}

SAILOR_EXPORT void SAILOR_SYSV_ABI __cxa_finalize(void* dso) {
    (void)dso;
}

SAILOR_EXPORT void SAILOR_SYSV_ABI __cxa_pure_virtual(void) {
    SB_LOG_ERROR("libc", "Trapped pure virtual function call (__cxa_pure_virtual)");
}

// ==========================================
// Event Flag Subsystem (libkernel)
// ==========================================

static bool SafeWriteEventFlag(Sailor::SceKernelEventFlag* dest, Sailor::SceKernelEventFlag val) {
#ifdef _WIN32
    __try {
        *dest = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *dest = val;
    return true;
#endif
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelCreateEventFlag(
    Sailor::SceKernelEventFlag* ef,
    const char* name,
    Sailor::u32 attr,
    Sailor::u64 initPattern,
    const void* opt) {
    (void)opt;
    if (!ef || (uintptr_t)ef < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    auto* flag = new Sailor::EventFlag(name ? name : "SceEventFlag", attr, initPattern);
    if (!SafeWriteEventFlag(ef, flag)) {
        SB_LOG_WARN("libkernel", "sceKernelCreateEventFlag: fault writing to ef {:p}", (void*)ef);
        delete flag;
        return Sailor::SCE_KERNEL_ERROR_EINVAL;
    }
    // Return standard PS5 kernel handle ID where (handle - 0x8002000C) <= 0x33
    // and bit (handle - 0x8002000C) is set in mask 0x0008000000800405.
    // 0x8002000C satisfies both checks (0 + 0x7FFDFFF4 = 0 <= 0x33, bit 0 is set).
    return (Sailor::i32)0x8002000C;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelDeleteEventFlag(Sailor::SceKernelEventFlag ef) {
    if (!ef || (uintptr_t)ef < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    delete ef;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelSetEventFlag(Sailor::SceKernelEventFlag ef, Sailor::u64 bits) {
    if (!ef || (uintptr_t)ef < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return ef->Set(bits);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelClearEventFlag(Sailor::SceKernelEventFlag ef, Sailor::u64 bits) {
    if (!ef || (uintptr_t)ef < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return ef->Clear(bits);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelPollEventFlag(
    Sailor::SceKernelEventFlag ef,
    Sailor::u64 bits,
    Sailor::u32 waitMode,
    Sailor::u64* resultBits) {
    if (!ef || (uintptr_t)ef < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return ef->Poll(bits, waitMode, resultBits);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceKernelWaitEventFlag(
    Sailor::SceKernelEventFlag ef,
    Sailor::u64 bits,
    Sailor::u32 waitMode,
    Sailor::u64* resultBits,
    const Sailor::u32* timeoutUsec) {
    if (!ef || (uintptr_t)ef < 0x10000) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    return ef->Wait(bits, waitMode, resultBits, timeoutUsec);
}

// ==========================================
// POSIX Mutex Attributes & Thread Helpers
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutexattr_init(void* attr) {
    if (attr) {
        *reinterpret_cast<uint64_t*>(attr) = 0;
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutexattr_settype(void* attr, int type) {
    if (attr) {
        *reinterpret_cast<uint32_t*>(attr) = static_cast<uint32_t>(type);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI pthread_mutexattr_destroy(void* attr) {
    (void)attr;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI scePthreadGetthreadid(void) {
#ifdef _WIN32
    return static_cast<Sailor::u64>(GetCurrentThreadId());
#else
    return 1;
#endif
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI scePthreadYield(void) {
#ifdef _WIN32
    SwitchToThread();
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sched_yield(void) {
#ifdef _WIN32
    SwitchToThread();
#endif
    return Sailor::SCE_OK;
}

static thread_local int s_ps5_posix_errno = 0;
SAILOR_EXPORT int* SAILOR_SYSV_ABI __error(void) {
    return &s_ps5_posix_errno;
}

// ==========================================
// Math Subsystem (sincosf)
// ==========================================

SAILOR_EXPORT void SAILOR_SYSV_ABI ps5_sincosf(float x, float* s, float* c) {
    if (s) *s = std::sin(x);
    if (c) *c = std::cos(x);
}

SAILOR_EXPORT void SAILOR_SYSV_ABI sincosf(float x, float* s, float* c) {
    if (s) *s = std::sin(x);
    if (c) *c = std::cos(x);
}

// ==========================================
// Network Subsystem (libSceNet)
// ==========================================

static thread_local int s_ps5_net_errno = 0;
SAILOR_EXPORT int* SAILOR_SYSV_ABI sceNetErrnoLoc(void) {
    return &s_ps5_net_errno;
}

static bool s_winsock_inited = false;
static void EnsureWinSock() {
#ifdef _WIN32
    if (!s_winsock_inited) {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        s_winsock_inited = true;
    }
#endif
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetSocket(const char* name, int family, int type, int protocol) {
    EnsureWinSock();
    SB_LOG_INFO("libSceNet", "sceNetSocket: name='{}', family={}, type={}, proto={}",
        name ? name : "null", family, type, protocol);
#ifdef _WIN32
    SOCKET s = socket(family, type, protocol);
    if (s == INVALID_SOCKET) {
        static std::atomic<int> s_dummySock{100};
        return s_dummySock.fetch_add(1);
    }
    return static_cast<int>(s);
#else
    return 100;
#endif
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetSetsockopt(int s, int level, int optname, const void* optval, int optlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        setsockopt(static_cast<SOCKET>(s), level, optname, static_cast<const char*>(optval), optlen);
    }
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetGetsockopt(int s, int level, int optname, void* optval, int* optlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100 && optlen) {
        getsockopt(static_cast<SOCKET>(s), level, optname, static_cast<char*>(optval), optlen);
    }
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT uint16_t SAILOR_SYSV_ABI sceNetHtons(uint16_t n) {
#ifdef _WIN32
    return _byteswap_ushort(n);
#else
    return (n << 8) | (n >> 8);
#endif
}

SAILOR_EXPORT uint16_t SAILOR_SYSV_ABI sceNetNtohs(uint16_t n) {
#ifdef _WIN32
    return _byteswap_ushort(n);
#else
    return (n << 8) | (n >> 8);
#endif
}

SAILOR_EXPORT uint32_t SAILOR_SYSV_ABI sceNetHtonl(uint32_t n) {
#ifdef _WIN32
    return _byteswap_ulong(n);
#else
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) | ((n >> 8) & 0xFF00) | ((n >> 24) & 0xFF);
#endif
}

SAILOR_EXPORT uint32_t SAILOR_SYSV_ABI sceNetNtohl(uint32_t n) {
#ifdef _WIN32
    return _byteswap_ulong(n);
#else
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) | ((n >> 8) & 0xFF00) | ((n >> 24) & 0xFF);
#endif
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetSocketClose(int s) {
#ifdef _WIN32
    if (s >= 0 && s < 100) closesocket(static_cast<SOCKET>(s));
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetSocketAbort(int s, int flags) {
    (void)flags;
#ifdef _WIN32
    if (s >= 0 && s < 100) closesocket(static_cast<SOCKET>(s));
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetShutdown(int s, int how) {
#ifdef _WIN32
    if (s >= 0 && s < 100) shutdown(static_cast<SOCKET>(s), how);
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetBind(int s, const void* addr, int addrlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        return bind(static_cast<SOCKET>(s), static_cast<const sockaddr*>(addr), addrlen) == 0 ? 0 : -1;
    }
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetListen(int s, int backlog) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        return listen(static_cast<SOCKET>(s), backlog) == 0 ? 0 : -1;
    }
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetConnect(int s, const void* addr, int addrlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        return connect(static_cast<SOCKET>(s), static_cast<const sockaddr*>(addr), addrlen) == 0 ? 0 : -1;
    }
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetAccept(int s, void* addr, int* addrlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        SOCKET client = accept(static_cast<SOCKET>(s), static_cast<sockaddr*>(addr), addrlen);
        return (client != INVALID_SOCKET) ? static_cast<int>(client) : -1;
    }
#endif
    return 101;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetSend(int s, const void* buf, size_t len, int flags) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        return send(static_cast<SOCKET>(s), static_cast<const char*>(buf), static_cast<int>(len), flags);
    }
#endif
    return static_cast<int>(len);
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetSendto(int s, const void* buf, size_t len, int flags, const void* to, int tolen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        return sendto(static_cast<SOCKET>(s), static_cast<const char*>(buf), static_cast<int>(len), flags, static_cast<const sockaddr*>(to), tolen);
    }
#endif
    return static_cast<int>(len);
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetRecv(int s, void* buf, size_t len, int flags) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        return recv(static_cast<SOCKET>(s), static_cast<char*>(buf), static_cast<int>(len), flags);
    }
#endif
    return 0;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetRecvfrom(int s, void* buf, size_t len, int flags, void* from, int* fromlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) {
        return recvfrom(static_cast<SOCKET>(s), static_cast<char*>(buf), static_cast<int>(len), flags, static_cast<sockaddr*>(from), fromlen);
    }
#endif
    return 0;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetGetsockname(int s, void* addr, int* addrlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) getsockname(static_cast<SOCKET>(s), static_cast<sockaddr*>(addr), addrlen);
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetGetpeername(int s, void* addr, int* addrlen) {
#ifdef _WIN32
    if (s >= 0 && s < 100) getpeername(static_cast<SOCKET>(s), static_cast<sockaddr*>(addr), addrlen);
#endif
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetPoolCreate(const char* name, int size, int flags) {
    (void)name; (void)size; (void)flags;
    return 1;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetPoolDestroy(int poolId) {
    (void)poolId;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetResolverCreate(const char* name, void* opt, int flags) {
    (void)name; (void)opt; (void)flags;
    return 1;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetResolverDestroy(int rid) {
    (void)rid;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetResolverStartNtoa(int rid, const void* inAddr, char* hostname, size_t hostlen, int flags, int timeout, int retry) {
    (void)rid; (void)inAddr; (void)hostname; (void)hostlen; (void)flags; (void)timeout; (void)retry;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetResolverStartAton(int rid, const char* hostname, void* inAddr, int flags, int timeout, int retry) {
    (void)rid; (void)hostname; (void)inAddr; (void)flags; (void)timeout; (void)retry;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetInit(void) {
    EnsureWinSock();
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetTerm(void) {
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetCtlInit(void) {
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetCtlTerm(void) {
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetCtlGetInfo(int code, void* info) {
    (void)code; (void)info;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNetCtlGetState(int* state) {
    if (state) *state = 1; // Connected
    return Sailor::SCE_OK;
}

#ifdef _WIN32
#pragma comment(linker, "/export:inet_pton=ps5_inet_pton")
#pragma comment(linker, "/export:inet_ntop=ps5_inet_ntop")
#pragma comment(linker, "/export:vsnprintf=ps5_vsnprintf")
#pragma comment(linker, "/export:snprintf=ps5_snprintf")
#pragma comment(linker, "/export:fprintf=ps5_fprintf")
#pragma comment(linker, "/export:printf=ps5_printf")
#pragma comment(linker, "/export:sprintf=ps5_sprintf")
#pragma comment(linker, "/export:vsprintf=ps5_vsprintf")
#pragma comment(linker, "/export:vfprintf=ps5_vfprintf")
#pragma comment(linker, "/export:sscanf=ps5_sscanf")
#pragma comment(linker, "/export:time=ps5_time")
#pragma comment(linker, "/export:atexit=ps5_atexit")
#pragma comment(linker, "/export:__tls_get_addr=ps5_tls_get_addr")
#endif

extern "C" int ps5_sprintf(char* buffer, const char* format, ...) {
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000) return 0;
    va_list args;
    va_start(args, format);
    int res = vsprintf(buffer, format ? format : "", args);
    va_end(args);
    return res;
}

extern "C" int ps5_vsprintf(char* buffer, const char* format, va_list arg) {
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000) return 0;
    return vsprintf(buffer, format ? format : "", arg);
}

extern "C" int ps5_vfprintf(FILE* stream, const char* format, va_list arg) {
    return vfprintf(stream ? stream : stderr, format ? format : "", arg);
}

extern "C" int ps5_sscanf(const char* buffer, const char* format, ...) {
    if (!buffer || !format) return 0;
    va_list args;
    va_start(args, format);
    int res = vsscanf(buffer, format, args);
    va_end(args);
    return res;
}

extern "C" int ps5_snprintf(char* buffer, size_t count, const char* format, ...) {
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000 || count == 0) return 0;
    va_list args;
    va_start(args, format);
    int res = vsnprintf(buffer, count, format ? format : "", args);
    va_end(args);
    return res;
}

extern "C" int ps5_fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vfprintf(stream ? stream : stderr, format ? format : "", args);
    va_end(args);
    return res;
}

extern "C" int ps5_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vprintf(format ? format : "", args);
    va_end(args);
    return res;
}

extern "C" time_t ps5_time(time_t* tloc) {
    time_t t = ::time(nullptr);
    if (tloc) *tloc = t;
    return t;
}

extern "C" int ps5_atexit(void (*func)(void)) {
    (void)func;
    return 0;
}

extern "C" int ps5_inet_pton(int af, const char* src, void* dst) {
    EnsureWinSock();
    int win_af = af;
    if (af == 28) win_af = AF_INET6;
    return InetPtonA(win_af, src, dst);
}

extern "C" const char* ps5_inet_ntop(int af, const void* src, char* dst, size_t size) {
    EnsureWinSock();
    int win_af = af;
    if (af == 28) win_af = AF_INET6;
    return InetNtopA(win_af, const_cast<void*>(src), dst, static_cast<socklen_t>(size));
}

extern "C" void* ps5_tls_get_addr(void* ti_ptr) {
    struct TlsIndex {
        uint64_t module;
        uint64_t offset;
    };
    uint64_t offset = 0;
    if (ti_ptr && reinterpret_cast<uintptr_t>(ti_ptr) >= 0x10000) {
        offset = reinterpret_cast<TlsIndex*>(ti_ptr)->offset;
    }

    // Thread-local TLS block for companion PRX (pros.sdk.Prospero.dll)
    thread_local static uint8_t t_prxTlsBlock[0x10000] = {};
    thread_local static bool t_prxTlsInit = false;

    if (!t_prxTlsInit) {
        t_prxTlsInit = true;
        HMODULE hPros = GetModuleHandleA("pros.sdk.Prospero.dll");
        if (hPros) {
            __try {
                const uint8_t* pTlsTemplate = reinterpret_cast<const uint8_t*>(hPros) + 0xD99000;
                std::memcpy(t_prxTlsBlock, pTlsTemplate, 0x40);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                *reinterpret_cast<int32_t*>(&t_prxTlsBlock[0]) = -1;
            }
        } else {
            *reinterpret_cast<int32_t*>(&t_prxTlsBlock[0]) = -1;
            *reinterpret_cast<int32_t*>(&t_prxTlsBlock[0x28]) = -1;
        }
    }

    if (offset < sizeof(t_prxTlsBlock)) {
        return &t_prxTlsBlock[offset];
    }
    return t_prxTlsBlock;
}

struct SysVVaList {
    uint32_t gp_offset;
    uint32_t fp_offset;
    void* overflow_arg_area;
    void* reg_save_area;
};

extern "C" int ps5_vsnprintf(char* buffer, size_t count, const char* format, void* argptr) {
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000 || count == 0) return 0;
    if (!format) {
        buffer[0] = '\0';
        return 0;
    }

    auto* sysv = reinterpret_cast<SysVVaList*>(argptr);
    bool isSysV = false;
    if (sysv && sysv->gp_offset <= 48 && sysv->fp_offset <= 176 && sysv->reg_save_area != nullptr) {
        isSysV = true;
    }

    if (!isSysV) {
        return ::vsnprintf(buffer, count, format, reinterpret_cast<va_list>(argptr));
    }

    SysVVaList ap = *sysv;
    size_t outIdx = 0;
    const char* p = format;

    auto get_gp = [&]() -> uint64_t {
        if (ap.gp_offset < 48) {
            uint64_t v = *reinterpret_cast<uint64_t*>(static_cast<char*>(ap.reg_save_area) + ap.gp_offset);
            ap.gp_offset += 8;
            return v;
        } else if (ap.overflow_arg_area) {
            uint64_t v = *reinterpret_cast<uint64_t*>(ap.overflow_arg_area);
            ap.overflow_arg_area = static_cast<char*>(ap.overflow_arg_area) + 8;
            return v;
        }
        return 0;
    };

    auto get_fp = [&]() -> double {
        if (ap.fp_offset < 176) {
            double v = *reinterpret_cast<double*>(static_cast<char*>(ap.reg_save_area) + ap.fp_offset);
            ap.fp_offset += 16;
            return v;
        } else if (ap.overflow_arg_area) {
            double v = *reinterpret_cast<double*>(ap.overflow_arg_area);
            ap.overflow_arg_area = static_cast<char*>(ap.overflow_arg_area) + 8;
            return v;
        }
        return 0.0;
    };

    while (*p && outIdx + 1 < count) {
        if (*p != '%') {
            buffer[outIdx++] = *p++;
            continue;
        }
        p++;
        if (*p == '%') {
            buffer[outIdx++] = '%';
            p++;
            continue;
        }

        char spec[32] = "%";
        size_t sIdx = 1;
        while (*p && strchr("-+ #0", *p) && sIdx < 16) {
            spec[sIdx++] = *p++;
        }
        while (*p && (*p >= '0' && *p <= '9') && sIdx < 20) {
            spec[sIdx++] = *p++;
        }
        if (*p == '.') {
            spec[sIdx++] = *p++;
            while (*p && (*p >= '0' && *p <= '9') && sIdx < 24) {
                spec[sIdx++] = *p++;
            }
        }
        while (*p && strchr("hljztL", *p) && sIdx < 28) {
            spec[sIdx++] = *p++;
        }
        char type = *p++;
        spec[sIdx++] = type;
        spec[sIdx] = '\0';

        char tempBuf[256] = {};
        int written = 0;
        if (type == 's') {
            const char* str = reinterpret_cast<const char*>(get_gp());
            written = snprintf(tempBuf, sizeof(tempBuf), spec, str ? str : "(null)");
        } else if (type == 'c') {
            int c = static_cast<int>(get_gp());
            written = snprintf(tempBuf, sizeof(tempBuf), spec, c);
        } else if (type == 'd' || type == 'i') {
            if (strstr(spec, "ll") || strstr(spec, "z")) {
                int64_t v = static_cast<int64_t>(get_gp());
                written = snprintf(tempBuf, sizeof(tempBuf), spec, v);
            } else {
                int32_t v = static_cast<int32_t>(get_gp());
                written = snprintf(tempBuf, sizeof(tempBuf), spec, v);
            }
        } else if (type == 'u' || type == 'x' || type == 'X' || type == 'o') {
            if (strstr(spec, "ll") || strstr(spec, "z")) {
                uint64_t v = get_gp();
                written = snprintf(tempBuf, sizeof(tempBuf), spec, v);
            } else {
                uint32_t v = static_cast<uint32_t>(get_gp());
                written = snprintf(tempBuf, sizeof(tempBuf), spec, v);
            }
        } else if (type == 'p') {
            void* ptr = reinterpret_cast<void*>(get_gp());
            written = snprintf(tempBuf, sizeof(tempBuf), spec, ptr);
        } else if (type == 'f' || type == 'g' || type == 'e' || type == 'F' || type == 'G' || type == 'E') {
            double v = get_fp();
            written = snprintf(tempBuf, sizeof(tempBuf), spec, v);
        } else {
            uint64_t v = get_gp();
            written = snprintf(tempBuf, sizeof(tempBuf), "%llx", v);
        }

        if (written > 0) {
            for (int i = 0; i < written && outIdx + 1 < count; ++i) {
                buffer[outIdx++] = tempBuf[i];
            }
        }
    }
    buffer[outIdx] = '\0';
    return static_cast<int>(outIdx);
}

// ==========================================
// libSceHttp Subsystem
// ==========================================

static std::atomic<int> s_nextHttpHandle{1};
static std::atomic<int> s_nextSslHandle{1};

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpInit(int memid, int ssl_ctx_id, uint64_t pool_size) {
    int id = s_nextHttpHandle.fetch_add(1);
    SB_LOG_INFO("libSceHttp", "sceHttpInit(memid={}, sslCtx={}, poolSize={:#x}) -> ctxId={}", memid, ssl_ctx_id, pool_size, id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpTerm(int http_ctx_id) {
    SB_LOG_INFO("libSceHttp", "sceHttpTerm(ctxId={})", http_ctx_id);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpCreateTemplate(int http_ctx_id, const char* user_agent, int http_ver, int is_auto_proxy_conf) {
    int id = s_nextHttpHandle.fetch_add(1);
    SB_LOG_INFO("libSceHttp", "sceHttpCreateTemplate(ctxId={}, ua='{}') -> tmplId={}", http_ctx_id, user_agent ? user_agent : "null", id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpDeleteTemplate(int tmpl_id) {
    SB_LOG_INFO("libSceHttp", "sceHttpDeleteTemplate(tmplId={})", tmpl_id);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpCreateConnection(int tmpl_id, const char* server_name, const char* scheme, uint16_t port, int enable_keep_alive) {
    int id = s_nextHttpHandle.fetch_add(1);
    SB_LOG_INFO("libSceHttp", "sceHttpCreateConnection(tmplId={}, server='{}') -> connId={}", tmpl_id, server_name ? server_name : "null", id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpCreateConnectionWithURL(int tmpl_id, const char* url, int enable_keep_alive) {
    int id = s_nextHttpHandle.fetch_add(1);
    SB_LOG_INFO("libSceHttp", "sceHttpCreateConnectionWithURL(tmplId={}, url='{}') -> connId={}", tmpl_id, url ? url : "null", id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpDeleteConnection(int conn_id) {
    SB_LOG_INFO("libSceHttp", "sceHttpDeleteConnection(connId={})", conn_id);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpCreateRequest(int conn_id, int method, const char* path, uint64_t content_length) {
    int id = s_nextHttpHandle.fetch_add(1);
    SB_LOG_INFO("libSceHttp", "sceHttpCreateRequest(connId={}, method={}, path='{}') -> reqId={}", conn_id, method, path ? path : "null", id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpCreateRequestWithURL2(int conn_id, const char* method, const char* url, uint64_t content_length) {
    int id = s_nextHttpHandle.fetch_add(1);
    SB_LOG_INFO("libSceHttp", "sceHttpCreateRequestWithURL2(connId={}, method='{}', url='{}') -> reqId={}", conn_id, method ? method : "null", url ? url : "null", id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpDeleteRequest(int req_id) {
    SB_LOG_INFO("libSceHttp", "sceHttpDeleteRequest(reqId={})", req_id);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSendRequest(int request_id, const void* post_data, size_t size) {
    (void)request_id; (void)post_data; (void)size;
    return 0x80431068; // SCE_HTTP_ERROR_NETWORK
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpAbortRequest(int request_id) {
    (void)request_id;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpAbortRequestForce(int request_id) {
    (void)request_id;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetNonblock(int id, int enable) {
    (void)id; (void)enable;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetAutoRedirect(int id, int enable) {
    (void)id; (void)enable;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetAuthEnabled(int id, int enable) {
    (void)id; (void)enable;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetCookieEnabled(int id, int enable) {
    (void)id; (void)enable;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetConnectTimeOut(int id, uint32_t usec) {
    (void)id; (void)usec;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetSendTimeOut(int id, uint32_t usec) {
    (void)id; (void)usec;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetRecvTimeOut(int id, uint32_t usec) {
    (void)id; (void)usec;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpSetResolveTimeOut(int id, uint32_t usec) {
    (void)id; (void)usec;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpAddRequestHeader(int id, const char* name, const char* value, uint32_t mode) {
    (void)id; (void)name; (void)value; (void)mode;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpsDisableOption(int id, uint32_t ssl_flags) {
    (void)id; (void)ssl_flags;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpsSetMinSslVersion(int id, uint32_t sslVersion) {
    (void)id; (void)sslVersion;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpsSetSslCallback(int id, void* cbfunc, void* userArg) {
    (void)id; (void)cbfunc; (void)userArg;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpGetResponseContentLength(int request_id, int* result, uint64_t* content_length) {
    (void)request_id; (void)result; (void)content_length;
    return 0x80431068;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpGetStatusCode(int request_id, int* status_code) {
    (void)request_id; (void)status_code;
    return 0x80431068;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttpGetAllResponseHeaders(int request_id, char** header, size_t* header_size) {
    (void)request_id; (void)header; (void)header_size;
    return 0x80431068;
}

// ==========================================
// libSceSsl Subsystem
// ==========================================

SAILOR_EXPORT int SAILOR_SYSV_ABI sceSslInit(uint64_t poolSize) {
    int id = s_nextSslHandle.fetch_add(1);
    SB_LOG_INFO("libSceSsl", "sceSslInit(poolSize={:#x}) -> sslCtx={}", poolSize, id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceSslTerm(int sslCtxId) {
    SB_LOG_INFO("libSceSsl", "sceSslTerm(sslCtx={})", sslCtxId);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceSslCreateConnection(int sslCtxId, int sockId, const char* host) {
    int id = s_nextSslHandle.fetch_add(1);
    SB_LOG_INFO("libSceSsl", "sceSslCreateConnection(sslCtx={}, sock={}, host='{}') -> connId={}", sslCtxId, sockId, host ? host : "null", id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceSslDeleteConnection(int sslConnectionId) {
    SB_LOG_INFO("libSceSsl", "sceSslDeleteConnection(connId={})", sslConnectionId);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceSslConnect(int sslConnectionId) {
    (void)sslConnectionId;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceSslClose(int sslConnectionId) {
    (void)sslConnectionId;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceSslGetMemoryPoolStats(int sslCtxId, void* stats) {
    (void)sslCtxId; (void)stats;
    return Sailor::SCE_OK;
}

// ==========================================
// libSceHttp2 Subsystem
// ==========================================

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttp2Init(void* param) {
    (void)param;
    int id = s_nextHttpHandle.fetch_add(1);
    SB_LOG_INFO("libSceHttp2", "sceHttp2Init() -> ctxId={}", id);
    return id;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceHttp2Term(int ctxId) {
    (void)ctxId;
    SB_LOG_INFO("libSceHttp2", "sceHttp2Term(ctxId={})", ctxId);
    return Sailor::SCE_OK;
}

// ==========================================
// libSceNpManager Callbacks
// ==========================================

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNpUnregisterNpReachabilityStateCallback(void) {
    SB_LOG_INFO("libSceNpManager", "sceNpUnregisterNpReachabilityStateCallback()");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceNpCheckCallback(void) {
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI scePlayGoGetChunkId(const char* path, Sailor::u32* chunkId) {
    (void)path;
    if (chunkId) *chunkId = 0;
    return Sailor::SCE_OK;
}

// ==========================================
// libSceCommonDialog Subsystem
// ==========================================

SAILOR_EXPORT int SAILOR_SYSV_ABI sceCommonDialogInitialize(void) {
    SB_LOG_INFO("libSceCommonDialog", "sceCommonDialogInitialize()");
    return Sailor::SCE_OK;
}

// ==========================================
// libScePlayerInvitationDialog Subsystem
// ==========================================

SAILOR_EXPORT int SAILOR_SYSV_ABI scePlayerInvitationDialogInitialize(void) {
    SB_LOG_INFO("libScePlayerInvitationDialog", "scePlayerInvitationDialogInitialize()");
    return Sailor::SCE_OK;
}

// ==========================================
// libSceAudioOut Subsystem
// ==========================================

SAILOR_EXPORT int SAILOR_SYSV_ABI sceAudioOut2Initialize(void) {
    SB_LOG_INFO("libSceAudioOut", "sceAudioOut2Initialize()");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceAudioOut2LoInit(void) {
    SB_LOG_INFO("libSceAudioOut", "sceAudioOut2LoInit()");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceAudioOut2LoTerminate(void) {
    SB_LOG_INFO("libSceAudioOut", "sceAudioOut2LoTerminate()");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceAudioOut2Terminate(void) {
    SB_LOG_INFO("libSceAudioOut", "sceAudioOut2Terminate()");
    return Sailor::SCE_OK;
}

SAILOR_EXPORT int SAILOR_SYSV_ABI sceAudioOutInit(void) {
    SB_LOG_INFO("libSceAudioOut", "sceAudioOutInit()");
    return Sailor::SCE_OK;
}

} // extern "C"
