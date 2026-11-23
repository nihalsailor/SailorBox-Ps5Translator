/**
 * ============================================================================
 * Tool: thunk_disassembler
 * Description: PE binary analyzer that inspects the Import Directory, maps
 *              Import Address Table (IAT) entries, and disassembles SysV-to-MSABI
 *              parameter marshalling thunks.
 * Usage: thunk_disassembler.exe <pe_binary_path> [symbol_filter]
 * ============================================================================
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <pe_binary_path> [symbol_filter]\n", argv[0]);
        printf("Example: %s eboot.exe Kernel\n", argv[0]);
        return 1;
    }

    const char* filePath = argv[1];
    const char* filter = (argc >= 3) ? argv[2] : "";

    FILE* f = fopen(filePath, "rb");
    if (!f) {
        printf("[ERROR] Failed to open file: %s\n", filePath);
        return 1;
    }

    IMAGE_DOS_HEADER dos = {};
    if (fread(&dos, sizeof(dos), 1, f) != 1 || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        printf("[ERROR] Invalid DOS header\n");
        fclose(f);
        return 1;
    }

    fseek(f, dos.e_lfanew, SEEK_SET);
    IMAGE_NT_HEADERS64 nt = {};
    if (fread(&nt, sizeof(nt), 1, f) != 1 || nt.Signature != IMAGE_NT_SIGNATURE) {
        printf("[ERROR] Invalid NT header\n");
        fclose(f);
        return 1;
    }

    printf("========================================================================\n");
    printf(" Thunk Disassembler: %s\n", filePath);
    printf(" ImageBase: 0x%016llX, Sections: %u\n", nt.OptionalHeader.ImageBase, nt.FileHeader.NumberOfSections);
    printf("========================================================================\n\n");

    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    fread(sections.data(), sizeof(IMAGE_SECTION_HEADER), nt.FileHeader.NumberOfSections, f);

    auto rvaToOffset = [&](uint64_t rva) -> uint64_t {
        for (const auto& sec : sections) {
            if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + sec.Misc.VirtualSize) {
                return sec.PointerToRawData + (rva - sec.VirtualAddress);
            }
        }
        return 0;
    };

    uint64_t importRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva) {
        printf("[INFO] No import directory present.\n");
        fclose(f);
        return 0;
    }

    uint64_t importOff = rvaToOffset(importRva);
    fseek(f, (long)importOff, SEEK_SET);

    IMAGE_IMPORT_DESCRIPTOR desc;
    int importedFuncs = 0;

    while (fread(&desc, sizeof(desc), 1, f) == 1 && desc.Name != 0) {
        long savePos = ftell(f);

        uint64_t nameOff = rvaToOffset(desc.Name);
        fseek(f, (long)nameOff, SEEK_SET);
        char dllName[256] = {};
        fread(dllName, 1, sizeof(dllName) - 1, f);

        uint64_t thunkRva = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
        uint64_t iatRva = desc.FirstThunk;
        uint64_t thunkOff = rvaToOffset(thunkRva);

        fseek(f, (long)thunkOff, SEEK_SET);
        uint64_t thunkVal = 0;
        int index = 0;

        while (fread(&thunkVal, 8, 1, f) == 1 && thunkVal != 0) {
            uint64_t currentIatRva = iatRva + index * 8;
            long currentThunkPos = ftell(f);

            char funcName[256] = "Ordinal";
            if (!(thunkVal & 0x8000000000000000ULL)) {
                uint64_t funcNameOff = rvaToOffset(thunkVal) + 2; // skip 2-byte hint
                fseek(f, (long)funcNameOff, SEEK_SET);
                fread(funcName, 1, sizeof(funcName) - 1, f);
            }

            if (filter[0] == '\0' || strstr(funcName, filter) != NULL || strstr(dllName, filter) != NULL) {
                importedFuncs++;
                printf("[%s] IAT RVA: 0x%08llX (VA 0x%016llX) -> %s\n",
                    dllName, currentIatRva, nt.OptionalHeader.ImageBase + currentIatRva, funcName);
            }

            fseek(f, currentThunkPos, SEEK_SET);
            index++;
        }

        fseek(f, savePos, SEEK_SET);
    }

    printf("\nTotal matching imports displayed: %d\n", importedFuncs);
    fclose(f);
    return 0;
}
