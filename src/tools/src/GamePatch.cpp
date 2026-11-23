#include "tools/GamePatch.hpp"
#include "common/Logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cstring>

namespace Sailor::Tools {

namespace {

std::string Trim(std::string_view s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return "";
    auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

u64 ParseHexOrDec(std::string_view s) {
    std::string str = Trim(s);
    if (str.starts_with("0x") || str.starts_with("0X")) {
        return std::stoull(str.substr(2), nullptr, 16);
    }
    return std::stoull(str, nullptr, 10);
}

std::vector<u8> ParseHexByteString(std::string_view s) {
    std::vector<u8> bytes;
    std::string str(s);
    std::stringstream ss(str);
    std::string token;
    while (ss >> token) {
        if (!token.empty() && token[0] != '#') {
            try {
                u8 b = static_cast<u8>(std::stoul(token, nullptr, 16));
                bytes.push_back(b);
            } catch (...) {}
        }
    }
    return bytes;
}

std::vector<u8> ValueToBytes(u64 val, PatchType type) {
    std::vector<u8> b;
    switch (type) {
        case PatchType::U8:
            b.resize(1);
            b[0] = static_cast<u8>(val);
            break;
        case PatchType::U16:
            b.resize(2);
            std::memcpy(b.data(), &val, 2);
            break;
        case PatchType::U32:
            b.resize(4);
            std::memcpy(b.data(), &val, 4);
            break;
        case PatchType::U64:
            b.resize(8);
            std::memcpy(b.data(), &val, 8);
            break;
        default:
            break;
    }
    return b;
}

} // anonymous namespace

bool PatchEngine::LoadFromFile(std::string_view filePath) {
    std::string path(filePath);
    std::ifstream file(path);
    if (!file) {
        SB_LOG_ERROR("GamePatch", "Could not open patch file: {}", filePath);
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return LoadFromString(buffer.str());
}

bool PatchEngine::LoadFromString(std::string_view tomlContent) {
    std::string content(tomlContent);
    std::istringstream stream(content);
    std::string line;

    PatchEntry currentEntry;
    bool inEntry = false;

    auto CommitEntry = [&]() {
        if (inEntry && !currentEntry.name.empty() && !currentEntry.replacement.empty()) {
            m_patches.push_back(currentEntry);
        }
        currentEntry = PatchEntry{};
        inEntry = false;
    };

    while (std::getline(stream, line)) {
        std::string s = Trim(line);
        if (s.empty() || s.starts_with("#")) {
            continue;
        }

        // Section header e.g. [patches.60fps_unlock]
        if (s.starts_with("[") && s.ends_with("]")) {
            CommitEntry();
            std::string section = s.substr(1, s.size() - 2);
            auto dotPos = section.find('.');
            if (dotPos != std::string::npos) {
                currentEntry.name = section.substr(dotPos + 1);
            } else {
                currentEntry.name = section;
            }
            inEntry = true;
            continue;
        }

        // Key = Value
        auto eqPos = s.find('=');
        if (eqPos != std::string::npos && inEntry) {
            std::string key = Trim(s.substr(0, eqPos));
            std::string rawVal = s.substr(eqPos + 1);
            bool inQuotes = false;
            for (std::size_t i = 0; i < rawVal.size(); ++i) {
                if (rawVal[i] == '"') inQuotes = !inQuotes;
                else if (rawVal[i] == '#' && !inQuotes) {
                    rawVal.resize(i);
                    break;
                }
            }
            std::string valStr = Trim(rawVal);

            // Strip quotes
            if (valStr.starts_with("\"") && valStr.ends_with("\"") && valStr.size() >= 2) {
                valStr = valStr.substr(1, valStr.size() - 2);
            }

            if (key == "title_id" || key == "titleId") {
                currentEntry.titleId = valStr;
            } else if (key == "address") {
                currentEntry.address = ParseHexOrDec(valStr);
            } else if (key == "type") {
                if (valStr == "u8") currentEntry.type = PatchType::U8;
                else if (valStr == "u16") currentEntry.type = PatchType::U16;
                else if (valStr == "u32") currentEntry.type = PatchType::U32;
                else if (valStr == "u64") currentEntry.type = PatchType::U64;
                else if (valStr == "bytes") currentEntry.type = PatchType::Bytes;
            } else if (key == "expected") {
                if (currentEntry.type == PatchType::Bytes) {
                    currentEntry.expected = ParseHexByteString(valStr);
                } else {
                    u64 num = ParseHexOrDec(valStr);
                    currentEntry.expected = ValueToBytes(num, currentEntry.type);
                }
            } else if (key == "value" || key == "replacement") {
                if (currentEntry.type == PatchType::Bytes) {
                    currentEntry.replacement = ParseHexByteString(valStr);
                } else {
                    u64 num = ParseHexOrDec(valStr);
                    currentEntry.replacement = ValueToBytes(num, currentEntry.type);
                }
            } else if (key == "description") {
                currentEntry.description = valStr;
            } else if (key == "enabled") {
                currentEntry.enabled = (valStr == "true" || valStr == "1");
            }
        }
    }

    CommitEntry();
    SB_LOG_INFO("GamePatch", "Loaded {} game patches successfully", m_patches.size());
    return !m_patches.empty();
}

void PatchEngine::AddPatch(PatchEntry entry) {
    m_patches.push_back(std::move(entry));
}

size_t PatchEngine::ApplyPatches(std::span<u8> memory, u64 baseAddress, std::string_view titleId) {
    size_t appliedCount = 0;

    for (const auto& patch : m_patches) {
        if (!patch.enabled) {
            continue;
        }

        // Title ID filter
        if (patch.titleId != "*" && titleId != "*" && patch.titleId != titleId) {
            continue;
        }

        u64 offset = patch.address;
        if (offset >= baseAddress) {
            offset -= baseAddress;
        }

        // If memory is an unmapped PE file on disk, translate RVA -> PointerToRawData
        if (memory.size() >= 0x100 && memory[0] == 'M' && memory[1] == 'Z') {
            u32 peOff = *reinterpret_cast<const u32*>(memory.data() + 0x3C);
            if (peOff + 24 + 240 <= memory.size() &&
                memory[peOff] == 'P' && memory[peOff + 1] == 'E') {
                u16 numSecs = *reinterpret_cast<const u16*>(memory.data() + peOff + 6);
                u16 optSize = *reinterpret_cast<const u16*>(memory.data() + peOff + 20);
                u32 sizeOfHeaders = *reinterpret_cast<const u32*>(memory.data() + peOff + 24 + 60);
                std::size_t secTableOff = peOff + 24 + optSize;
                for (u16 s = 0; s < numSecs && secTableOff + (s + 1) * 40 <= memory.size(); ++s) {
                    const u8* sh = memory.data() + secTableOff + s * 40;
                    u32 vSize = *reinterpret_cast<const u32*>(sh + 8);
                    u32 vAddr = *reinterpret_cast<const u32*>(sh + 12);
                    u32 rawSize = *reinterpret_cast<const u32*>(sh + 16);
                    u32 rawPtr = *reinterpret_cast<const u32*>(sh + 20);
                    // Only translate if the buffer is in raw file layout (rawPtr < vAddr)
                    if (sizeOfHeaders < vAddr && offset >= vAddr && offset < vAddr + std::max(vSize, rawSize)) {
                        offset = rawPtr + (offset - vAddr);
                        break;
                    }
                }
            }
        }

        if (offset + patch.replacement.size() > memory.size()) {
            SB_LOG_WARN("GamePatch", "Patch '{}' at 0x{:X} exceeds memory size (0x{:X}), skipping",
                patch.name, patch.address, memory.size());
            continue;
        }

        // Validate expected bytes if specified
        if (!patch.expected.empty()) {
            if (offset + patch.expected.size() > memory.size() ||
                std::memcmp(memory.data() + offset, patch.expected.data(), patch.expected.size()) != 0) {
                SB_LOG_WARN("GamePatch", "Patch '{}' expected bytes mismatch at 0x{:X}, aborting patch to prevent corruption",
                    patch.name, patch.address);
                continue;
            }
        }

        // Apply replacement bytes
        std::memcpy(memory.data() + offset, patch.replacement.data(), patch.replacement.size());
        appliedCount++;

        SB_LOG_INFO("GamePatch", "Applied patch '{}' at 0x{:X} ({} bytes): {}",
            patch.name, patch.address, patch.replacement.size(), patch.description);
    }

    return appliedCount;
}

size_t PatchEngine::PatchFile(std::string_view targetPath, u64 baseAddress, std::string_view titleId) {
    std::fstream file(std::string(targetPath), std::ios::in | std::ios::out | std::ios::binary);
    if (!file) {
        SB_LOG_ERROR("GamePatch", "Cannot open target file for patching: {}", targetPath);
        return 0;
    }

    file.seekg(0, std::ios::end);
    const size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<u8> buffer(fileSize);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

    const size_t applied = ApplyPatches(buffer, baseAddress, titleId);
    if (applied > 0) {
        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<const char*>(buffer.data()), fileSize);
        SB_LOG_INFO("GamePatch", "Successfully patched file '{}' with {} patches", targetPath, applied);
    }

    return applied;
}

} // namespace Sailor::Tools
