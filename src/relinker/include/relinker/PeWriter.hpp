#pragma once

#include "common/Types.hpp"
#include "relinker/ElfReader.hpp"
#include <filesystem>
#include <vector>
#include <span>

namespace Sailor {

struct PeSection {
    std::string name;
    u32 characteristics{0};
    u32 virtualAddress{0};
    u32 virtualSize{0};
    std::vector<u8> rawData;
};

class PeWriter {
public:
    PeWriter();

    void AddSection(std::string_view name, u32 characteristics, u32 virtualAddress, u32 virtualSize, std::vector<u8> data);
    void SetEntryPointRva(u32 rva) { m_entryPointRva = rva; }
    void SetImageBase(u64 imageBase) { m_imageBase = imageBase; }
    void SetSubsystem(u16 subsystem) { m_subsystem = subsystem; }
    void SetExportDirectory(u32 rva, u32 size) { m_exportDirRva = rva; m_exportDirSize = size; }
    void SetImportDirectory(u32 rva, u32 size) { m_importDirRva = rva; m_importDirSize = size; }
    void SetIatDirectory(u32 rva, u32 size) { m_iatDirRva = rva; m_iatDirSize = size; }
    void SetIsDll(bool isDll) { m_isDll = isDll; }
    bool IsDll() const { return m_isDll; }

    bool PatchQword(u32 rva, u64 value);
    bool PatchDword(u32 rva, u32 value);
    bool PatchBytes(u32 rva, std::span<const u8> bytes);

    bool BuildAndSave(const std::filesystem::path& outputPath);

private:
    void MergeOverlappingSections();

    u64 m_imageBase{0x140000000ULL};
    u32 m_entryPointRva{0x1000};
    u16 m_subsystem{3}; // 3 = IMAGE_SUBSYSTEM_WINDOWS_CUI, 2 = IMAGE_SUBSYSTEM_WINDOWS_GUI
    u32 m_sectionAlignment{0x1000};
    u32 m_fileAlignment{0x200};
    u32 m_exportDirRva{0};
    u32 m_exportDirSize{0};
    u32 m_importDirRva{0};
    u32 m_importDirSize{0};
    u32 m_iatDirRva{0};
    u32 m_iatDirSize{0};
    bool m_isDll{false};
    std::vector<PeSection> m_sections;
};

} // namespace Sailor
