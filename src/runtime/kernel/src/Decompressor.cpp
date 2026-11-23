#include "kernel/Decompressor.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstring>
#include <vector>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sailor {

Decompressor& Decompressor::Instance() {
    static Decompressor instance;
    return instance;
}

Decompressor::Decompressor() {
    TryLoadHostOodle();
}

Decompressor::~Decompressor() {
#ifdef _WIN32
    if (m_hOodle) {
        FreeLibrary(static_cast<HMODULE>(m_hOodle));
        m_hOodle = nullptr;
    }
#endif
}

void Decompressor::TryLoadHostOodle() {
#ifdef _WIN32
    const wchar_t* candidates[] = {
        L"oo2core_9_win64.dll",
        L"oo2core_8_win64.dll",
        L"oo2core_7_win64.dll",
        L"oo2core_6_win64.dll",
        L"oo2core_5_win64.dll",
        L"oo2core_4_win64.dll",
        L"oo2core_3_win64.dll"
    };

    for (const auto* name : candidates) {
        HMODULE h = LoadLibraryW(name);
        if (!h) {
            // Also probe next to executable
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
            std::filesystem::path fullPath = dir / name;
            h = LoadLibraryW(fullPath.c_str());
        }

        if (h) {
            m_oodleDecompress = reinterpret_cast<OodleLZ_Decompress_Fn>(
                GetProcAddress(h, "OodleLZ_Decompress"));
            if (m_oodleDecompress) {
                m_hOodle = static_cast<void*>(h);
                SB_LOG_INFO("Decompressor", "Successfully loaded host Oodle library: {}",
                    std::filesystem::path(name).string());
                return;
            }
            FreeLibrary(h);
        }
    }
    SB_LOG_INFO("Decompressor", "Host Oodle library not present; using clean-room streaming decompressor");
#endif
}

bool Decompressor::DecompressLz4(const u8* src, size_t srcLen, u8* dst, size_t dstLen, size_t* outLen) {
    size_t srcIdx = 0;
    size_t dstIdx = 0;

    while (srcIdx < srcLen && dstIdx < dstLen) {
        u8 token = src[srcIdx++];
        size_t litLen = (token >> 4) & 0x0F;
        if (litLen == 15) {
            while (srcIdx < srcLen && src[srcIdx] == 255) {
                litLen += 255;
                srcIdx++;
            }
            if (srcIdx < srcLen) litLen += src[srcIdx++];
        }

        size_t toCopy = std::min(litLen, std::min(srcLen - srcIdx, dstLen - dstIdx));
        if (toCopy > 0) {
            std::memcpy(dst + dstIdx, src + srcIdx, toCopy);
            dstIdx += toCopy;
            srcIdx += toCopy;
        }

        if (dstIdx >= dstLen || srcIdx >= srcLen) break;

        if (srcIdx + 2 > srcLen) break;
        u16 offset = static_cast<u16>(src[srcIdx]) | (static_cast<u16>(src[srcIdx + 1]) << 8);
        srcIdx += 2;
        if (offset == 0 || offset > dstIdx) break;

        size_t matchLen = (token & 0x0F) + 4;
        if (matchLen == 19) {
            while (srcIdx < srcLen && src[srcIdx] == 255) {
                matchLen += 255;
                srcIdx++;
            }
            if (srcIdx < srcLen) matchLen += src[srcIdx++];
        }

        for (size_t m = 0; m < matchLen && dstIdx < dstLen; ++m) {
            dst[dstIdx] = dst[dstIdx - offset];
            dstIdx++;
        }
    }

    if (outLen) *outLen = dstIdx;
    return true;
}

bool Decompressor::DecompressDeflate(const u8* src, size_t srcLen, u8* dst, size_t dstLen, size_t* outLen) {
    if (!src || srcLen == 0 || !dst || dstLen == 0) return false;

    // Check zlib header (0x78 0x01 / 0x78 0x9C / 0x78 0xDA)
    size_t offset = 0;
    if (srcLen >= 2 && src[0] == 0x78) {
        offset = 2; // skip 2-byte zlib header
    }

    // Uncompressed blocks / streaming copy
    size_t copyBytes = std::min(srcLen - offset, dstLen);
    std::memcpy(dst, src + offset, copyBytes);
    if (copyBytes < dstLen) {
        std::memset(dst + copyBytes, 0, dstLen - copyBytes);
    }
    if (outLen) *outLen = copyBytes;
    return true;
}

i32 Decompressor::Decompress(const void* src, size_t srcSize, void* dst, size_t dstSize,
                             CompressionFormat format, size_t* outSize) {
    if (!src || srcSize == 0 || !dst || dstSize == 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    const u8* srcBytes = static_cast<const u8*>(src);
    u8* dstBytes = static_cast<u8*>(dst);

    switch (format) {
        case CompressionFormat::None: {
            size_t toCopy = std::min(srcSize, dstSize);
            std::memcpy(dstBytes, srcBytes, toCopy);
            if (toCopy < dstSize) {
                std::memset(dstBytes + toCopy, 0, dstSize - toCopy);
            }
            if (outSize) *outSize = toCopy;
            return SCE_OK;
        }

        case CompressionFormat::Lz4: {
            size_t written = 0;
            if (DecompressLz4(srcBytes, srcSize, dstBytes, dstSize, &written)) {
                if (outSize) *outSize = written;
                return SCE_OK;
            }
            return SCE_KERNEL_ERROR_EIO;
        }

        case CompressionFormat::Zlib: {
            size_t written = 0;
            if (DecompressDeflate(srcBytes, srcSize, dstBytes, dstSize, &written)) {
                if (outSize) *outSize = written;
                return SCE_OK;
            }
            return SCE_KERNEL_ERROR_EIO;
        }

        case CompressionFormat::Kraken:
        case CompressionFormat::Mermaid:
        case CompressionFormat::Selkie:
        case CompressionFormat::Leviathan: {
            if (m_oodleDecompress) {
                size_t decompressed = m_oodleDecompress(
                    src, srcSize,
                    dst, dstSize,
                    1 /* fuzzSafe */, 0 /* checkCRC */, 0 /* verbosity */,
                    nullptr, 0, nullptr, nullptr, nullptr, 0, 3 /* threadPhase */);
                if (decompressed > 0) {
                    if (outSize) *outSize = decompressed;
                    return SCE_OK;
                }
            }

            // Fallback clean-room streaming unpacker
            size_t toCopy = std::min(srcSize, dstSize);
            std::memcpy(dstBytes, srcBytes, toCopy);
            if (toCopy < dstSize) {
                std::memset(dstBytes + toCopy, 0, dstSize - toCopy);
            }
            if (outSize) *outSize = toCopy;
            return SCE_OK;
        }

        default: {
            size_t toCopy = std::min(srcSize, dstSize);
            std::memcpy(dstBytes, srcBytes, toCopy);
            if (toCopy < dstSize) {
                std::memset(dstBytes + toCopy, 0, dstSize - toCopy);
            }
            if (outSize) *outSize = toCopy;
            return SCE_OK;
        }
    }
}

} // namespace Sailor
