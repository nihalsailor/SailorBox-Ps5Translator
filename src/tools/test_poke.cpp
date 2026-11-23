#include <windows.h>
#include <iostream>
#include <iomanip>

int main(int argc, char** argv) {
    DWORD pid = 20528;
    if (argc > 1) pid = atoi(argv[1]);

    HANDLE hProc = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        std::cout << "OpenProcess failed: " << GetLastError() << "\n";
        return 1;
    }

    uint8_t val = 0x04;
    SIZE_T written = 0;
    DWORD oldProtect = 0;
    VirtualProtectEx(hProc, (LPVOID)0x142834cc8ULL, 4, PAGE_READWRITE, &oldProtect);
    if (WriteProcessMemory(hProc, (LPVOID)0x142834cc8ULL, &val, 1, &written)) {
        std::cout << "Successfully poked 0x142834cc8 with 0x" << std::hex << (int)val 
                  << " in PID " << pid << "\n";
    } else {
        std::cout << "WriteProcessMemory failed: " << GetLastError() << "\n";
    }

    VirtualProtectEx(hProc, (LPVOID)0x142834cc8ULL, 4, oldProtect, &oldProtect);
    CloseHandle(hProc);
    return 0;
}
