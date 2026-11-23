#pragma once

#include "Types.hpp"
#include <string_view>
#include <format>
#include <iostream>
#include <mutex>

namespace Sailor {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

class Logger {
public:
    static Logger& Instance();

    void SetLevel(LogLevel level);
    LogLevel GetLevel() const;

    void Log(LogLevel level, std::string_view tag, std::string_view message);

    void RecordStub(std::string_view module, std::string_view func, std::string_view nid = "");

private:
    Logger() = default;
    LogLevel m_level{LogLevel::Info};
    std::mutex m_mutex;
};

template <typename... Args>
inline void LogFormat(LogLevel level, std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (level < Logger::Instance().GetLevel()) return;
    try {
        std::string message = std::format(fmt, std::forward<Args>(args)...);
        Logger::Instance().Log(level, tag, message);
    } catch (...) {
        Logger::Instance().Log(level, tag, "Logging format error");
    }
}

#define SB_LOG_TRACE(tag, ...) ::Sailor::LogFormat(::Sailor::LogLevel::Trace, tag, __VA_ARGS__)
#define SB_LOG_DEBUG(tag, ...) ::Sailor::LogFormat(::Sailor::LogLevel::Debug, tag, __VA_ARGS__)
#define SB_LOG_INFO(tag, ...)  ::Sailor::LogFormat(::Sailor::LogLevel::Info,  tag, __VA_ARGS__)
#define SB_LOG_WARN(tag, ...)  ::Sailor::LogFormat(::Sailor::LogLevel::Warn,  tag, __VA_ARGS__)
#define SB_LOG_ERROR(tag, ...) ::Sailor::LogFormat(::Sailor::LogLevel::Error, tag, __VA_ARGS__)
#define SB_LOG_FATAL(tag, ...) ::Sailor::LogFormat(::Sailor::LogLevel::Fatal, tag, __VA_ARGS__)

#define SB_LOG_STUB(module, func, ...) \
    do { \
        ::Sailor::Logger::Instance().RecordStub(module, func); \
        ::Sailor::LogFormat(::Sailor::LogLevel::Warn, module, "STUB: " func " called " __VA_ARGS__); \
    } while(0)

} // namespace Sailor
