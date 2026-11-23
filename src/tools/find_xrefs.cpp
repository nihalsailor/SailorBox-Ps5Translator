#include <iostream>
#include <iomanip>
#include "relinker/ElfReader.hpp"

using namespace Sailor;

int main() {
    ElfReader reader;
    if (!reader.Load("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.bin")) return 1;

    const auto& seg = reader.GetSegments()[0];
    uint64_t target = 0xcf6fa9;

    std::cout << "Searching for references to 0x" << std::hex << target << "...\n";
    for (size_t i = 0; i < seg.data.size() - 5; ++i) {
        // Direct call E8 xx xx xx xx
        if (seg.data[i] == 0xe8 || seg.data[i] == 0xe9) {
            int32_t rel = *(int32_t*)&seg.data[i + 1];
            uint64_t dest = seg.vaddr + i + 5 + rel;
            if (dest == target) {
                std::cout << "Call/Jmp at 0x" << std::hex << (seg.vaddr + i) << " -> 0x" << dest << "\n";
            }
        }
    }
    // Also search for 8-byte pointers
    for (size_t i = 0; i < seg.data.size() - 8; ++i) {
        uint64_t ptr = *(uint64_t*)&seg.data[i];
        if (ptr == target) {
            std::cout << "Pointer at 0x" << std::hex << (seg.vaddr + i) << " -> 0x" << ptr << "\n";
        }
    }
    return 0;
}
