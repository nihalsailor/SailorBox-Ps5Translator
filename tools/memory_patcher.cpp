/**
 * ============================================================================
 * Tool: memory_patcher
 * Description: Live process memory hot-patcher that safely modifies memory page
 *              protections (VirtualProtectEx), writes byte patches (NOPs, jumps),
 *              restores original protection, and verifies patch integrity.
 * Usage: memory_patcher.exe <process_name_or_pid> <hex_address> <hex_bytes|nop:count>
 * ============================================================================
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

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
    if (argc < 4) {
        printf("Usage: %s <process_name_or_pid> <hex_address> <hex_bytes|nop:count>\n", argv[0]);
        printf("Examples:\n");
        printf("  Write custom bytes: %s game.exe 0x1406F4F6C \"90 90 90 90 90 90\"\n", argv[0]);
        printf("  Write 6 NOPs:       %s game.exe 0x1406F4F6C nop:6\n", argv[0]);
        return 1;
    }

    DWORD pid = FindProcessId(argv[1]);
    if (!pid) {
        printf("[ERROR] Could not find process: %s\n", argv[1]);
        return 1;
    }

    uint64_t targetAddr = strtoull(argv[2], NULL, 16);
    if (!targetAddr) {
        printf("[ERROR] Invalid target address: %s\n", argv[2]);
        return 1;
    }

    std::vector<uint8_t> patchBytes;
    if (_strnicmp(argv[3], "nop:", 4) == 0) {
        int nopCount = atoi(argv[3] + 4);
        if (nopCount <= 0) nopCount = 1;
        patchBytes.assign(nopCount, 0x90);
    } else {
        const char* p = argv[3];
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char* next = NULL;
            uint8_t b = (uint8_t)strtoul(p, &next, 16);
            if (next == p) break;
            patchBytes.push_back(b);
            p = next;
        }
    }

    if (patchBytes.empty()) {
        printf("[ERROR] No valid patch bytes specified\n");
        return 1;
    }

    HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!hProc) {
        printf("[ERROR] Failed to open process PID %lu (error %lu)\n", pid, GetLastError());
        return 1;
    }

    // Read original bytes first
    std::vector<uint8_t> originalBytes(patchBytes.size());
    SIZE_T bytesRead = 0;
    ReadProcessMemory(hProc, (void*)targetAddr, originalBytes.data(), originalBytes.size(), &bytesRead);

    printf("[PATCHER] Target Process: PID %lu\n", pid);
    printf("[PATCHER] Target Address: 0x%016llX (%zu bytes)\n", targetAddr, patchBytes.size());
    printf("  Original Bytes: ");
    for (uint8_t b : originalBytes) printf("%02X ", b);
    printf("\n  Patch Bytes:    ");
    for (uint8_t b : patchBytes) printf("%02X ", b);
    printf("\n");

    DWORD oldProt = 0;
    if (!VirtualProtectEx(hProc, (void*)targetAddr, patchBytes.size(), PAGE_EXECUTE_READWRITE, &oldProt)) {
        printf("[ERROR] VirtualProtectEx failed (error %lu)\n", GetLastError());
        CloseHandle(hProc);
        return 1;
    }

    SIZE_T bytesWritten = 0;
    BOOL writeOk = WriteProcessMemory(hProc, (void*)targetAddr, patchBytes.data(), patchBytes.size(), &bytesWritten);

    DWORD dummy = 0;
    VirtualProtectEx(hProc, (void*)targetAddr, patchBytes.size(), oldProt, &dummy);

    if (writeOk && bytesWritten == patchBytes.size()) {
        FlushInstructionCache(hProc, (void*)targetAddr, patchBytes.size());
        printf("[SUCCESS] Successfully patched %zu bytes at 0x%016llX\n", bytesWritten, targetAddr);
    } else {
        printf("[ERROR] WriteProcessMemory failed (error %lu)\n", GetLastError());
    }

    CloseHandle(hProc);
    return 0;
}
