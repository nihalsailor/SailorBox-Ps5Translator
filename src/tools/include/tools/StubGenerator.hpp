#pragma once

#include "common/Types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <string_view>

namespace Sailor::Tools {

struct StubEntry {
    std::string moduleName;
    std::string nid;
    std::string functionName;
    std::string returnType{"i32"};
    std::string defaultReturnValue{"SCE_OK"};
    std::vector<std::string> paramTypes;
    std::vector<std::string> paramNames;
};

class StubGenerator {
public:
    StubGenerator() = default;

    // Registers an unhandled NID for stub generation
    void AddEntry(StubEntry entry);

    // Parses unhandled_apis.log produced by SB_LOG_STUB at runtime
    size_t ParseLogFile(std::string_view logFilePath);

    // Generates C++ header file string with function prototypes
    [[nodiscard]] std::string GenerateHeader(std::string_view moduleName) const;

    // Generates C++ source file string with SysV ABI implementations and SB_LOG_STUB
    [[nodiscard]] std::string GenerateSource(std::string_view moduleName) const;

    // Writes .hpp and .cpp stub files to disk in target directory
    bool GenerateFiles(std::string_view moduleName, std::string_view outputDirectory) const;

    [[nodiscard]] size_t EntryCount() const { return m_entries.size(); }
    [[nodiscard]] const std::vector<StubEntry>& GetEntries() const { return m_entries; }
    void Clear() { m_entries.clear(); }

private:
    std::vector<StubEntry> m_entries;
};

} // namespace Sailor::Tools
