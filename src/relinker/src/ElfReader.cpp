#include "relinker/ElfReader.hpp"
#include "common/Logger.hpp"
#include "common/Nid.hpp"
#include <fstream>
#include <cstring>

namespace Sailor {

bool ElfReader::Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        SB_LOG_ERROR("ElfReader", "Failed to open input file: {}", path.string());
        return false;
    }

    std::size_t size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<u8> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        SB_LOG_ERROR("ElfReader", "Failed to read binary data from: {}", path.string());
        return false;
    }

    return LoadFromMemory(buffer);
}

bool ElfReader::LoadFromMemory(std::span<const u8> bytes) {
    if (bytes.size() < 32) {
        SB_LOG_ERROR("ElfReader", "File size too small: {} bytes", bytes.size());
        return false;
    }

    // Check for PlayStation 5 (0xEEF51454) or PlayStation 4 (0x1D3D154F) SELF container magic
    static constexpr u8 PS5_SELF_MAGIC[4] = { 0x54, 0x14, 0xF5, 0xEE };
    static constexpr u8 PS4_SELF_MAGIC[4] = { 0x4F, 0x15, 0x3D, 0x1D };

    if (std::memcmp(bytes.data(), PS5_SELF_MAGIC, 4) == 0 || std::memcmp(bytes.data(), PS4_SELF_MAGIC, 4) == 0) {
        SB_LOG_INFO("ElfReader", "Detected PlayStation SELF container; unwrapping embedded ELF...");
        m_unwrappedBuffer = UnwrapSelf(bytes);
        if (m_unwrappedBuffer.empty()) {
            SB_LOG_ERROR("ElfReader", "Failed to unwrap PlayStation SELF container");
            return false;
        }
        return LoadFromMemory(m_unwrappedBuffer);
    }

    if (bytes.size() < sizeof(Elf64_Ehdr)) {
        SB_LOG_ERROR("ElfReader", "File size too small for ELF header: {} bytes", bytes.size());
        return false;
    }

    std::memcpy(&m_header, bytes.data(), sizeof(Elf64_Ehdr));

    if (std::memcmp(m_header.e_ident, ELF_MAGIC, 4) != 0) {
        SB_LOG_ERROR("ElfReader", "Invalid ELF magic: {:02X} {:02X} {:02X} {:02X}",
            m_header.e_ident[0], m_header.e_ident[1], m_header.e_ident[2], m_header.e_ident[3]);
        return false;
    }

    // e_ident[4] = 2 (64-bit), e_ident[5] = 1 (Little Endian)
    if (m_header.e_ident[4] != 2 || m_header.e_ident[5] != 1) {
        SB_LOG_ERROR("ElfReader", "Unsupported ELF class/endianness: class={}, endian={}",
            m_header.e_ident[4], m_header.e_ident[5]);
        return false;
    }

    // e_machine = 0x3E (x86-64 / AMD64)
    if (m_header.e_machine != 0x3E) {
        SB_LOG_ERROR("ElfReader", "Unsupported ELF architecture machine: 0x{:X} (expected x86-64)", m_header.e_machine);
        return false;
    }

    SB_LOG_INFO("ElfReader", "Parsed valid 64-bit x86-64 ELF (entry: 0x{:X}, type: 0x{:X})",
        m_header.e_entry, m_header.e_type);

    return ParseHeaders(bytes);
}

bool ElfReader::ParseHeaders(std::span<const u8> bytes) {
    if (m_header.e_phoff + m_header.e_phnum * sizeof(Elf64_Phdr) > bytes.size()) {
        SB_LOG_ERROR("ElfReader", "Program headers table exceeds file size");
        return false;
    }

    m_programHeaders.resize(m_header.e_phnum);
    std::memcpy(m_programHeaders.data(), bytes.data() + m_header.e_phoff, m_header.e_phnum * sizeof(Elf64_Phdr));

    m_segments.clear();
    m_tlsInfo = ElfTlsInfo{};
    for (const auto& phdr : m_programHeaders) {
        if (phdr.p_type == PT_LOAD) {
            ElfSegment seg;
            seg.flags = phdr.p_flags;
            seg.vaddr = phdr.p_vaddr;
            seg.memsz = phdr.p_memsz;

            if (phdr.p_filesz > 0 && phdr.p_offset + phdr.p_filesz <= bytes.size()) {
                seg.data.resize(phdr.p_filesz);
                std::memcpy(seg.data.data(), bytes.data() + phdr.p_offset, phdr.p_filesz);
            }
            m_segments.push_back(std::move(seg));

            SB_LOG_DEBUG("ElfReader", "Loaded PT_LOAD segment: vaddr=0x{:X}, memsz=0x{:X}, flags=0x{:X}",
                phdr.p_vaddr, phdr.p_memsz, phdr.p_flags);
        } else if (phdr.p_type == PT_TLS) {
            m_tlsInfo.hasTls = true;
            m_tlsInfo.vaddr = phdr.p_vaddr;
            m_tlsInfo.filesz = phdr.p_filesz;
            m_tlsInfo.memsz = phdr.p_memsz;
            m_tlsInfo.align = (phdr.p_align > 0) ? phdr.p_align : 16;
            if (phdr.p_filesz > 0 && phdr.p_offset + phdr.p_filesz <= bytes.size()) {
                m_tlsInfo.templateData.resize(phdr.p_filesz);
                std::memcpy(m_tlsInfo.templateData.data(), bytes.data() + phdr.p_offset, phdr.p_filesz);
            }
            SB_LOG_INFO("ElfReader", "Loaded PT_TLS segment: vaddr=0x{:X}, filesz=0x{:X}, memsz=0x{:X}, align=0x{:X}",
                phdr.p_vaddr, phdr.p_filesz, phdr.p_memsz, phdr.p_align);
        }
    }

    return ParseDynamic(bytes);
}

bool ElfReader::ParseDynamic(std::span<const u8> bytes) {
    m_imports.clear();
    // Scan for PT_DYNAMIC segment and parse with DynamicLinker
    for (const auto& phdr : m_programHeaders) {
        if (phdr.p_type == PT_DYNAMIC) {
            SB_LOG_INFO("ElfReader", "Found PT_DYNAMIC segment at offset 0x{:X}, size 0x{:X}",
                phdr.p_offset, phdr.p_filesz);
            if (!m_dynamicLinker.Parse(bytes, &phdr, m_programHeaders)) {
                return false;
            }
            const auto& info = m_dynamicLinker.GetInfo();
            std::string defaultMod = info.neededLibraries.empty() ? "libkernel" : info.neededLibraries.front();
            for (std::size_t i = 0; i < info.symbolNames.size(); ++i) {
                if (info.symbolNames[i].empty()) continue;
                if (i < info.symbolValues.size() && info.symbolValues[i] != 0) continue;
                ImportEntry imp;
                imp.moduleName = defaultMod;
                imp.symbolName = info.symbolNames[i];
                imp.nid = (i < info.symbolNids.size()) ? info.symbolNids[i] : Nid::Compute(imp.symbolName);
                m_imports.push_back(std::move(imp));
            }
            break;
        }
    }
    return true;
}

std::vector<u8> ElfReader::UnwrapSelf(std::span<const u8> bytes) {
    if (bytes.size() < 32) return {};

    u16 numEntries = *reinterpret_cast<const u16*>(bytes.data() + 24);
    u64 elfHeaderOffset = 32 + static_cast<u64>(numEntries) * 32;
    if (bytes.size() < elfHeaderOffset + sizeof(Elf64_Ehdr)) {
        SB_LOG_ERROR("ElfReader", "SELF container too small for ELF header at offset 0x{:X}", elfHeaderOffset);
        return {};
    }

    Elf64_Ehdr elfHdr{};
    std::memcpy(&elfHdr, bytes.data() + elfHeaderOffset, sizeof(Elf64_Ehdr));
    if (std::memcmp(elfHdr.e_ident, ELF_MAGIC, 4) != 0) {
        SB_LOG_ERROR("ElfReader", "Embedded SELF header did not contain valid ELF magic at offset 0x{:X}", elfHeaderOffset);
        return {};
    }

    u64 phoff = elfHdr.e_phoff;
    u16 phnum = elfHdr.e_phnum;
    u16 phentsize = elfHdr.e_phentsize;
    if (bytes.size() < elfHeaderOffset + phoff + static_cast<u64>(phnum) * phentsize) {
        SB_LOG_ERROR("ElfReader", "SELF container truncated before program headers");
        return {};
    }

    std::vector<Elf64_Phdr> phdrs(phnum);
    std::memcpy(phdrs.data(), bytes.data() + elfHeaderOffset + phoff, phnum * sizeof(Elf64_Phdr));

    u64 maxElfSize = 0;
    for (const auto& ph : phdrs) {
        if (ph.p_offset + ph.p_filesz > maxElfSize) {
            maxElfSize = ph.p_offset + ph.p_filesz;
        }
    }
    if (maxElfSize == 0) return {};

    std::vector<u8> elfBuffer(maxElfSize, 0);

    // Copy ELF header
    std::memcpy(elfBuffer.data(), bytes.data() + elfHeaderOffset, sizeof(Elf64_Ehdr));

    // Copy program headers
    if (phoff + phnum * sizeof(Elf64_Phdr) <= elfBuffer.size()) {
        std::memcpy(elfBuffer.data() + phoff, phdrs.data(), phnum * sizeof(Elf64_Phdr));
    }

    // Parse SELF segment entries table
    struct SelfEntry {
        u64 props;
        u64 offset;
        u64 encSize;
        u64 decSize;
    };

    const SelfEntry* entries = reinterpret_cast<const SelfEntry*>(bytes.data() + 32);
    for (u16 i = 0; i < numEntries; ++i) {
        const auto& e = entries[i];
        if (e.decSize == 0) continue;

        u16 segIndex = static_cast<u16>((e.props >> 20) & 0xFFFF);
        if (segIndex >= phnum) continue;

        const auto& ph = phdrs[segIndex];
        bool hasBlocks = ((e.props >> 11) & 1) != 0;
        if (hasBlocks || e.decSize == ph.p_filesz) {
            if (e.offset + e.decSize <= bytes.size() && ph.p_offset + e.decSize <= elfBuffer.size()) {
                std::memcpy(elfBuffer.data() + ph.p_offset, bytes.data() + e.offset, e.decSize);
            }
        }
    }

    SB_LOG_INFO("ElfReader", "Successfully unwrapped PlayStation 5 SELF container into native ELF ({} bytes, {} segments)",
        elfBuffer.size(), phnum);
    return elfBuffer;
}

} // namespace Sailor
