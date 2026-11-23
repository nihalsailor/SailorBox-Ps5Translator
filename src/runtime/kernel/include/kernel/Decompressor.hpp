#pragma once

#include "common/Types.hpp"
#include <cstddef>
#include <span>

namespace Sailor {

enum class CompressionFormat {
    None = 0,
    Zlib = 1,
    Lz4 = 2,
    Kraken = 3,
    Mermaid = 4,
    Selkie = 5,
    Leviathan = 6,
    Zstd = 7
};

class Decompressor {
public:
    static Decompressor& Instance();

    i32 Decompress(const void* src, size_t srcSize, void* dst, size_t dstSize,
                   CompressionFormat format, size_t* outSize);

    bool HasOodleHardwareLibrary() const { return m_hOodle != nullptr; }

private:
    Decompressor();
    ~Decompressor();

    void TryLoadHostOodle();
    bool DecompressLz4(const u8* src, size_t srcLen, u8* dst, size_t dstLen, size_t* outLen);
    bool DecompressDeflate(const u8* src, size_t srcLen, u8* dst, size_t dstLen, size_t* outLen);

    void* m_hOodle{nullptr};
    using OodleLZ_Decompress_Fn = size_t(__stdcall*)(
        const void* compBuf, size_t compBufSize,
        void* rawBuf, size_t rawBufSize,
        int fuzzSafe, int checkCRC, int verbosity,
        void* decBufBase, size_t decBufSize,
        void* fpCallback, void* callbackUserData,
        void* decoderMemory, size_t decoderMemorySize,
        int threadPhase);

    OodleLZ_Decompress_Fn m_oodleDecompress{nullptr};
};

} // namespace Sailor
