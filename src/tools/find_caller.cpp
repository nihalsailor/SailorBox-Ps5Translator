#include <windows.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

int main() {
    HMODULE hMod = LoadLibraryExA("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.exe", nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!hMod) {
        std::cerr << "Failed to load eboot.exe: " << GetLastError() << "\n";
        return 1;
    }

    uint8_t* base = (uint8_t*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir.VirtualAddress);

    std::vector<std::pair<std::string, uint64_t>> videoOutIats;

    while (imp->Name) {
        const char* name = (const char*)(base + imp->Name);
        IMAGE_THUNK_DATA64* thunk = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA64* origThunk = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        while (origThunk->u1.AddressOfData) {
            if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                std::string symName(ibn->Name);
                if (symName.find("VideoOut") != std::string::npos || symName.find("Up36PTk687E") != std::string::npos || symName.find("rKBU") != std::string::npos) {
                    uint64_t rva = (uint64_t)thunk - (uint64_t)base;
                    videoOutIats.push_back({symName, rva});
                    std::cout << "Import: " << symName << " in " << name << " -> IAT RVA 0x" << std::hex << rva << "\n";
                }
            }
            thunk++;
            origThunk++;
        }
        imp++;
    }

    // Now search .text section (RVA 0xC00, size ...) for references to these IAT RVAs
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        std::string secName((const char*)sec->Name, 8);
        if (secName.rfind(".text", 0) == 0) {
            uint8_t* text = base + sec->VirtualAddress;
            uint32_t size = sec->Misc.VirtualSize;
            std::cout << "Scanning " << secName << " for callers...\n";
            for (const auto& [sym, iatRva] : videoOutIats) {
                int count = 0;
                for (uint32_t i = 0; i + 6 <= size; ++i) {
                    if ((text[i] == 0xFF && text[i+1] == 0x15) || (text[i] == 0xFF && text[i+1] == 0x25)) {
                        int32_t disp = *(int32_t*)(text + i + 2);
                        uint64_t rip = sec->VirtualAddress + i + 6;
                        if (rip + disp == iatRva) {
                            std::cout << "  " << (text[i+1] == 0x15 ? "CALL" : "JMP") << " to " << sym 
                                      << " at RVA 0x" << std::hex << (sec->VirtualAddress + i) 
                                      << " (VA 0x" << (0x140000000ULL + sec->VirtualAddress + i) << ")\n";
                            count++;
                        }
                    }
                }
                std::cout << "Symbol " << sym << " had " << count << " calls\n";
            }
        }
    }

    return 0;
}
