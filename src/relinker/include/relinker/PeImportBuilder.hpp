#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace Sailor {

struct PeImportFunction {
    std::string name;
    u16 hint{0};
};

struct PeImportModule {
    std::string dllName;
    std::vector<PeImportFunction> functions;
};

struct BuiltPeImports {
    std::vector<u8> idataSectionData;
    u32 importDirectoryRva{0};
    u32 importDirectorySize{0};
    u32 iatRva{0};
    u32 iatSize{0};
    std::unordered_map<std::string, u32> functionIatRvas;
};

class PeImportBuilder {
public:
    PeImportBuilder() = default;

    void AddModule(std::string_view dllName, std::vector<std::string> functionNames);

    BuiltPeImports Build(u32 sectionBaseRva) const;

private:
    std::vector<PeImportModule> m_modules;
};

} // namespace Sailor
