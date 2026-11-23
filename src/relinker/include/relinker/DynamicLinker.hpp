#pragma once

#include "ElfTypes.hpp"
#include "common/Nid.hpp"
#include <vector>
#include <string>
#include <span>
#include <unordered_map>

namespace Sailor {

struct RelocEntry {
    u64 offset{0};
    u32 type{0};
    u32 symbolIndex{0};
    i64 addend{0};
};

struct DynamicInfo {
    std::vector<std::string> neededLibraries;
    std::vector<RelocEntry> relocations;
    std::vector<RelocEntry> pltRelocations;
    std::vector<std::string> symbolNames;
    std::vector<Nid> symbolNids;
    std::vector<u64> symbolValues;
    std::vector<u64> initArrayFunctions;
    std::string soname;
};

class DynamicLinker {
public:
    DynamicLinker() = default;

    bool Parse(std::span<const u8> fileBytes,
               const Elf64_Phdr* dynamicPhdr,
               std::span<const Elf64_Phdr> allPhdrs);

    const DynamicInfo& GetInfo() const { return m_info; }

private:
    u64 VirtualToOffset(u64 vaddr, std::span<const Elf64_Phdr> phdrs) const;

    DynamicInfo m_info;
};

} // namespace Sailor
