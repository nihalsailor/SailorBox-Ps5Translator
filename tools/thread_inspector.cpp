/**
 * ============================================================================
 * Tool: thread_inspector
 * Description: Multi-threaded process inspector that snapshots all running
 *              threads, reads register contexts, checks instruction pointers,
 *              and unwinds stack frames to diagnose deadlocks and wait states.
 * Usage: thread_inspector.exe <process_name_or_pid> [filter_module]
 * ============================================================================
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

struct ModuleInfo {
    char name[MAX_PATH];
    uint64_t base;
    uint64_t size;
};

static DWORD FindProcessId(const char* nameOrPid) {
    char* end = NULL;
    DWORD pid = strtoul(nameOrPid, &end, 10);
    if (end && *end == '\0') return pid;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    DWORD foundPid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, nameOrPid) == 0) {
                foundPid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return foundPid;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <process_name_or_pid> [filter_module]\n", argv[0]);
        printf("Example: %s game.exe\n", argv[0]);
        return 1;
    }

    DWORD pid = FindProcessId(argv[1]);
    if (!pid) {
        printf("[ERROR] Could not find process: %s\n", argv[1]);
        return 1;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        printf("[ERROR] Failed to open process PID %lu (error %lu)\n", pid, GetLastError());
        return 1;
    }

    // Collect module list
    std::vector<ModuleInfo> modules;
    HANDLE hModSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hModSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me = { sizeof(me) };
        if (Module32First(hModSnap, &me)) {
            do {
                ModuleInfo mi = {};
                strncpy_s(mi.name, me.szModule, sizeof(mi.name) - 1);
                mi.base = (uint64_t)me.modBaseAddr;
                mi.size = (uint64_t)me.modBaseSize;
                modules.push_back(mi);
            } while (Module32Next(hModSnap, &me));
        }
        CloseHandle(hModSnap);
    }

    auto resolveAddress = [&](uint64_t addr, char* outBuf, size_t bufSize) {
        for (const auto& m : modules) {
            if (addr >= m.base && addr < m.base + m.size) {
                snprintf(outBuf, bufSize, "%s+0x%llX", m.name, addr - m.base);
                return;
            }
        }
        snprintf(outBuf, bufSize, "0x%016llX", addr);
    };

    printf("========================================================================\n");
    printf(" Thread Inspector: PID %lu (Found %zu loaded modules)\n", pid, modules.size());
    printf("========================================================================\n\n");

    HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hThreadSnap == INVALID_HANDLE_VALUE) {
        CloseHandle(hProc);
        return 1;
    }

    THREADENTRY32 te = { sizeof(te) };
    int threadCount = 0;

    if (Thread32First(hThreadSnap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                threadCount++;
                HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (hThread) {
                    SuspendThread(hThread);
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    if (GetThreadContext(hThread, &ctx)) {
                        char locStr[512] = {};
                        resolveAddress(ctx.Rip, locStr, sizeof(locStr));

                        printf("Thread #%d [TID %lu]: RIP = %s\n", threadCount, te.th32ThreadID, locStr);
                        printf("  RSP: 0x%016llX   RAX: 0x%016llX   RBX: 0x%016llX\n", ctx.Rsp, ctx.Rax, ctx.Rbx);
                        printf("  RCX: 0x%016llX   RDX: 0x%016llX   RBP: 0x%016llX\n", ctx.Rcx, ctx.Rdx, ctx.Rbp);

                        // Read top 16 values from stack to find return addresses
                        uint64_t stack[16] = {};
                        SIZE_T bytesRead = 0;
                        if (ReadProcessMemory(hProc, (void*)ctx.Rsp, stack, sizeof(stack), &bytesRead) && bytesRead > 0) {
                            size_t count = bytesRead / sizeof(uint64_t);
                            printf("  Call Stack References:\n");
                            for (size_t i = 0; i < count; ++i) {
                                for (const auto& m : modules) {
                                    if (stack[i] >= m.base && stack[i] < m.base + m.size) {
                                        printf("    [RSP+0x%02zX] -> %s+0x%llX\n", i * 8, m.name, stack[i] - m.base);
                                        break;
                                    }
                                }
                            }
                        }
                        printf("\n");
                    }
                    ResumeThread(hThread);
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hThreadSnap, &te));
    }

    CloseHandle(hThreadSnap);
    CloseHandle(hProc);
    printf("Total threads inspected: %d\n", threadCount);
    return 0;
}
