#include "relinker/PeWriter.hpp"
#include "common/Logger.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace Sailor {

#pragma pack(push, 1)

struct DosHeader {
    u16 e_magic{0x5A4D}; // 'MZ'
    u16 e_cblp{0x90};
    u16 e_cp{3};
    u16 e_crlc{0};
    u16 e_cparhdr{4};
    u16 e_minalloc{0};
    u16 e_maxalloc{0xFFFF};
    u16 e_ss{0};
    u16 e_sp{0xB8};
    u16 e_csum{0};
    u16 e_ip{0};
    u16 e_cs{0};
    u16 e_lfarlc{0x40};
    u16 e_ovno{0};
    u16 e_res[4]{0};
    u16 e_oemid{0};
    u16 e_oeminfo{0};
    u16 e_res2[10]{0};
    u32 e_lfanew{0x80}; // Offset to PE header
};

struct FileHeader {
    u16 Machine{0x8664}; // AMD64 / x86-64
    u16 NumberOfSections{0};
    u32 TimeDateStamp{0};
    u32 PointerToSymbolTable{0};
    u32 NumberOfSymbols{0};
    u16 SizeOfOptionalHeader{0};
    u16 Characteristics{0x0022}; // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
};

struct ImageDataDirectory {
    u32 VirtualAddress{0};
    u32 Size{0};
};

struct OptionalHeader64 {
    u16 Magic{0x020B}; // PE32+ (64-bit)
    u8  MajorLinkerVersion{14};
    u8  MinorLinkerVersion{0};
    u32 SizeOfCode{0};
    u32 SizeOfInitializedData{0};
    u32 SizeOfUninitializedData{0};
    u32 AddressOfEntryPoint{0};
    u32 BaseOfCode{0x1000};
    u64 ImageBase{0x140000000ULL};
    u32 SectionAlignment{0x1000};
    u32 FileAlignment{0x200};
    u16 MajorOperatingSystemVersion{6};
    u16 MinorOperatingSystemVersion{0};
    u16 MajorImageVersion{0};
    u16 MinorImageVersion{0};
    u16 MajorSubsystemVersion{6};
    u16 MinorSubsystemVersion{0};
    u32 Win32VersionValue{0};
    u32 SizeOfImage{0};
    u32 SizeOfHeaders{0};
    u32 CheckSum{0};
    u16 Subsystem{3}; // IMAGE_SUBSYSTEM_WINDOWS_CUI (Console)
    u16 DllCharacteristics{0x0100}; // IMAGE_DLLCHARACTERISTICS_NX_COMPAT (No ASLR)
    u64 SizeOfStackReserve{0x1000000}; // 16 MB Stack
    u64 SizeOfStackCommit{0x100000};   // 1 MB Commit
    u64 SizeOfHeapReserve{0x100000};
    u64 SizeOfHeapCommit{0x10000};
    u32 LoaderFlags{0};
    u32 NumberOfRvaAndSizes{16};
    ImageDataDirectory DataDirectory[16];
};

struct SectionHeader {
    u8  Name[8]{0};
    u32 VirtualSize{0};
    u32 VirtualAddress{0};
    u32 SizeOfRawData{0};
    u32 PointerToRawData{0};
    u32 PointerToRelocations{0};
    u32 PointerToLinenumbers{0};
    u16 NumberOfRelocations{0};
    u16 NumberOfLinenumbers{0};
    u32 Characteristics{0};
};

#pragma pack(pop)

namespace {
u32 AlignUp(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}
} // namespace

PeWriter::PeWriter() = default;

void PeWriter::AddSection(std::string_view name, u32 characteristics, u32 virtualAddress, u32 virtualSize, std::vector<u8> data) {
    PeSection section;
    section.name = name;
    section.characteristics = characteristics;
    section.virtualAddress = virtualAddress;
    section.virtualSize = virtualSize;
    section.rawData = std::move(data);
    m_sections.push_back(std::move(section));
}

void PeWriter::MergeOverlappingSections() {
    std::sort(m_sections.begin(), m_sections.end(), [](const PeSection& a, const PeSection& b) {
        return a.virtualAddress < b.virtualAddress;
    });

    std::vector<PeSection> merged;
    for (auto& sec : m_sections) {
        if (!merged.empty()) {
            auto& prev = merged.back();
            u32 prevAlignedEnd = AlignUp(prev.virtualAddress + prev.virtualSize, m_sectionAlignment);
            if (sec.virtualAddress < prevAlignedEnd) {
                u32 relOff = sec.virtualAddress - prev.virtualAddress;
                prev.virtualSize = std::max(prev.virtualSize, relOff + sec.virtualSize);
                if (!sec.rawData.empty()) {
                    if (prev.rawData.size() < relOff + sec.rawData.size()) {
                        prev.rawData.resize(relOff + sec.rawData.size(), 0);
                    }
                    for (std::size_t k = 0; k < sec.rawData.size(); ++k) {
                        if (sec.rawData[k] != 0) {
                            prev.rawData[relOff + k] = sec.rawData[k];
                        }
                    }
                }
                prev.characteristics |= sec.characteristics;
                continue;
            }
        }
        merged.push_back(std::move(sec));
    }
    m_sections = std::move(merged);
}

bool PeWriter::PatchQword(u32 rva, u64 value) {
    MergeOverlappingSections();
    for (auto& sec : m_sections) {
        if (rva >= sec.virtualAddress && rva + sizeof(u64) <= sec.virtualAddress + sec.virtualSize) {
            u32 offset = rva - sec.virtualAddress;
            if (offset + sizeof(u64) > sec.rawData.size()) {
                sec.rawData.resize(offset + sizeof(u64), 0);
            }
            std::memcpy(sec.rawData.data() + offset, &value, sizeof(value));
            return true;
        }
    }
    return false;
}

bool PeWriter::PatchDword(u32 rva, u32 value) {
    MergeOverlappingSections();
    for (auto& sec : m_sections) {
        if (rva >= sec.virtualAddress && rva + sizeof(u32) <= sec.virtualAddress + sec.virtualSize) {
            u32 offset = rva - sec.virtualAddress;
            if (offset + sizeof(u32) > sec.rawData.size()) {
                sec.rawData.resize(offset + sizeof(u32), 0);
            }
            std::memcpy(sec.rawData.data() + offset, &value, sizeof(value));
            return true;
        }
    }
    return false;
}

bool PeWriter::PatchBytes(u32 rva, std::span<const u8> bytes) {
    if (bytes.empty()) return true;
    MergeOverlappingSections();
    for (auto& sec : m_sections) {
        if (rva >= sec.virtualAddress && rva + bytes.size() <= sec.virtualAddress + sec.virtualSize) {
            u32 offset = rva - sec.virtualAddress;
            if (offset + bytes.size() > sec.rawData.size()) {
                sec.rawData.resize(offset + bytes.size(), 0);
            }
            std::memcpy(sec.rawData.data() + offset, bytes.data(), bytes.size());
            return true;
        }
    }
    return false;
}

bool PeWriter::BuildAndSave(const std::filesystem::path& outputPath) {
    // Ensure sections are sorted strictly by ascending VirtualAddress and overlapping pages merged
    MergeOverlappingSections();

    // Windows NT loader requires PE sections in memory to be contiguous without gaps.
    // If there is a gap between m_sections[i] and m_sections[i + 1], expand VirtualSize to bridge it,
    // and resize/zero-initialize rawData buffer up to the bridged VirtualSize so memory cannot contain garbage.
    for (std::size_t i = 0; i + 1 < m_sections.size(); ++i) {
        u32 currentEnd = m_sections[i].virtualAddress + m_sections[i].virtualSize;
        if (currentEnd < m_sections[i + 1].virtualAddress) {
            u32 bridgedSize = m_sections[i + 1].virtualAddress - m_sections[i].virtualAddress;
            m_sections[i].virtualSize = bridgedSize;
            if (!m_sections[i].rawData.empty() || (m_sections[i].characteristics & 0x60)) {
                if (m_sections[i].rawData.size() < bridgedSize) {
                    m_sections[i].rawData.resize(bridgedSize, 0);
                }
            }
        }
    }

    DosHeader dosHeader{};
    dosHeader.e_lfanew = 0x80;

    u32 peSignature = 0x00004550; // "PE\0\0"

    FileHeader fileHeader{};
    fileHeader.NumberOfSections = static_cast<u16>(m_sections.size());
    fileHeader.SizeOfOptionalHeader = sizeof(OptionalHeader64);
    if (m_isDll) {
        fileHeader.Characteristics |= 0x2000; // IMAGE_FILE_DLL
    }

    OptionalHeader64 optHeader{};
    optHeader.ImageBase = m_imageBase;
    optHeader.AddressOfEntryPoint = m_entryPointRva;
    optHeader.SectionAlignment = m_sectionAlignment;
    optHeader.FileAlignment = m_fileAlignment;
    optHeader.Subsystem = m_subsystem;

    // Set Data Directories
    if (m_exportDirRva > 0) {
        optHeader.DataDirectory[0].VirtualAddress = m_exportDirRva; // IMAGE_DIRECTORY_ENTRY_EXPORT
        optHeader.DataDirectory[0].Size = m_exportDirSize;
    }
    if (m_importDirRva > 0) {
        optHeader.DataDirectory[1].VirtualAddress = m_importDirRva; // IMAGE_DIRECTORY_ENTRY_IMPORT
        optHeader.DataDirectory[1].Size = m_importDirSize;
    }
    if (m_iatDirRva > 0) {
        optHeader.DataDirectory[12].VirtualAddress = m_iatDirRva;   // IMAGE_DIRECTORY_ENTRY_IAT
        optHeader.DataDirectory[12].Size = m_iatDirSize;
    }

    u32 headersSize = sizeof(DosHeader) + (dosHeader.e_lfanew - sizeof(DosHeader))
                    + sizeof(peSignature) + sizeof(FileHeader) + sizeof(OptionalHeader64)
                    + static_cast<u32>(m_sections.size() * sizeof(SectionHeader));
    optHeader.SizeOfHeaders = AlignUp(headersSize, m_fileAlignment);

    // Compute layout for sections
    u32 currentRawOffset = optHeader.SizeOfHeaders;
    u32 maxVirtualAddress = m_sectionAlignment;

    std::vector<SectionHeader> secHeaders(m_sections.size());
    for (std::size_t i = 0; i < m_sections.size(); ++i) {
        auto& src = m_sections[i];
        auto& dst = secHeaders[i];

        std::size_t copyLen = std::min(src.name.size(), std::size_t(8));
        std::memcpy(dst.Name, src.name.data(), copyLen);

        dst.VirtualSize = std::max<u32>({src.virtualSize, static_cast<u32>(src.rawData.size()), 1u});
        dst.VirtualAddress = src.virtualAddress ? src.virtualAddress : maxVirtualAddress;
        dst.SizeOfRawData = AlignUp(static_cast<u32>(src.rawData.size()), m_fileAlignment);
        dst.PointerToRawData = dst.SizeOfRawData > 0 ? currentRawOffset : 0;
        dst.Characteristics = src.characteristics;

        currentRawOffset += dst.SizeOfRawData;
        maxVirtualAddress = AlignUp(dst.VirtualAddress + dst.VirtualSize, m_sectionAlignment);
    }

    // Windows NT loader requires PE sections in memory to be contiguous without gaps.
    // If there is a gap between secHeaders[i] and secHeaders[i + 1], expand VirtualSize to bridge it,
    // and ensure rawData is zero-padded up to VirtualSize.
    for (std::size_t i = 0; i + 1 < secHeaders.size(); ++i) {
        u32 currentEnd = secHeaders[i].VirtualAddress + secHeaders[i].VirtualSize;
        if (currentEnd < secHeaders[i + 1].VirtualAddress) {
            u32 bridgedSize = secHeaders[i + 1].VirtualAddress - secHeaders[i].VirtualAddress;
            secHeaders[i].VirtualSize = bridgedSize;
            if (m_sections[i].rawData.size() < bridgedSize) {
                m_sections[i].rawData.resize(bridgedSize, 0);
            }
        }
    }

    if (!secHeaders.empty()) {
        optHeader.SizeOfImage = AlignUp(secHeaders.back().VirtualAddress + secHeaders.back().VirtualSize, m_sectionAlignment);
    } else {
        optHeader.SizeOfImage = maxVirtualAddress;
    }

    // Write PE file to disk
    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open()) {
        SB_LOG_ERROR("PeWriter", "Failed to open destination file for write: {}", outputPath.string());
        return false;
    }

    // Write DOS Header
    out.write(reinterpret_cast<const char*>(&dosHeader), sizeof(dosHeader));

    // Pad to e_lfanew
    std::vector<char> pad(dosHeader.e_lfanew - sizeof(dosHeader), 0);
    out.write(pad.data(), pad.size());

    // Write PE Signature + FileHeader + OptionalHeader64
    out.write(reinterpret_cast<const char*>(&peSignature), sizeof(peSignature));
    out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char*>(&optHeader), sizeof(optHeader));

    // Write Section Headers
    out.write(reinterpret_cast<const char*>(secHeaders.data()), secHeaders.size() * sizeof(SectionHeader));

    // Pad headers up to SizeOfHeaders
    std::size_t currentPos = static_cast<std::size_t>(out.tellp());
    if (currentPos < optHeader.SizeOfHeaders) {
        std::vector<char> headerPad(optHeader.SizeOfHeaders - currentPos, 0);
        out.write(headerPad.data(), headerPad.size());
    }

    // Write section contents
    for (std::size_t i = 0; i < m_sections.size(); ++i) {
        const auto& src = m_sections[i];
        const auto& hdr = secHeaders[i];

        if (hdr.SizeOfRawData > 0) {
            out.write(reinterpret_cast<const char*>(src.rawData.data()), src.rawData.size());
            if (hdr.SizeOfRawData > src.rawData.size()) {
                std::vector<char> secPad(hdr.SizeOfRawData - src.rawData.size(), 0);
                out.write(secPad.data(), secPad.size());
            }
        }
    }

    SB_LOG_INFO("PeWriter", "Successfully generated native Windows PE executable: {} (Size: {} bytes)",
        outputPath.string(), static_cast<u64>(out.tellp()));
    return true;
}

} // namespace Sailor
