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

    uint64_t target = 0x142834cc8ULL;
    std::cout << "Searching for references to 0x" << std::hex << target << "...\n";

    for (size_t c = 0x400; c + 7 < 0x1E783FF; ++c) {
        // rip-relative disp: target - (c + 0xc00 + 0x140000000 + inst_len)
        // Check disp32 in instructions
        for (int instLen = 5; instLen <= 8; ++instLen) {
            int32_t disp = *(int32_t*)(data.data() + c + (instLen - 4));
            uint64_t rip = 0x140000000ULL + c + 0xC00 + instLen;
            if (rip + disp == target) {
                std::cout << "Reference to 0x142834cc8 at RVA 0x" << std::hex << (c + 0xC00) 
                          << " (VA 0x" << (0x140000000ULL + c + 0xC00) << "), len=" << instLen << ":\n  Bytes: ";
                for (int k = 0; k < instLen + 4; ++k) {
                    std::cout << std::setfill('0') << std::setw(2) << (int)data[c + k] << " ";
                }
                std::cout << "\n";
            }
        }
    }
    return 0;
}
