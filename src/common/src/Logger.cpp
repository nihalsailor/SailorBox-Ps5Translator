#include "common/Logger.hpp"
#include <chrono>
#include <fstream>
#include <unordered_set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Sailor {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetLevel(LogLevel level) {
    std::lock_guard lock(m_mutex);
    m_level = level;
}

LogLevel Logger::GetLevel() const {
    return m_level;
}

void Logger::Log(LogLevel level, std::string_view tag, std::string_view message) {
    std::lock_guard lock(m_mutex);
    if (level < m_level) return;

    const char* levelStr = "INFO";
    switch (level) {
        case LogLevel::Trace: levelStr = "TRACE"; break;
        case LogLevel::Debug: levelStr = "DEBUG"; break;
        case LogLevel::Info:  levelStr = "INFO";  break;
        case LogLevel::Warn:  levelStr = "WARN";  break;
        case LogLevel::Error: levelStr = "ERROR"; break;
        case LogLevel::Fatal: levelStr = "FATAL"; break;
    }

    auto now = std::chrono::system_clock::now();
    std::string line = std::format("[{:%H:%M:%S}] [{}] [{}] {}\n", now, levelStr, tag, message);
    std::cout.clear();
    std::cout << line;
    std::cout.flush();

#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        WriteFile(hOut, line.data(), static_cast<DWORD>(line.size()), &written, NULL);
    }
#endif

    static std::ofstream s_runtimeLog("C:\\Developments\\@SailorBox\\scratch\\runtime_live.log", std::ios::app);
    if (s_runtimeLog.is_open()) {
        s_runtimeLog << line;
        s_runtimeLog.flush();
    }
}

void Logger::RecordStub(std::string_view module, std::string_view func, std::string_view nid) {
    std::lock_guard lock(m_mutex);
    static std::unordered_set<std::string> recordedStubs;
    std::string key = std::format("{}::{}#{}", module, func, nid);
    if (recordedStubs.insert(key).second) {
        // First time seeing this stub! Log to file unhandled_apis.json
        std::ofstream out("unhandled_apis.log", std::ios::app);
        if (out.is_open()) {
            out << key << "\n";
        }
    }
}

} // namespace Sailor
