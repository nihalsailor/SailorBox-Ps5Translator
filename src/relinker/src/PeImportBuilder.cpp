#include "relinker/PeImportBuilder.hpp"
#include <cstring>
#include <algorithm>

namespace Sailor {

#pragma pack(push, 1)

struct ImageImportDescriptor {
    u32 OriginalFirstThunk{0};
    u32 TimeDateStamp{0};
    u32 ForwarderChain{0};
    u32 Name{0};
    u32 FirstThunk{0};
};

#pragma pack(pop)

void PeImportBuilder::AddModule(std::string_view dllName, std::vector<std::string> functionNames) {
    PeImportModule mod;
    mod.dllName = dllName;
    for (auto& fn : functionNames) {
        PeImportFunction f;
        f.name = std::move(fn);
        f.hint = 0;
        mod.functions.push_back(std::move(f));
    }
    m_modules.push_back(std::move(mod));
}

BuiltPeImports PeImportBuilder::Build(u32 sectionBaseRva) const {
    BuiltPeImports result;
    if (m_modules.empty()) {
        return result;
    }

    std::vector<u8>& data = result.idataSectionData;

    // 1. Calculate space for Import Descriptors: (numModules + 1) * sizeof(ImageImportDescriptor)
    std::size_t numModules = m_modules.size();
    std::size_t descriptorsSize = (numModules + 1) * sizeof(ImageImportDescriptor);
    data.resize(descriptorsSize, 0);

    result.importDirectoryRva = sectionBaseRva;
    result.importDirectorySize = static_cast<u32>(descriptorsSize);

    // Track ILT and IAT offsets for each module
    std::vector<u32> iltOffsets(numModules);
    std::vector<u32> iatOffsets(numModules);

    u32 totalIatSize = 0;
    u32 firstIatRva = 0;

    for (std::size_t i = 0; i < numModules; ++i) {
        const auto& mod = m_modules[i];
        std::size_t thunkCount = mod.functions.size() + 1; // Null-terminated

        // Allocate ILT
        iltOffsets[i] = static_cast<u32>(data.size());
        data.resize(data.size() + thunkCount * sizeof(u64), 0);

        // Allocate IAT
        iatOffsets[i] = static_cast<u32>(data.size());
        if (firstIatRva == 0) {
            firstIatRva = sectionBaseRva + iatOffsets[i];
        }
        totalIatSize += static_cast<u32>(thunkCount * sizeof(u64));
        data.resize(data.size() + thunkCount * sizeof(u64), 0);
    }

    result.iatRva = firstIatRva;
    result.iatSize = totalIatSize;

    // Build Hint/Name entries and write thunks
    for (std::size_t i = 0; i < numModules; ++i) {
        const auto& mod = m_modules[i];

        for (std::size_t j = 0; j < mod.functions.size(); ++j) {
            const auto& fn = mod.functions[j];
            u32 nameEntryRva = sectionBaseRva + static_cast<u32>(data.size());

            // Write Hint (2 bytes)
            u16 hint = fn.hint;
            data.push_back(static_cast<u8>(hint & 0xFF));
            data.push_back(static_cast<u8>((hint >> 8) & 0xFF));

            // Write Name ASCII + null
            for (char c : fn.name) {
                data.push_back(static_cast<u8>(c));
            }
            data.push_back(0);

            // 2-byte alignment padding
            if (data.size() % 2 != 0) {
                data.push_back(0);
            }

            // Write 64-bit RVA pointer to ILT and IAT
            u64 thunkValue = nameEntryRva;
            std::size_t iltPos = iltOffsets[i] + j * sizeof(u64);
            std::size_t iatPos = iatOffsets[i] + j * sizeof(u64);

            std::memcpy(data.data() + iltPos, &thunkValue, sizeof(u64));
            std::memcpy(data.data() + iatPos, &thunkValue, sizeof(u64));

            u32 funcIatRva = sectionBaseRva + static_cast<u32>(iatPos);
            result.functionIatRvas[fn.name] = funcIatRva;
        }

        // Write DLL Name
        u32 dllNameRva = sectionBaseRva + static_cast<u32>(data.size());
        for (char c : mod.dllName) {
            data.push_back(static_cast<u8>(c));
        }
        data.push_back(0);
        if (data.size() % 2 != 0) {
            data.push_back(0);
        }

        // Fill in the ImageImportDescriptor
        ImageImportDescriptor desc{};
        desc.OriginalFirstThunk = sectionBaseRva + iltOffsets[i];
        desc.Name = dllNameRva;
        desc.FirstThunk = sectionBaseRva + iatOffsets[i];

        std::size_t descOffset = i * sizeof(ImageImportDescriptor);
        std::memcpy(data.data() + descOffset, &desc, sizeof(ImageImportDescriptor));
    }

    return result;
}

} // namespace Sailor
