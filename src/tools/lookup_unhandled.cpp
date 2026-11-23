#include "relinker/ElfReader.hpp"
#include "common/Nid.hpp"
#include <iostream>
#include <fstream>
#include <unordered_map>

using namespace Sailor;

int main() {
    ElfReader reader;
    if (!reader.Load("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.bin")) return 1;

    std::unordered_map<std::string, std::string> videoNames;
    std::ifstream f("src/tools/video_names.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            videoNames[Nid::Compute(line).ToString()] = line;
        }
    }

    const auto& dyn = reader.GetDynamicLinker().GetInfo();
    std::cout << "=== All VideoOut / Display Symbols ===\n";
    for (size_t i = 0; i < dyn.symbolNames.size(); ++i) {
        std::string raw = dyn.symbolNames[i];
        if (raw.find("#d#d") != std::string::npos || raw.find("VideoOut") != std::string::npos) {
            std::cout << "Sym [" << i << "]: " << raw << "\n";
        }
    }
    return 0;
}
