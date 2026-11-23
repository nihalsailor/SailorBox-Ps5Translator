/**
 * ============================================================================
 * Tool: opcode_scanner
 * Description: Fast executable section binary scanner that searches for
 *              relative CALL/JMP opcodes, analyzes target offsets, and inspects
 *              caller return-value validation patterns in x64 binaries.
 * Usage: opcode_scanner.exe <binary_path> <mode: call|pattern> <target_rva_or_hex_pattern>
 * ============================================================================
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: %s <binary_path> <call|pattern> <target_rva_hex|hex_byte_pattern>\n", argv[0]);
        printf("Examples:\n");
        printf("  Find all calls to RVA 0x1E76CB0: %s game.exe call 0x1E76CB0\n", argv[0]);
        printf("  Find byte pattern:              %s game.exe pattern \"05 F4 FF FD 7F\"\n", argv[0]);
        return 1;
    }

    const char* filePath = argv[1];
    const char* mode = argv[2];
    const char* query = argv[3];

    FILE* f = fopen(filePath, "rb");
    if (!f) {
        printf("[ERROR] Failed to open file: %s\n", filePath);
        return 1;
    }

    IMAGE_DOS_HEADER dos = {};
    fread(&dos, sizeof(dos), 1, f);
    fseek(f, dos.e_lfanew, SEEK_SET);
    IMAGE_NT_HEADERS64 nt = {};
    fread(&nt, sizeof(nt), 1, f);

    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    fread(sections.data(), sizeof(IMAGE_SECTION_HEADER), nt.FileHeader.NumberOfSections, f);

    // Locate .text or first executable section
    const IMAGE_SECTION_HEADER* codeSec = NULL;
    for (const auto& sec : sections) {
        if (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            codeSec = &sec;
            break;
        }
    }

    if (!codeSec) {
        printf("[ERROR] No executable code section found\n");
        fclose(f);
        return 1;
    }

    printf("[SCANNER] Scanning code section: %s (RVA 0x%08lX, Size 0x%08lX)...\n",
        codeSec->Name, codeSec->VirtualAddress, codeSec->Misc.VirtualSize);

    fseek(f, codeSec->PointerToRawData, SEEK_SET);
    std::vector<uint8_t> code(codeSec->Misc.VirtualSize);
    fread(code.data(), 1, codeSec->Misc.VirtualSize, f);
    fclose(f);

    uint64_t textRva = codeSec->VirtualAddress;
    size_t textSize = codeSec->Misc.VirtualSize;

    if (_stricmp(mode, "call") == 0) {
        uint64_t targetRva = strtoull(query, NULL, 16);
        printf("[SCANNER] Searching for relative CALLs (0xE8) targeting RVA 0x%llX...\n\n", targetRva);

        int matchCount = 0;
        for (size_t i = 0; i < textSize - 5; ++i) {
            if (code[i] == 0xE8) {
                int32_t rel = *(int32_t*)&code[i + 1];
                uint64_t currentRva = textRva + i;
                uint64_t callTarget = currentRva + 5 + rel;

                if (callTarget == targetRva) {
                    matchCount++;
                    printf("Match #%d: CALL from RVA 0x%08llX (VA 0x%016llX)\n",
                        matchCount, currentRva, nt.OptionalHeader.ImageBase + currentRva);

                    printf("  Post-Call Instructions (next 16 bytes): ");
                    for (int j = 5; j < 21 && (i + j < textSize); ++j) {
                        printf("%02X ", code[i + j]);
                    }
                    printf("\n");
                }
            }
        }
        printf("\nTotal matching calls found: %d\n", matchCount);
    } else {
        // Parse hex pattern
        std::vector<uint8_t> pattern;
        const char* p = query;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char* next = NULL;
            uint8_t b = (uint8_t)strtoul(p, &next, 16);
            if (next == p) break;
            pattern.push_back(b);
            p = next;
        }

        if (pattern.empty()) {
            printf("[ERROR] Invalid byte pattern\n");
            return 1;
        }

        printf("[SCANNER] Searching for %zu-byte pattern in code section...\n\n", pattern.size());
        int matchCount = 0;
        for (size_t i = 0; i <= textSize - pattern.size(); ++i) {
            if (memcmp(&code[i], pattern.data(), pattern.size()) == 0) {
                matchCount++;
                uint64_t currentRva = textRva + i;
                printf("Match #%d at RVA 0x%08llX (VA 0x%016llX)\n",
                    matchCount, currentRva, nt.OptionalHeader.ImageBase + currentRva);
            }
        }
        printf("\nTotal pattern matches found: %d\n", matchCount);
    }

    return 0;
}
