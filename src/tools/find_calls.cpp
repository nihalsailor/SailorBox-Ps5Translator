#include <windows.h>
#include <iostream>
#include <iomanip>
#include <vector>

int main() {
    HANDLE hFile = CreateFileA("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.exe", 
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 1;

    DWORD size = GetFileSize(hFile, nullptr);
    std::vector<uint8_t> data(size);
    DWORD read = 0;
    ReadFile(hFile, data.data(), size, &read, nullptr);
    CloseHandle(hFile);

    uint64_t startVa = 0x140cff000ULL;
    uint64_t endVa   = 0x140d01000ULL;
    uint32_t startOff = (uint32_t)(startVa - 0x140000000ULL - 0xc00);
    uint32_t endOff   = (uint32_t)(endVa - 0x140000000ULL - 0xc00);

    for (uint32_t i = startOff; i < endOff; ++i) {
        if (data[i] == 0xE8) { // call rel32
            int32_t disp = *(int32_t*)(&data[i+1]);
            uint64_t rip = 0x140000000ULL + i + 0xC00 + 5;
            uint64_t target = rip + disp;
            std::cout << "Call at 0x" << std::hex << (rip - 5) << " -> 0x" << target << "\n";
        }
        if (data[i] == 0xFF && (data[i+1] == 0x15 || data[i+1] == 0x25)) { // call/jmp [rip+disp]
            int32_t disp = *(int32_t*)(&data[i+2]);
            uint64_t rip = 0x140000000ULL + i + 0xC00 + 6;
            uint64_t target = rip + disp;
            std::cout << (data[i+1] == 0x15 ? "Call" : "Jmp") << " [rip] at 0x" << std::hex << (rip - 6) << " -> [0x" << target << "]\n";
        }
    }
    return 0;
}
