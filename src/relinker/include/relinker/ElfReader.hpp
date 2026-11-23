#pragma once

#include "ElfTypes.hpp"
#include "DynamicLinker.hpp"
#include "common/Nid.hpp"
#include <vector>
#include <string>
#include <span>
#include <filesystem>

namespace Sailor {

struct ElfSegment {
    u32 flags{0};
    u64 vaddr{0};
    u64 memsz{0};
    std::vector<u8> data;
};

struct ImportEntry {
    std::string moduleName;
    Nid nid;
    std::string symbolName;
};

struct ElfTlsInfo {
    bool hasTls{false};
    u64 vaddr{0};
    u64 filesz{0};
    u64 memsz{0};
    u64 align{16};
    std::vector<u8> templateData;
};

class ElfReader {
public:
    ElfReader() = default;

    bool Load(const std::filesystem::path& path);
    bool LoadFromMemory(std::span<const u8> bytes);

    u64 GetEntryPoint() const { return m_header.e_entry; }
    u16 GetType() const { return m_header.e_type; }

    const std::vector<ElfSegment>& GetSegments() const { return m_segments; }
    const std::vector<ImportEntry>& GetImports() const { return m_imports; }
    const DynamicLinker& GetDynamicLinker() const { return m_dynamicLinker; }
    const ElfTlsInfo& GetTlsInfo() const { return m_tlsInfo; }

private:
    bool ParseHeaders(std::span<const u8> bytes);
    bool ParseDynamic(std::span<const u8> bytes);
    std::vector<u8> UnwrapSelf(std::span<const u8> bytes);

    Elf64_Ehdr m_header{};
    std::vector<Elf64_Phdr> m_programHeaders;
    std::vector<ElfSegment> m_segments;
    std::vector<ImportEntry> m_imports;
    ElfTlsInfo m_tlsInfo{};
    DynamicLinker m_dynamicLinker;
    std::vector<u8> m_unwrappedBuffer;
};

} // namespace Sailor
