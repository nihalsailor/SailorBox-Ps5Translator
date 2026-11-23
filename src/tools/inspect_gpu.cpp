#include "relinker/ElfReader.hpp"
#include <iostream>
#include <iomanip>

using namespace Sailor;

int main() {
    ElfReader reader;
    if (!reader.Load("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.bin")) {
        return 1;
    }

    const auto& dyn = reader.GetDynamicLinker().GetInfo();
    std::cout << "Imported libraries in eboot.bin:\n";
    for (const auto& lib : dyn.neededLibraries) {
        std::cout << "  " << lib << "\n";
    }

    std::cout << "\nVideoOut symbols in eboot.bin:\n";
    for (size_t i = 0; i < dyn.symbolNames.size(); ++i) {
        if (dyn.symbolNames[i].find("VideoOut") != std::string::npos) {
            std::cout << "  [" << i << "] " << dyn.symbolNames[i] << " (NID: 0x" 
                      << std::hex << (i < dyn.symbolNids.size() ? dyn.symbolNids[i].Value() : 0) << ")\n";
        }
    }
    return 0;
}
