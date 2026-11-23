#include <windows.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

int main() {
    HMODULE hMod = LoadLibraryExA("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\pros.sdk.Prospero.dll", nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!hMod) {
        std::cerr << "Failed to load pros.sdk.Prospero.dll: " << GetLastError() << "\n";
        return 1;
    }

    uint8_t* base = (uint8_t*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    
    // Find import IAT entry for sceVideoOutOpen
    IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir.VirtualAddress);

    uint64_t iatVideoOutOpen = 0;
    while (imp->Name) {
        const char* name = (const char*)(base + imp->Name);
        IMAGE_THUNK_DATA64* thunk = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA64* origThunk = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        
        while (origThunk->u1.AddressOfData) {
            if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                if (std::string(ibn->Name) == "sceVideoOutOpen") {
                    iatVideoOutOpen = (uint64_t)thunk - (uint64_t)base;
                    std::cout << "Found sceVideoOutOpen IAT RVA: 0x" << std::hex << iatVideoOutOpen << " in " << name << "\n";
                }
            }
            thunk++;
            origThunk++;
        }
        imp++;
    }

    if (!iatVideoOutOpen) {
        std::cout << "Could not find sceVideoOutOpen in IAT\n";
        return 1;
    }

    // Now search .text section for callers: call [rip+disp] (FF 15) or mov reg, [rip+disp]
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        std::string secName((const char*)sec->Name, 8);
        if (secName.rfind(".text", 0) == 0) {
            uint8_t* text = base + sec->VirtualAddress;
            uint32_t size = sec->Misc.VirtualSize;
            std::cout << "Searching " << secName << " (0x" << std::hex << sec->VirtualAddress << ", size 0x" << size << ")...\n";
            for (uint32_t i = 0; i + 6 <= size; ++i) {
                // FF 15 disp32 -> call [rip + disp32]
                if (text[i] == 0xFF && text[i+1] == 0x15) {
                    int32_t disp = *(int32_t*)(text + i + 2);
                    uint64_t targetRva = sec->VirtualAddress + i + 6 + disp;
                    if (targetRva == iatVideoOutOpen) {
                        std::cout << "Direct call at RVA 0x" << std::hex << (sec->VirtualAddress + i) << "\n";
                    }
                }
                // 48 8B 05 disp32 -> mov rax, [rip + disp32]
                if (text[i] == 0x48 && text[i+1] == 0x8B && (text[i+2] & 0xC7) == 0x05) {
                    int32_t disp = *(int32_t*)(text + i + 3);
                    uint64_t targetRva = sec->VirtualAddress + i + 7 + disp;
                    if (targetRva == iatVideoOutOpen) {
                        std::cout << "Pointer load at RVA 0x" << std::hex << (sec->VirtualAddress + i) << "\n";
                    }
                }
            }
        }
    }

    return 0;
}
