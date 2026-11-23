#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>
#include <string_view>

namespace Sailor {

struct PeExportEntry {
    std::string name;
    u32 functionRva{0};
};

struct BuiltPeExports {
    std::vector<u8> edataSectionData;
    u32 exportDirectoryRva{0};
    u32 exportDirectorySize{0};
};

class PeExportBuilder {
public:
    PeExportBuilder() = default;

    void SetModuleName(std::string_view moduleName) { m_moduleName = moduleName; }
    void AddExport(std::string_view name, u32 functionRva);

    BuiltPeExports Build(u32 sectionBaseRva) const;

private:
    std::string m_moduleName{"module.dll"};
    std::vector<PeExportEntry> m_exports;
};

} // namespace Sailor
