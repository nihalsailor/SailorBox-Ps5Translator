#include <windows.h>
#include <iostream>
#include <iomanip>

int main() {
    HANDLE hFile = CreateFileA("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.exe",
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 1;

    DWORD off = 0xcf79af; // file offset of call sceVideoOutOpen
    SetFilePointer(hFile, off - 1024, nullptr, FILE_BEGIN);
    uint8_t code[1024]{}; DWORD read = 0;
    ReadFile(hFile, code, sizeof(code), &read, nullptr);

    // Search backwards for 55 48 89 e5 (push rbp; mov rbp, rsp) or 41 57 41 56 (push r15; push r14)
    for (int i = 1020; i >= 0; --i) {
        if ((code[i] == 0x55 && code[i+1] == 0x48 && code[i+2] == 0x89 && code[i+3] == 0xE5) ||
            (code[i] == 0x48 && code[i+1] == 0x83 && code[i+2] == 0xEC) ||
            (code[i] == 0x41 && code[i+1] == 0x57 && code[i+2] == 0x41 && code[i+3] == 0x56)) {
            DWORD funcOff = off - 1024 + i;
            std::cout << "Possible function prologue at file offset 0x" << std::hex << funcOff 
                      << " (RVA 0x" << (funcOff + 0xc00) << ", VA 0x" << (0x140000000ULL + funcOff + 0xc00) << ")\n";
        }
    }
    CloseHandle(hFile);
    return 0;
}
