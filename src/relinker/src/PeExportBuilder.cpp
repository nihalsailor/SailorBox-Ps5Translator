#include "relinker/PeExportBuilder.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace Sailor {

#pragma pack(push, 1)
struct ImageExportDirectory {
    u32 Characteristics{0};
    u32 TimeDateStamp{0};
    u16 MajorVersion{0};
    u16 MinorVersion{0};
    u32 Name{0};
    u32 Base{1};
    u32 NumberOfFunctions{0};
    u32 NumberOfNames{0};
    u32 AddressOfFunctions{0};
    u32 AddressOfNames{0};
    u32 AddressOfNameOrdinals{0};
};
#pragma pack(pop)

void PeExportBuilder::AddExport(std::string_view name, u32 functionRva) {
    if (name.empty() || functionRva == 0) return;
    PeExportEntry entry;
    entry.name = std::string(name);
    entry.functionRva = functionRva;
    m_exports.push_back(std::move(entry));
}

BuiltPeExports PeExportBuilder::Build(u32 sectionBaseRva) const {
    BuiltPeExports result{};
    if (m_exports.empty()) {
        return result;
    }

    // Deduplicate exports by name and sort alphabetically for Windows PE binary search
    std::vector<PeExportEntry> sortedExports;
    std::unordered_set<std::string> seenNames;
    for (const auto& exp : m_exports) {
        if (seenNames.insert(exp.name).second) {
            sortedExports.push_back(exp);
        }
    }

    std::sort(sortedExports.begin(), sortedExports.end(), [](const PeExportEntry& a, const PeExportEntry& b) {
        return a.name < b.name;
    });

    const u32 count = static_cast<u32>(sortedExports.size());

    // Layout:
    // 0: ImageExportDirectory (40 bytes)
    // 40: AddressOfFunctions (count * sizeof(u32))
    // offNames: AddressOfNames (count * sizeof(u32))
    // offOrdinals: AddressOfNameOrdinals (count * sizeof(u16))
    // offStrings: ASCII strings (m_moduleName, then sorted names)

    u32 offFunctions = sizeof(ImageExportDirectory);
    u32 offNames = offFunctions + count * sizeof(u32);
    u32 offOrdinals = offNames + count * sizeof(u32);
    u32 offStrings = offOrdinals + count * sizeof(u16);
    offStrings = (offStrings + 3) & ~3u; // 4-byte align

    // Precalculate string offsets
    u32 moduleNameOffset = offStrings;
    u32 currentStringOffset = moduleNameOffset + static_cast<u32>(m_moduleName.size() + 1);

    std::vector<u32> nameOffsets(count);
    for (u32 i = 0; i < count; ++i) {
        nameOffsets[i] = currentStringOffset;
        currentStringOffset += static_cast<u32>(sortedExports[i].name.size() + 1);
    }

    u32 totalSize = (currentStringOffset + 15u) & ~15u; // 16-byte align
    result.edataSectionData.resize(totalSize, 0);

    // Build directory header
    ImageExportDirectory dir{};
    dir.Characteristics = 0;
    dir.TimeDateStamp = 0;
    dir.MajorVersion = 0;
    dir.MinorVersion = 0;
    dir.Name = sectionBaseRva + moduleNameOffset;
    dir.Base = 1; // Ordinal base 1
    dir.NumberOfFunctions = count;
    dir.NumberOfNames = count;
    dir.AddressOfFunctions = sectionBaseRva + offFunctions;
    dir.AddressOfNames = sectionBaseRva + offNames;
    dir.AddressOfNameOrdinals = sectionBaseRva + offOrdinals;

    std::memcpy(result.edataSectionData.data(), &dir, sizeof(dir));

    // Fill AddressOfFunctions
    u32* pFunctions = reinterpret_cast<u32*>(result.edataSectionData.data() + offFunctions);
    for (u32 i = 0; i < count; ++i) {
        pFunctions[i] = sortedExports[i].functionRva;
    }

    // Fill AddressOfNames
    u32* pNames = reinterpret_cast<u32*>(result.edataSectionData.data() + offNames);
    for (u32 i = 0; i < count; ++i) {
        pNames[i] = sectionBaseRva + nameOffsets[i];
    }

    // Fill AddressOfNameOrdinals (0-based index into AddressOfFunctions)
    u16* pOrdinals = reinterpret_cast<u16*>(result.edataSectionData.data() + offOrdinals);
    for (u32 i = 0; i < count; ++i) {
        pOrdinals[i] = static_cast<u16>(i);
    }

    // Write module name string
    std::memcpy(result.edataSectionData.data() + moduleNameOffset, m_moduleName.c_str(), m_moduleName.size() + 1);

    // Write export name strings
    for (u32 i = 0; i < count; ++i) {
        std::memcpy(result.edataSectionData.data() + nameOffsets[i], sortedExports[i].name.c_str(), sortedExports[i].name.size() + 1);
    }

    result.exportDirectoryRva = sectionBaseRva;
    result.exportDirectorySize = totalSize;

    return result;
}

} // namespace Sailor
