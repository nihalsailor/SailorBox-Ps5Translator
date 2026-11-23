#include "relinker/DynamicLinker.hpp"
#include "common/Logger.hpp"
#include <cstring>

namespace Sailor {

namespace {

constexpr i64 DT_NULL     = 0;
constexpr i64 DT_NEEDED   = 1;
constexpr i64 DT_PLTRELSZ = 2;
constexpr i64 DT_STRTAB   = 5;
constexpr i64 DT_SYMTAB   = 6;
constexpr i64 DT_RELA     = 7;
constexpr i64 DT_RELASZ   = 8;
constexpr i64 DT_RELAENT  = 9;
constexpr i64 DT_STRSZ    = 10;
constexpr i64 DT_SYMENT   = 11;
constexpr i64 DT_SONAME   = 14;
constexpr i64 DT_JMPREL   = 23;

} // namespace

u64 DynamicLinker::VirtualToOffset(u64 vaddr, std::span<const Elf64_Phdr> phdrs) const {
    // 1. If PT_SCE_PROGLIB (0x61000000) exists, check it first for DT_SCE_* relative offsets!
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_SCE_PROGLIB && vaddr < phdr.p_filesz) {
            return phdr.p_offset + vaddr;
        }
    }
    // 2. Otherwise match standard PT_LOAD segments
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_LOAD && vaddr >= phdr.p_vaddr && vaddr < phdr.p_vaddr + phdr.p_memsz) {
            return phdr.p_offset + (vaddr - phdr.p_vaddr);
        }
    }
    return 0;
}

bool DynamicLinker::Parse(std::span<const u8> fileBytes,
                          const Elf64_Phdr* dynamicPhdr,
                          std::span<const Elf64_Phdr> allPhdrs) {
    m_info = DynamicInfo{};

    if (!dynamicPhdr || dynamicPhdr->p_offset + dynamicPhdr->p_filesz > fileBytes.size()) {
        SB_LOG_ERROR("DynamicLinker", "Invalid or out-of-bounds PT_DYNAMIC header");
        return false;
    }

    std::size_t numEntries = dynamicPhdr->p_filesz / sizeof(Elf64_Dyn);
    const auto* dynEntries = reinterpret_cast<const Elf64_Dyn*>(fileBytes.data() + dynamicPhdr->p_offset);

    u64 strtabVaddr = 0;
    u64 strtabSize = 0;
    u64 symtabVaddr = 0;
    u64 symtabEntSize = sizeof(Elf64_Sym);
    u64 symtabSize = 0;
    u64 relaVaddr = 0;
    u64 relaSize = 0;
    u64 jmprelVaddr = 0;
    u64 jmprelSize = 0;
    u64 initArrayVaddr = 0;
    u64 initArraySize = 0;

    bool hasStrtab = false;
    bool hasSymtab = false;
    bool hasRela = false;
    bool hasJmprel = false;

    std::vector<u64> neededOffsets;

    for (std::size_t i = 0; i < numEntries; ++i) {
        const auto& entry = dynEntries[i];
        if (entry.d_tag == DT_NULL) break;

        switch (entry.d_tag) {
            case DT_NEEDED:
            case DT_SCE_NEEDED:
                neededOffsets.push_back(entry.d_un.d_val);
                break;
            case DT_STRTAB:
            case DT_SCE_STRTAB:
                strtabVaddr = entry.d_un.d_ptr;
                hasStrtab = true;
                break;
            case DT_STRSZ:
            case DT_SCE_STRSZ:
                strtabSize = entry.d_un.d_val;
                break;
            case DT_SYMTAB:
            case DT_SCE_SYMTAB:
                symtabVaddr = entry.d_un.d_ptr;
                hasSymtab = true;
                break;
            case DT_SYMENT:
            case DT_SCE_SYMENT:
                symtabEntSize = entry.d_un.d_val;
                break;
            case DT_SCE_SYMTABSZ:
                symtabSize = entry.d_un.d_val;
                break;
            case DT_RELA:
            case DT_SCE_RELA:
                relaVaddr = entry.d_un.d_ptr;
                hasRela = true;
                break;
            case DT_RELASZ:
            case DT_SCE_RELASZ:
                relaSize = entry.d_un.d_val;
                break;
            case DT_JMPREL:
            case DT_SCE_JMPREL:
                jmprelVaddr = entry.d_un.d_ptr;
                hasJmprel = true;
                break;
            case DT_PLTRELSZ:
            case DT_SCE_PLTRELSZ:
                jmprelSize = entry.d_un.d_val;
                break;
            case DT_INIT_ARRAY:
                initArrayVaddr = entry.d_un.d_ptr;
                break;
            case DT_INIT_ARRAYSZ:
                initArraySize = entry.d_un.d_val;
                break;
            default:
                break;
        }
    }

    u64 strtabOffset = VirtualToOffset(strtabVaddr, allPhdrs);
    if (!hasStrtab || !strtabOffset || strtabOffset + strtabSize > fileBytes.size()) {
        SB_LOG_WARN("DynamicLinker", "Could not locate valid string table in dynamic section");
        return true;
    }

    const char* strtab = reinterpret_cast<const char*>(fileBytes.data() + strtabOffset);

    // Resolve DT_NEEDED library names
    m_info.neededLibraries.clear();
    for (u64 nameOffset : neededOffsets) {
        if (nameOffset < strtabSize) {
            std::string libName = strtab + nameOffset;
            m_info.neededLibraries.push_back(libName);
            SB_LOG_DEBUG("DynamicLinker", "Found needed library: {}", libName);
        }
    }

    // Parse Relocations (DT_RELA)
    if (hasRela && relaSize) {
        u64 relaOffset = VirtualToOffset(relaVaddr, allPhdrs);
        if (relaOffset && relaOffset + relaSize <= fileBytes.size()) {
            std::size_t count = relaSize / sizeof(Elf64_Rela);
            const auto* relas = reinterpret_cast<const Elf64_Rela*>(fileBytes.data() + relaOffset);
            for (std::size_t i = 0; i < count; ++i) {
                RelocEntry r;
                r.offset = relas[i].r_offset;
                r.type = static_cast<u32>(relas[i].r_info & 0xFFFFFFFF);
                r.symbolIndex = static_cast<u32>(relas[i].r_info >> 32);
                r.addend = relas[i].r_addend;
                m_info.relocations.push_back(r);
            }
            SB_LOG_DEBUG("DynamicLinker", "Parsed {} data relocations", count);
        }
    }

    // Parse PLT Relocations (DT_JMPREL)
    if (hasJmprel && jmprelSize) {
        u64 jmprelOffset = VirtualToOffset(jmprelVaddr, allPhdrs);
        if (jmprelOffset && jmprelOffset + jmprelSize <= fileBytes.size()) {
            std::size_t count = jmprelSize / sizeof(Elf64_Rela);
            const auto* relas = reinterpret_cast<const Elf64_Rela*>(fileBytes.data() + jmprelOffset);
            for (std::size_t i = 0; i < count; ++i) {
                RelocEntry r;
                r.offset = relas[i].r_offset;
                r.type = static_cast<u32>(relas[i].r_info & 0xFFFFFFFF);
                r.symbolIndex = static_cast<u32>(relas[i].r_info >> 32);
                r.addend = relas[i].r_addend;
                m_info.pltRelocations.push_back(r);
            }
            SB_LOG_DEBUG("DynamicLinker", "Parsed {} PLT jump relocations", count);
        }
    }

    // Parse Symbol Table (DT_SYMTAB)
    if (hasSymtab) {
        u64 symtabOffset = VirtualToOffset(symtabVaddr, allPhdrs);
        if (symtabOffset && symtabOffset < fileBytes.size()) {
            std::size_t maxSymbols = 0;
            std::size_t maxRelocSym = 0;
            for (const auto& rel : m_info.relocations) {
                maxRelocSym = std::max(maxRelocSym, static_cast<std::size_t>(rel.symbolIndex));
            }
            for (const auto& rel : m_info.pltRelocations) {
                maxRelocSym = std::max(maxRelocSym, static_cast<std::size_t>(rel.symbolIndex));
            }

            if (symtabSize > 0 && symtabOffset + symtabSize <= fileBytes.size()) {
                maxSymbols = static_cast<std::size_t>(symtabSize / symtabEntSize);
            } else {
                if (maxRelocSym > 0) {
                    maxSymbols = std::min((fileBytes.size() - symtabOffset) / symtabEntSize, maxRelocSym + 1024);
                } else {
                    maxSymbols = (fileBytes.size() - symtabOffset) / symtabEntSize;
                }
            }

            const auto* syms = reinterpret_cast<const Elf64_Sym*>(fileBytes.data() + symtabOffset);
            for (std::size_t i = 0; i < maxSymbols; ++i) {
                if (syms[i].st_name >= strtabSize) break;
                // If symtab size is unknown and past all known relocation symbol references,
                // a completely zeroed symbol indicates end of symbol table
                if (symtabSize == 0 && i > maxRelocSym &&
                    syms[i].st_name == 0 && syms[i].st_value == 0 &&
                    syms[i].st_size == 0 && syms[i].st_info == 0 && syms[i].st_shndx == 0) {
                    break;
                }
                const char* symName = strtab + syms[i].st_name;
                if (!symName || !*symName) {
                    m_info.symbolNames.emplace_back("");
                    m_info.symbolNids.emplace_back(0);
                    m_info.symbolValues.emplace_back(syms[i].st_value);
                    continue;
                }
                m_info.symbolNames.emplace_back(symName);
                m_info.symbolValues.emplace_back(syms[i].st_value);
                std::string_view symView(symName);
                auto hashPos = symView.find('#');
                std::string_view nidPart = (hashPos != std::string_view::npos) ? symView.substr(0, hashPos) : symView;
                try {
                    if (nidPart.size() == 11) {
                        m_info.symbolNids.emplace_back(Nid(nidPart));
                    } else {
                        m_info.symbolNids.emplace_back(Nid::Compute(nidPart));
                    }
                } catch (...) {
                    m_info.symbolNids.emplace_back(Nid::Compute(nidPart));
                }
            }
        }
    }

    if (initArrayVaddr > 0 && initArraySize > 0) {
        u64 initArrayOffset = VirtualToOffset(initArrayVaddr, allPhdrs);
        if (initArrayOffset && initArrayOffset + initArraySize <= fileBytes.size()) {
            std::size_t count = initArraySize / sizeof(u64);
            const auto* fns = reinterpret_cast<const u64*>(fileBytes.data() + initArrayOffset);
            m_info.initArrayFunctions.assign(fns, fns + count);
            SB_LOG_INFO("DynamicLinker", "Extracted {} DT_INIT_ARRAY static constructors at vaddr 0x{:X}",
                count, initArrayVaddr);
        }
    }

    SB_LOG_INFO("DynamicLinker", "Dynamic section analysis complete: {} libraries, {} symbols, {} PLT imports",
        m_info.neededLibraries.size(), m_info.symbolNames.size(), m_info.pltRelocations.size());

    return true;
}

} // namespace Sailor
