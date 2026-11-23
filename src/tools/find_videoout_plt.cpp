#include <windows.h>
#include <iostream>
#include <iomanip>
#include <vector>

int main() {
    HMODULE hMod = LoadLibraryExA("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\pros.sdk.Prospero.dll", nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!hMod) return 1;

    uint8_t* base = (uint8_t*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir.VirtualAddress);

    uint64_t iatRva = 0;
    while (imp->Name) {
        const char* name = (const char*)(base + imp->Name);
        IMAGE_THUNK_DATA64* thunk = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA64* origThunk = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        while (origThunk->u1.AddressOfData) {
            if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                if (std::string(ibn->Name) == "sceVideoOutOpen") {
                    iatRva = (uint64_t)thunk - (uint64_t)base;
                    break;
                }
            }
            thunk++;
            origThunk++;
        }
        if (iatRva) break;
        imp++;
    }

    std::cout << "IAT RVA: 0x" << std::hex << iatRva << "\n";

    // Search all sections for jmp [rip+disp] or call [rip+disp] where (rip + disp) == iatRva
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        uint8_t* ptr = base + sec->VirtualAddress;
        uint32_t sz = sec->Misc.VirtualSize;
        for (uint32_t i = 0; i + 6 <= sz; ++i) {
            if ((ptr[i] == 0xFF && ptr[i+1] == 0x25) || (ptr[i] == 0xFF && ptr[i+1] == 0x15)) {
                int32_t disp = *(int32_t*)(ptr + i + 2);
                uint64_t rip = sec->VirtualAddress + i + 6;
                if (rip + disp == iatRva) {
                    std::cout << "Found PLT/call at sec " << (const char*)sec->Name << " RVA 0x" << std::hex << (sec->VirtualAddress + i) 
                              << " (" << (ptr[i+1] == 0x25 ? "JMP" : "CALL") << ")\n";
                }
            }
        }
    }
    return 0;
}
