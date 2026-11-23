#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>
#include <span>
#include <string_view>

namespace Sailor::Tools {

enum class PatchType {
    U8,
    U16,
    U32,
    U64,
    Bytes
};

struct PatchEntry {
    std::string name;
    std::string titleId{"*"};
    u64 address{0};
    PatchType type{PatchType::U32};
    std::vector<u8> expected;
    std::vector<u8> replacement;
    std::string description;
    bool enabled{true};
};

class PatchEngine {
public:
    PatchEngine() = default;

    // Parses a TOML or custom patch file
    bool LoadFromFile(std::string_view filePath);

    // Parses raw TOML text content
    bool LoadFromString(std::string_view tomlContent);

    // Applies loaded patches to in-memory buffer
    // Returns number of successfully applied patches
    size_t ApplyPatches(std::span<u8> memory, u64 baseAddress = 0x140000000ULL, std::string_view titleId = "*");

    // Applies loaded patches directly to an executable file on disk
    size_t PatchFile(std::string_view targetPath, u64 baseAddress = 0x140000000ULL, std::string_view titleId = "*");

    // Adds a patch entry programmatically
    void AddPatch(PatchEntry entry);

    [[nodiscard]] const std::vector<PatchEntry>& GetPatches() const { return m_patches; }
    void Clear() { m_patches.clear(); }

private:
    std::vector<PatchEntry> m_patches;
};

} // namespace Sailor::Tools
