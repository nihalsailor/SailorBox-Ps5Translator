#include "tools/StubGenerator.hpp"
#include "common/Logger.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <format>
#include <regex>
#include <algorithm>
#include <cctype>

namespace Sailor::Tools {

namespace {

std::string Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

void ParseArgumentSignatures(const std::string& line, StubEntry& entry) {
    // 1. Check for explicit argument count hints: Args: 4, ArgCount: 2, argc: 3, Params: 2
    const std::regex argCountRegex(R"((?:Args|ArgCount|argc|Params|ParamCount):\s*(\d+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(line, match, argCountRegex)) {
        int count = std::clamp(std::stoi(match[1].str()), 0, 16);
        entry.paramTypes.clear();
        entry.paramNames.clear();
        for (int i = 0; i < count; ++i) {
            entry.paramTypes.push_back("u64");
            entry.paramNames.push_back(std::format("arg{}", i + 1));
        }
        return;
    }

    // 2. Check for explicit parameter list: functionName(type1 arg1, type2 arg2) or (arg1, arg2)
    const std::regex parenRegex(R"(\(([^)]+)\))");
    if (std::regex_search(line, match, parenRegex)) {
        std::string inner = Trim(match[1].str());
        if (inner.empty() || inner == "void") {
            return;
        }

        // Check if inside parentheses is something like "3 args" or "2 params"
        const std::regex countInParenRegex(R"(^(\d+)\s*(?:args?|params?)$)", std::regex::icase);
        std::smatch countMatch;
        if (std::regex_match(inner, countMatch, countInParenRegex)) {
            int count = std::clamp(std::stoi(countMatch[1].str()), 0, 16);
            entry.paramTypes.clear();
            entry.paramNames.clear();
            for (int i = 0; i < count; ++i) {
                entry.paramTypes.push_back("u64");
                entry.paramNames.push_back(std::format("arg{}", i + 1));
            }
            return;
        }

        // Split by comma
        std::stringstream ss(inner);
        std::string token;
        int argIdx = 1;
        while (std::getline(ss, token, ',')) {
            std::string t = Trim(token);
            if (t.empty()) continue;

            auto lastSpace = t.find_last_of(" \t*");
            if (lastSpace != std::string::npos && lastSpace + 1 < t.size()) {
                if (t[lastSpace] == '*') {
                    entry.paramTypes.push_back(Trim(t.substr(0, lastSpace + 1)));
                    entry.paramNames.push_back(Trim(t.substr(lastSpace + 1)));
                } else {
                    entry.paramTypes.push_back(Trim(t.substr(0, lastSpace)));
                    entry.paramNames.push_back(Trim(t.substr(lastSpace + 1)));
                }
            } else {
                entry.paramTypes.push_back(t);
                entry.paramNames.push_back(std::format("arg{}", argIdx));
            }
            argIdx++;
        }
    }
}

} // namespace

void StubGenerator::AddEntry(StubEntry entry) {
    if (entry.functionName.empty()) {
        entry.functionName = entry.nid.empty() ? "unnamed_stub" : ("stub_" + entry.nid);
    }
    auto parenPos = entry.functionName.find('(');
    if (parenPos != std::string::npos) {
        entry.functionName = entry.functionName.substr(0, parenPos);
    }
    for (const auto& existing : m_entries) {
        if (existing.moduleName == entry.moduleName && existing.functionName == entry.functionName) {
            return;
        }
    }
    m_entries.push_back(std::move(entry));
}

size_t StubGenerator::ParseLogFile(std::string_view logFilePath) {
    std::string path(logFilePath);
    std::ifstream file(path);
    if (!file) {
        SB_LOG_WARN("StubGen", "Could not open stub log: {}", logFilePath);
        return 0;
    }

    size_t beforeCount = m_entries.size();
    std::string line;

    // Pattern 1 (Explicit Labels): Module: <mod>, Function: <func>, NID: <nid>
    const std::regex labeledRegex(R"(Module:\s*([A-Za-z0-9_]+)[,\s]+Function:\s*([A-Za-z0-9_]+)(?:[,\s]+NID:\s*([A-Za-z0-9_+-]+))?)");

    // Pattern 2 (Logger::RecordStub output): Module::Function#NID
    const std::regex sbLogRegex(R"(([A-Za-z0-9_]+)::([A-Za-z0-9_]+)#([A-Za-z0-9_+-]*))");

    // Pattern 3: Module # NID # Function
    const std::regex hashRegex(R"(([A-Za-z0-9_]+)\s*#\s*([A-Za-z0-9_+-]+)\s*#\s*([A-Za-z0-9_]+))");

    // Pattern 4: Module,NID,Function
    const std::regex csvRegex(R"(([A-Za-z0-9_]+),([A-Za-z0-9_+-]+),([A-Za-z0-9_]+))");

    // Pattern 5: [STUB] Module Function (NID)
    const std::regex simpleRegex(R"(\[STUB\]\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)(?:\s+\(?([A-Za-z0-9_+-]+)\)?)?)");

    while (std::getline(file, line)) {
        std::smatch match;
        if (std::regex_search(line, match, labeledRegex)) {
            StubEntry entry;
            entry.moduleName = match[1].str();
            entry.functionName = match[2].str();
            entry.nid = match.size() > 3 ? match[3].str() : "";
            ParseArgumentSignatures(line, entry);
            AddEntry(std::move(entry));
        } else if (std::regex_search(line, match, sbLogRegex)) {
            StubEntry entry;
            entry.moduleName = match[1].str();
            entry.functionName = match[2].str();
            entry.nid = match.size() > 3 ? match[3].str() : "";
            ParseArgumentSignatures(line, entry);
            AddEntry(std::move(entry));
        } else if (std::regex_search(line, match, hashRegex)) {
            StubEntry entry;
            entry.moduleName = match[1].str();
            entry.nid = match[2].str();
            entry.functionName = match[3].str();
            ParseArgumentSignatures(line, entry);
            AddEntry(std::move(entry));
        } else if (std::regex_search(line, match, csvRegex)) {
            StubEntry entry;
            entry.moduleName = match[1].str();
            entry.nid = match[2].str();
            entry.functionName = match[3].str();
            ParseArgumentSignatures(line, entry);
            AddEntry(std::move(entry));
        } else if (std::regex_search(line, match, simpleRegex)) {
            StubEntry entry;
            entry.moduleName = match[1].str();
            entry.functionName = match[2].str();
            entry.nid = match.size() > 3 ? match[3].str() : "";
            ParseArgumentSignatures(line, entry);
            AddEntry(std::move(entry));
        }
    }

    size_t added = m_entries.size() - beforeCount;
    SB_LOG_INFO("StubGen", "Parsed {} unhandled API entries from {}", added, logFilePath);
    return added;
}

std::string StubGenerator::GenerateHeader(std::string_view moduleName) const {
    std::string s;
    s += "// =====================================================================\n";
    s += "// Auto-generated by SailorBox StubGen - Next-Gen PS5 Native Runtime\n";
    s += "// Module: " + std::string(moduleName) + "\n";
    s += "// =====================================================================\n\n";
    s += "#pragma once\n\n";
    s += "#include \"common/Types.hpp\"\n";
    s += "#include \"common/SceErrors.hpp\"\n\n";
    s += "extern \"C\" {\n\n";

    for (const auto& entry : m_entries) {
        if (!moduleName.empty() && entry.moduleName != moduleName) {
            continue;
        }

        s += std::format("SAILOR_EXPORT SAILOR_SYSV_ABI {} {}(", entry.returnType, entry.functionName);
        if (entry.paramTypes.empty()) {
            s += "void";
        } else {
            for (size_t i = 0; i < entry.paramTypes.size(); ++i) {
                if (i > 0) s += ", ";
                s += entry.paramTypes[i];
                if (i < entry.paramNames.size()) {
                    s += " " + entry.paramNames[i];
                }
            }
        }
        s += ");\n";
    }

    s += "\n} // extern \"C\"\n";
    return s;
}

std::string StubGenerator::GenerateSource(std::string_view moduleName) const {
    std::string s;
    s += "// =====================================================================\n";
    s += "// Auto-generated by SailorBox StubGen - Next-Gen PS5 Native Runtime\n";
    s += "// Module: " + std::string(moduleName) + "\n";
    s += "// =====================================================================\n\n";
    s += "#include \"" + std::string(moduleName) + ".hpp\"\n";
    s += "#include \"common/Logger.hpp\"\n\n";
    s += "extern \"C\" {\n\n";

    for (const auto& entry : m_entries) {
        if (!moduleName.empty() && entry.moduleName != moduleName) {
            continue;
        }

        s += std::format("SAILOR_EXPORT SAILOR_SYSV_ABI {} {}(", entry.returnType, entry.functionName);
        if (entry.paramTypes.empty()) {
            s += "void";
        } else {
            for (size_t i = 0; i < entry.paramTypes.size(); ++i) {
                if (i > 0) s += ", ";
                s += entry.paramTypes[i];
                if (i < entry.paramNames.size()) {
                    s += " " + entry.paramNames[i];
                }
            }
        }
        s += ") {\n";
        s += std::format("    SB_LOG_STUB(\"{}\", \"{}\", \"NID: {}\");\n",
            entry.moduleName, entry.functionName, entry.nid);

        for (const auto& pName : entry.paramNames) {
            s += std::format("    (void){};\n", pName);
        }

        if (entry.returnType != "void") {
            s += std::format("    return {};\n", entry.defaultReturnValue);
        }
        s += "}\n\n";
    }

    s += "} // extern \"C\"\n";
    return s;
}

bool StubGenerator::GenerateFiles(std::string_view moduleName, std::string_view outputDirectory) const {
    std::filesystem::path outDir(outputDirectory);
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::filesystem::path headerPath = outDir / (std::string(moduleName) + ".hpp");
    std::filesystem::path sourcePath = outDir / (std::string(moduleName) + ".cpp");

    std::ofstream hFile(headerPath);
    if (!hFile) return false;
    hFile << GenerateHeader(moduleName);

    std::ofstream cFile(sourcePath);
    if (!cFile) return false;
    cFile << GenerateSource(moduleName);

    SB_LOG_INFO("StubGen", "Generated stub files: {} and {}", headerPath.string(), sourcePath.string());
    return true;
}

} // namespace Sailor::Tools
