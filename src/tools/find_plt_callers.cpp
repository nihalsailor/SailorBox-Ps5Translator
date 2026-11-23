#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <unordered_map>

int main() {
    std::ifstream file("C:\\PS5\\World War Z Aftermath PS5 BASE GAME\\eboot.exe", std::ios::binary);
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    std::unordered_map<uint64_t, std::string> gotTargets = {
        { 0x142748500ULL, "zlfEH8FmyUA (Ampr)" },
        { 0x142748C60ULL, "rQUnqlwNRsI (Prospero)" },
        { 0x142748670ULL, "cRILAEvn+9M (NpCommerce)" },
        { 0x142748838ULL, "0HBYxYAjmf0 (NpEntitlement)" }
    };

    std::unordered_map<uint64_t, std::string> pltTargets;

    // Single pass to find PLT entries
    for (size_t i = 0x400; i + 6 <= 0x1E78400; ++i) {
        if (data[i] == 0xFF && data[i+1] == 0x25) {
            uint64_t instVa = 0x140001000ULL + (i - 0x400);
            int32_t disp = *reinterpret_cast<const int32_t*>(&data[i+2]);
            uint64_t gotVa = instVa + 6 + disp;
            auto it = gotTargets.find(gotVa);
            if (it != gotTargets.end()) {
                std::cout << it->second << " PLT at VA 0x" << std::hex << instVa << "\n";
                pltTargets[instVa] = it->second;
            }
        }
    }

    // Single pass to find callers of PLTs
    for (size_t i = 0x400; i + 5 <= 0x1E78400; ++i) {
        uint8_t op = data[i];
        if (op == 0xE8 || op == 0xE9) {
            uint64_t instVa = 0x140001000ULL + (i - 0x400);
            int32_t rel = *reinterpret_cast<const int32_t*>(&data[i+1]);
            uint64_t target = instVa + 5 + rel;
            auto it = pltTargets.find(target);
            if (it != pltTargets.end()) {
                std::cout << "  Caller of " << it->second << " at VA 0x" << std::hex << instVa << "\n";
            }
        }
    }

    return 0;
}
