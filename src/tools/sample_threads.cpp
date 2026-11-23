#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

struct Mod { std::string name; uint64_t base, size; };

int main(int argc, char* argv[]) {
    DWORD pid = 0;
    if (argc > 1) {
        pid = std::stoul(argv[1]);
    } else {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 pe{sizeof(pe)};
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, "eboot.exe") == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }

    if (!pid) return 1;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        std::cerr << "OpenProcess failed: " << GetLastError() << "\n";
        return 1;
    }

    HMODULE hMods[1024]; DWORD cbNeeded;
    std::vector<Mod> modules;
    if (EnumProcessModules(hProc, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); ++i) {
            char szModName[MAX_PATH]; MODULEINFO mi;
            if (GetModuleInformation(hProc, hMods[i], &mi, sizeof(mi))) {
                GetModuleBaseNameA(hProc, hMods[i], szModName, sizeof(szModName));
                modules.push_back({szModName, (uint64_t)mi.lpBaseOfDll, (uint64_t)mi.SizeOfImage});
            }
        }
    }

    auto resolve = [&](uint64_t addr) -> std::string {
        for (const auto& m : modules) {
            if (addr >= m.base && addr < m.base + m.size) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s+0x%llx", m.name.c_str(), addr - m.base);
                return buf;
            }
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%llx", addr);
        return buf;
    };

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    std::vector<DWORD> tids;
    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                tids.push_back(te.th32ThreadID);
            }
        } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);

    for (DWORD tid : tids) {
        HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!hThread) {
            hThread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT, FALSE, tid);
        }
        if (!hThread) continue;

        FILETIME ct, et, kt, ut;
        ULARGE_INTEGER ukt, uut;
        uint64_t totalCpuMs = 0;
        if (GetThreadTimes(hThread, &ct, &et, &kt, &ut)) {
            ukt.LowPart = kt.dwLowDateTime; ukt.HighPart = kt.dwHighDateTime;
            uut.LowPart = ut.dwLowDateTime; uut.HighPart = ut.dwHighDateTime;
            totalCpuMs = (ukt.QuadPart + uut.QuadPart) / 10000;
        }

        SuspendThread(hThread);
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(hThread, &ctx)) {
            std::cout << "\n=== TID " << tid << " (CPU " << totalCpuMs << " ms) ===\n";
            std::cout << "RIP: " << resolve(ctx.Rip) << " RSP: 0x" << std::hex << ctx.Rsp << std::dec << "\n";
            uint64_t stack[512]{};
            SIZE_T read = 0;
            if (ReadProcessMemory(hProc, (LPCVOID)ctx.Rsp, stack, sizeof(stack), &read)) {
                for (int i = 0; i < 512; ++i) {
                    uint64_t val = stack[i];
                    for (const auto& m : modules) {
                        if (val >= m.base && val < m.base + m.size) {
                            std::cout << "  [" << i << "] " << resolve(val) << "\n";
                            break;
                        }
                    }
                }
            }
        }
        ResumeThread(hThread);
        CloseHandle(hThread);
    }

    CloseHandle(hProc);
    return 0;
}
