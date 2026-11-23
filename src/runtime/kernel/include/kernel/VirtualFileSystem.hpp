#pragma once

#include "common/Types.hpp"
#include <string>
#include <string_view>
#include <span>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <fstream>

namespace Sailor {

struct SceKernelStat {
    u32 st_mode{0};
    u32 st_uid{0};
    u32 st_gid{0};
    i64 st_size{0};
    i64 st_atime{0};
    i64 st_mtime{0};
    i64 st_ctime{0};
};

struct FileHandle {
    std::filesystem::path hostPath;
    void* handle{nullptr};
    int flags{0};
};

// ==========================================
// libSceApr Structures & Types
// ==========================================

enum class AprCompressionFormat : u32 {
    None = 0,
    Kraken = 1,
    Zstd = 2,
    Lz4 = 3
};

struct AprCommand {
    i32 fd{0};
    u64 fileOffset{0};
    void* destinationMemory{nullptr};
    SceSize compressedSize{0};
    SceSize uncompressedSize{0};
    AprCompressionFormat format{AprCompressionFormat::None};
};

struct AprCommandBuffer {
    const AprCommand* commands{nullptr};
    u32 commandCount{0};
};

using SceAprSubmissionId = u64;

class VirtualFileSystem {
public:
    static VirtualFileSystem& Instance();

    void SetAppDirectory(const std::filesystem::path& dir);
    void SetSaveDirectory(const std::filesystem::path& dir);
    void Mount(std::string_view mountPoint, const std::filesystem::path& hostDir);
    void Unmount(std::string_view mountPoint);

    std::filesystem::path ResolvePath(std::string_view guestPath) const;

    i32 Open(const char* path, int flags, u32 mode);
    i32 Close(i32 fd);
    i64 Read(i32 fd, void* buf, SceSize count);
    i64 Write(i32 fd, const void* buf, SceSize count);
    i64 Pread(i32 fd, void* buf, SceSize count, i64 offset);
    i64 Pwrite(i32 fd, const void* buf, SceSize count, i64 offset);
    i64 Lseek(i32 fd, i64 offset, int whence);
    i32 Stat(const char* path, SceKernelStat* outStat);
    i32 Fstat(i32 fd, SceKernelStat* outStat);
    i32 Mkdir(const char* path, u32 mode);

    i32 SubmitAprCommandBuffer(const AprCommandBuffer* cmdBuffer, SceAprSubmissionId* outId);
    i32 WaitApr(SceAprSubmissionId id);

    static bool DecompressAprBlock(AprCompressionFormat format, std::span<const u8> src, void* dst, SceSize dstSize);

private:
    VirtualFileSystem();

    std::filesystem::path m_appDir{"."};
    std::filesystem::path m_saveDir;
    std::filesystem::path m_dataDir;

    std::mutex m_mutex;
    mutable std::mutex m_mountsMutex;
    std::unordered_map<std::string, std::filesystem::path> m_mounts;
    i32 m_nextFd{10}; // Guest file descriptors start at 10
    std::unordered_map<i32, FileHandle> m_openFiles;
};

} // namespace Sailor
