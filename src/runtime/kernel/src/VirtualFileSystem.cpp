#include "kernel/VirtualFileSystem.hpp"
#include "kernel/Decompressor.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sailor {

VirtualFileSystem& VirtualFileSystem::Instance() {
    static VirtualFileSystem instance;
    return instance;
}

VirtualFileSystem::VirtualFileSystem() {
    const char* envAppDir = std::getenv("SAILOR_APP_DIR");
    if (envAppDir && *envAppDir) {
        m_appDir = envAppDir;
    } else {
        std::error_code curEc;
        m_appDir = std::filesystem::current_path(curEc);
    }

    const char* appData = std::getenv("APPDATA");
    if (appData) {
        std::filesystem::path base = std::filesystem::path(appData) / "SailorBox";
        m_saveDir = base / "savedata";
        m_dataDir = base / "data";
    } else {
        m_saveDir = "./savedata";
        m_dataDir = "./data";
    }

    std::error_code ec;
    std::filesystem::create_directories(m_saveDir, ec);
    std::filesystem::create_directories(m_dataDir, ec);

    SB_LOG_INFO("VFS", "Initialized VFS (app0='{}', savedata='{}')",
        m_appDir.string(), m_saveDir.string());
}

void VirtualFileSystem::SetAppDirectory(const std::filesystem::path& dir) {
    std::lock_guard lock(m_mutex);
    m_appDir = dir;
    SB_LOG_INFO("VFS", "Updated app0 directory: {}", m_appDir.string());
}

void VirtualFileSystem::SetSaveDirectory(const std::filesystem::path& dir) {
    std::lock_guard lock(m_mutex);
    m_saveDir = dir;
    std::error_code ec;
    std::filesystem::create_directories(m_saveDir, ec);
    SB_LOG_INFO("VFS", "Updated savedata directory: {}", m_saveDir.string());
}

void VirtualFileSystem::Mount(std::string_view mountPoint, const std::filesystem::path& hostDir) {
    std::lock_guard lock(m_mountsMutex);
    std::string mp(mountPoint);
    for (char& c : mp) if (c == '\\') c = '/';
    if (!mp.starts_with('/')) mp = "/" + mp;
    m_mounts[mp] = hostDir;
    SB_LOG_INFO("VFS", "Mounted '{}' -> '{}'", mp, hostDir.string());
}

void VirtualFileSystem::Unmount(std::string_view mountPoint) {
    std::lock_guard lock(m_mountsMutex);
    std::string mp(mountPoint);
    for (char& c : mp) if (c == '\\') c = '/';
    if (!mp.starts_with('/')) mp = "/" + mp;
    m_mounts.erase(mp);
    SB_LOG_INFO("VFS", "Unmounted '{}'", mp);
}

std::filesystem::path VirtualFileSystem::ResolvePath(std::string_view guestPath) const {
    if (guestPath.empty()) return m_appDir;

    std::string pathStr(guestPath);
    for (char& c : pathStr) {
        if (c == '\\') c = '/';
    }

    {
        std::lock_guard lock(m_mountsMutex);
        for (const auto& [mountPoint, hostPath] : m_mounts) {
            if (pathStr == mountPoint || pathStr == mountPoint + "/") {
                return hostPath;
            }
            std::string prefix = mountPoint.ends_with('/') ? mountPoint : mountPoint + "/";
            if (pathStr.starts_with(prefix)) {
                return hostPath / pathStr.substr(prefix.size());
            }
        }
    }

    if (pathStr == "/app0" || pathStr == "app0" || pathStr == "/app0/" || pathStr == "app0/") {
        return m_appDir;
    }
    if (pathStr.starts_with("/app0/") || pathStr.starts_with("app0/")) {
        std::size_t skip = pathStr.starts_with('/') ? 6 : 5;
        return m_appDir / pathStr.substr(skip);
    }
    if (pathStr == "/savedata" || pathStr == "savedata" || pathStr == "/savedata/" || pathStr == "savedata/") {
        return m_saveDir;
    }
    if (pathStr.starts_with("/savedata/") || pathStr.starts_with("savedata/")) {
        std::size_t skip = pathStr.starts_with('/') ? 10 : 9;
        return m_saveDir / pathStr.substr(skip);
    }
    if (pathStr == "/data" || pathStr == "data" || pathStr == "/data/" || pathStr == "data/") {
        return m_dataDir;
    }
    if (pathStr.starts_with("/data/") || pathStr.starts_with("data/")) {
        std::size_t skip = pathStr.starts_with('/') ? 6 : 5;
        return m_dataDir / pathStr.substr(skip);
    }
    if (pathStr == "/temp" || pathStr == "temp" || pathStr == "/temp/" || pathStr == "temp/") {
        return std::filesystem::temp_directory_path() / "SailorBox";
    }
    if (pathStr.starts_with("/temp/") || pathStr.starts_with("temp/")) {
        std::size_t skip = pathStr.starts_with('/') ? 6 : 5;
        return std::filesystem::temp_directory_path() / "SailorBox" / pathStr.substr(skip);
    }

    if (pathStr.front() == '/') {
        return m_appDir / pathStr.substr(1);
    }
    return m_appDir / pathStr;
}

i32 VirtualFileSystem::Open(const char* path, int flags, u32 mode) {
    (void)mode;
    if (!path) return SCE_KERNEL_ERROR_EINVAL;

    std::filesystem::path hostPath = ResolvePath(path);

    constexpr int O_RDONLY = 0x0000;
    constexpr int O_WRONLY = 0x0001;
    constexpr int O_RDWR   = 0x0002;
    constexpr int O_CREAT  = 0x0200;
    constexpr int O_TRUNC  = 0x0400;
    constexpr int O_APPEND = 0x0008;

    // Create parent directories if creating a new file
    if (flags & O_CREAT) {
        std::error_code ec;
        std::filesystem::create_directories(hostPath.parent_path(), ec);
    }

#ifdef _WIN32
    DWORD desiredAccess = 0;
    int access = flags & 3;
    if (access == O_RDONLY) desiredAccess = GENERIC_READ;
    else if (access == O_WRONLY) desiredAccess = (flags & O_APPEND) ? FILE_APPEND_DATA : GENERIC_WRITE;
    else if (access == O_RDWR) desiredAccess = GENERIC_READ | ((flags & O_APPEND) ? FILE_APPEND_DATA : GENERIC_WRITE);

    DWORD creationDisposition = OPEN_EXISTING;
    if ((flags & O_CREAT) && (flags & O_TRUNC)) {
        creationDisposition = CREATE_ALWAYS;
    } else if (flags & O_CREAT) {
        creationDisposition = OPEN_ALWAYS;
    } else if (flags & O_TRUNC) {
        creationDisposition = TRUNCATE_EXISTING;
    }

    HANDLE hFile = CreateFileW(
        hostPath.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        creationDisposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        SB_LOG_WARN("VFS", "Failed to open path: '{}' (resolved: '{}', Win32 error={})",
            path, hostPath.string(), err);
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ?
            SCE_KERNEL_ERROR_ENOENT : SCE_KERNEL_ERROR_EACCES;
    }

    struct ScopedFileHandle {
        HANDLE h{INVALID_HANDLE_VALUE};
        ~ScopedFileHandle() {
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
        HANDLE release() {
            HANDLE temp = h;
            h = INVALID_HANDLE_VALUE;
            return temp;
        }
    } scopedHandle{hFile};

    std::lock_guard lock(m_mutex);
    i32 fd = m_nextFd++;
    FileHandle handle;
    handle.hostPath = hostPath;
    handle.handle = static_cast<void*>(scopedHandle.release());
    handle.flags = flags;
    m_openFiles[fd] = handle;

    SB_LOG_DEBUG("VFS", "Opened fd={} for guest path '{}'", fd, path);
    return fd;
#else
    return SCE_KERNEL_ERROR_ENOSYS;
#endif
}

i32 VirtualFileSystem::Close(i32 fd) {
    std::lock_guard lock(m_mutex);
    auto it = m_openFiles.find(fd);
    if (it == m_openFiles.end()) {
        return SCE_KERNEL_ERROR_EBADF;
    }
#ifdef _WIN32
    if (it->second.handle) {
        CloseHandle(static_cast<HANDLE>(it->second.handle));
    }
#endif
    m_openFiles.erase(it);
    SB_LOG_DEBUG("VFS", "Closed fd={}", fd);
    return SCE_OK;
}

i64 VirtualFileSystem::Read(i32 fd, void* buf, SceSize count) {
    if (!buf || count == 0) return 0;

    std::lock_guard lock(m_mutex);
    auto it = m_openFiles.find(fd);
    if (it == m_openFiles.end()) {
        return SCE_KERNEL_ERROR_EBADF;
    }

#ifdef _WIN32
    DWORD bytesRead = 0;
    if (!ReadFile(static_cast<HANDLE>(it->second.handle), buf, static_cast<DWORD>(count), &bytesRead, nullptr)) {
        return SCE_KERNEL_ERROR_EIO;
    }
    return static_cast<i64>(bytesRead);
#else
    return 0;
#endif
}

i64 VirtualFileSystem::Write(i32 fd, const void* buf, SceSize count) {
    if (!buf || count == 0) return 0;

    std::lock_guard lock(m_mutex);
    auto it = m_openFiles.find(fd);
    if (it == m_openFiles.end()) {
        return SCE_KERNEL_ERROR_EBADF;
    }

#ifdef _WIN32
    DWORD bytesWritten = 0;
    if (!WriteFile(static_cast<HANDLE>(it->second.handle), buf, static_cast<DWORD>(count), &bytesWritten, nullptr)) {
        return SCE_KERNEL_ERROR_EIO;
    }
    return static_cast<i64>(bytesWritten);
#else
    return 0;
#endif
}

i64 VirtualFileSystem::Pread(i32 fd, void* buf, SceSize count, i64 offset) {
    if (!buf || count == 0) return 0;

    std::lock_guard lock(m_mutex);
    auto it = m_openFiles.find(fd);
    if (it == m_openFiles.end()) {
        return SCE_KERNEL_ERROR_EBADF;
    }

#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(it->second.handle);
    LARGE_INTEGER savedPos{}, zero{};
    SetFilePointerEx(h, zero, &savedPos, FILE_CURRENT);

    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(h, buf, static_cast<DWORD>(count), &bytesRead, &ov);
    DWORD err = ok ? 0 : GetLastError();

    SetFilePointerEx(h, savedPos, nullptr, FILE_BEGIN);

    if (!ok) {
        if (err == ERROR_HANDLE_EOF) return 0;
        return SCE_KERNEL_ERROR_EIO;
    }
    return static_cast<i64>(bytesRead);
#else
    return 0;
#endif
}

i64 VirtualFileSystem::Pwrite(i32 fd, const void* buf, SceSize count, i64 offset) {
    if (!buf || count == 0) return 0;

    std::lock_guard lock(m_mutex);
    auto it = m_openFiles.find(fd);
    if (it == m_openFiles.end()) {
        return SCE_KERNEL_ERROR_EBADF;
    }

#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(it->second.handle);
    LARGE_INTEGER savedPos{}, zero{};
    SetFilePointerEx(h, zero, &savedPos, FILE_CURRENT);

    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(h, buf, static_cast<DWORD>(count), &bytesWritten, &ov);

    SetFilePointerEx(h, savedPos, nullptr, FILE_BEGIN);

    if (!ok) {
        return SCE_KERNEL_ERROR_EIO;
    }
    return static_cast<i64>(bytesWritten);
#else
    return 0;
#endif
}

i64 VirtualFileSystem::Lseek(i32 fd, i64 offset, int whence) {
    std::lock_guard lock(m_mutex);
    auto it = m_openFiles.find(fd);
    if (it == m_openFiles.end()) {
        return SCE_KERNEL_ERROR_EBADF;
    }

#ifdef _WIN32
    DWORD moveMethod = FILE_BEGIN;
    if (whence == 1) moveMethod = FILE_CURRENT;
    else if (whence == 2) moveMethod = FILE_END;

    LARGE_INTEGER liOffset, liNewPos;
    liOffset.QuadPart = offset;
    if (!SetFilePointerEx(static_cast<HANDLE>(it->second.handle), liOffset, &liNewPos, moveMethod)) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    return liNewPos.QuadPart;
#else
    return 0;
#endif
}

i32 VirtualFileSystem::Stat(const char* path, SceKernelStat* outStat) {
    if (!path || !outStat) return SCE_KERNEL_ERROR_EINVAL;
    *outStat = SceKernelStat{};

    std::filesystem::path hostPath = ResolvePath(path);
    std::error_code ec;
    auto status = std::filesystem::status(hostPath, ec);
    if (ec || !std::filesystem::exists(status)) {
        return SCE_KERNEL_ERROR_ENOENT;
    }

    outStat->st_size = std::filesystem::is_regular_file(status) ?
        static_cast<i64>(std::filesystem::file_size(hostPath, ec)) : 0;
    outStat->st_mode = std::filesystem::is_directory(status) ? 0040777 : 0100666;
    return SCE_OK;
}

i32 VirtualFileSystem::Fstat(i32 fd, SceKernelStat* outStat) {
    if (!outStat) return SCE_KERNEL_ERROR_EINVAL;
    *outStat = SceKernelStat{};

    std::lock_guard lock(m_mutex);
    auto it = m_openFiles.find(fd);
    if (it == m_openFiles.end()) {
        return SCE_KERNEL_ERROR_EBADF;
    }

#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(static_cast<HANDLE>(it->second.handle), &info)) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    outStat->st_size = (static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    outStat->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0040777 : 0100666;
    return SCE_OK;
#else
    return SCE_KERNEL_ERROR_ENOSYS;
#endif
}

i32 VirtualFileSystem::Mkdir(const char* path, u32 mode) {
    (void)mode;
    if (!path) return SCE_KERNEL_ERROR_EINVAL;
    std::filesystem::path hostPath = ResolvePath(path);
    std::error_code ec;
    std::filesystem::create_directories(hostPath, ec);
    return ec ? SCE_KERNEL_ERROR_EIO : SCE_OK;
}

i32 VirtualFileSystem::SubmitAprCommandBuffer(const AprCommandBuffer* cmdBuffer, SceAprSubmissionId* outId) {
    if (!cmdBuffer || cmdBuffer->commandCount == 0 || !cmdBuffer->commands) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    static std::atomic<u64> s_nextSubmissionId{1};
    u64 submissionId = s_nextSubmissionId.fetch_add(1, std::memory_order_relaxed);
    if (outId) *outId = submissionId;

    for (u32 i = 0; i < cmdBuffer->commandCount; ++i) {
        const auto& cmd = cmdBuffer->commands[i];
        if (!cmd.destinationMemory || (cmd.compressedSize == 0 && cmd.uncompressedSize == 0)) {
            continue;
        }

        SceSize targetSize = cmd.uncompressedSize ? cmd.uncompressedSize : cmd.compressedSize;

        if (cmd.format == AprCompressionFormat::None) {
            if (cmd.fd > 0) {
                i64 readBytes = Pread(cmd.fd, cmd.destinationMemory, targetSize, static_cast<i64>(cmd.fileOffset));
                if (readBytes < static_cast<i64>(targetSize)) {
                    std::memset(static_cast<u8*>(cmd.destinationMemory) + (readBytes > 0 ? readBytes : 0), 0,
                                targetSize - (readBytes > 0 ? static_cast<size_t>(readBytes) : 0));
                }
            } else {
                std::memset(cmd.destinationMemory, 0, targetSize);
            }
        } else {
            if (cmd.fd > 0 && cmd.compressedSize > 0) {
                std::vector<u8> compressedBuf(cmd.compressedSize);
                i64 readBytes = Pread(cmd.fd, compressedBuf.data(), cmd.compressedSize, static_cast<i64>(cmd.fileOffset));
                if (readBytes > 0) {
                    DecompressAprBlock(cmd.format, std::span<const u8>(compressedBuf.data(), static_cast<size_t>(readBytes)),
                                       cmd.destinationMemory, targetSize);
                } else {
                    std::memset(cmd.destinationMemory, 0, targetSize);
                }
            } else {
                std::memset(cmd.destinationMemory, 0, targetSize);
            }
        }

        SB_LOG_DEBUG("VFS", "libSceApr processed command #{}: fd={}, offset=0x{:X}, dest={:p}, size=0x{:X}, format={}",
            i, cmd.fd, cmd.fileOffset, cmd.destinationMemory, targetSize, static_cast<u32>(cmd.format));
    }

    SB_LOG_INFO("VFS", "libSceApr completed command buffer submission #{} ({} streaming blocks)",
        submissionId, cmdBuffer->commandCount);
    return SCE_OK;
}

i32 VirtualFileSystem::WaitApr(SceAprSubmissionId id) {
    (void)id;
    return SCE_OK;
}

bool VirtualFileSystem::DecompressAprBlock(AprCompressionFormat format, std::span<const u8> src, void* dst, SceSize dstSize) {
    if (!dst || dstSize == 0) return false;

    switch (format) {
        case AprCompressionFormat::None: {
            size_t toCopy = std::min(static_cast<size_t>(dstSize), src.size());
            std::memcpy(dst, src.data(), toCopy);
            if (toCopy < dstSize) {
                std::memset(static_cast<u8*>(dst) + toCopy, 0, dstSize - toCopy);
            }
            return true;
        }

        case AprCompressionFormat::Lz4: {
            // High-speed robust LZ4 block decompressor
            const u8* srcData = src.data();
            const size_t srcLen = src.size();
            u8* dstData = static_cast<u8*>(dst);
            size_t srcIdx = 0;
            size_t dstIdx = 0;

            while (srcIdx < srcLen && dstIdx < dstSize) {
                u8 token = srcData[srcIdx++];
                size_t litLen = (token >> 4) & 0x0F;
                if (litLen == 15) {
                    while (srcIdx < srcLen && srcData[srcIdx] == 255) {
                        litLen += 255;
                        srcIdx++;
                    }
                    if (srcIdx < srcLen) litLen += srcData[srcIdx++];
                }

                size_t toCopy = std::min(litLen, std::min(srcLen - srcIdx, static_cast<size_t>(dstSize) - dstIdx));
                if (toCopy > 0) {
                    std::memcpy(dstData + dstIdx, srcData + srcIdx, toCopy);
                    dstIdx += toCopy;
                    srcIdx += toCopy;
                }

                if (dstIdx >= dstSize || srcIdx >= srcLen) break;

                if (srcIdx + 2 > srcLen) break;
                u16 offset = static_cast<u16>(srcData[srcIdx]) | (static_cast<u16>(srcData[srcIdx + 1]) << 8);
                srcIdx += 2;
                if (offset == 0 || offset > dstIdx) break;

                size_t matchLen = (token & 0x0F) + 4;
                if (matchLen == 19) {
                    while (srcIdx < srcLen && srcData[srcIdx] == 255) {
                        matchLen += 255;
                        srcIdx++;
                    }
                    if (srcIdx < srcLen) matchLen += srcData[srcIdx++];
                }

                for (size_t m = 0; m < matchLen && dstIdx < dstSize; ++m) {
                    dstData[dstIdx] = dstData[dstIdx - offset];
                    dstIdx++;
                }
            }
            if (dstIdx < dstSize) {
                std::memset(dstData + dstIdx, 0, dstSize - dstIdx);
            }
            return true;
        }

        case AprCompressionFormat::Kraken: {
            size_t outLen = 0;
            i32 err = Decompressor::Instance().Decompress(
                src.data(), src.size(), dst, dstSize, CompressionFormat::Kraken, &outLen);
            return err == SCE_OK;
        }

        case AprCompressionFormat::Zstd: {
            size_t outLen = 0;
            i32 err = Decompressor::Instance().Decompress(
                src.data(), src.size(), dst, dstSize, CompressionFormat::Zstd, &outLen);
            return err == SCE_OK;
        }

        default:
            std::memset(dst, 0, dstSize);
            return false;
    }
}

} // namespace Sailor
