#include "relinker/ElfReader.hpp"
#include "common/Nid.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace Sailor;

int main() {
    ElfReader reader;
    if (!reader.Load("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.bin")) {
        std::cerr << "Failed to load eboot.bin\n";
        return 1;
    }

    const auto& dyn = reader.GetDynamicLinker().GetInfo();
    std::unordered_set<std::string> ebootNids;
    for (const auto& sym : dyn.symbolNames) {
        std::string bare = sym;
        auto hashPos = bare.find('#');
        if (hashPos != std::string::npos) {
            bare = bare.substr(0, hashPos);
        }
        if (!bare.empty()) {
            ebootNids.insert(bare);
        }
    }

    auto checkNamesFile = [&](const std::string& path, const std::string& label) {
        std::ifstream file(path);
        if (!file.is_open()) return;
        std::string line;
        int matchCount = 0;
        std::cout << "=== Matching " << label << " ===\n";
        while (std::getline(file, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty()) continue;
            Nid nid = Nid::Compute(line);
            std::string enc = nid.ToString();
            if (ebootNids.count(enc)) {
                std::cout << "  MATCH: " << line << " -> NID " << enc << " (0x" << std::hex << nid.Value() << ")\n";
                matchCount++;
            }
        }
        std::cout << "Total matches in " << label << ": " << matchCount << "\n\n";
    };

    checkNamesFile("src/tools/video_names.txt", "video_names.txt");
    checkNamesFile("src/tools/agc_names.txt", "agc_names.txt");

    return 0;
}
