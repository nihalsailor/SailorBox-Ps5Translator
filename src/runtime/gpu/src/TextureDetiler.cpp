#include "gpu/TextureDetiler.hpp"
#include "common/Logger.hpp"
#include <cstring>
#include <bit>
#include <algorithm>
#include <mutex>
#include <thread>
#include <future>

namespace Sailor {

struct SwizzleEquationTable {
    u32 swizzleMode;
    u32 elementBytes;
    u32 bits[16];
};

static constexpr SwizzleEquationTable kEquations[] = {
    // Mode 24: Sw64KB_S_X
    {24u, 1u, {0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x00000008u, 0x00010000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {24u, 2u, {0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x00000008u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00010000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {24u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x00004000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00008000u, 0x00000010u, 0x00040000u, 0x00000040u}},
    {24u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x00000004u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000008u, 0x00010000u, 0x00000040u}},
    {24u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08008008u, 0x04010010u, 0x02020040u, 0x01040020u, 0x00004000u, 0x00000004u, 0x00008000u, 0x00000010u}},

    // Mode 25: Sw64KB_D_X
    {25u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {25u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {25u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
    {25u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00002000u, 0x00000002u, 0x00000004u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
    {25u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00002000u, 0x00000001u, 0x00000002u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}},

    // Mode 26: Sw64KB_R_X (Primary PS5 Scanout & Render Target Swizzle)
    {26u, 1u, {0x00000001u, 0x00000002u, 0x00000004u, 0x00002000u, 0x00001000u, 0x00004000u, 0x00000008u, 0x00008000u, 0x08010080u, 0x04080010u, 0x02020040u, 0x01040020u, 0x00040000u, 0x00000040u, 0x00080000u, 0x00000080u}},
    {26u, 2u, {0x00000000u, 0x00000001u, 0x00000002u, 0x00000004u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000008u, 0x08008080u, 0x04040010u, 0x02010040u, 0x01020020u, 0x00020000u, 0x00000040u, 0x00040000u, 0x00000080u}},
    {26u, 4u, {0x00000000u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00001000u, 0x00002000u, 0x00004000u, 0x00000004u, 0x08008040u, 0x04040008u, 0x02010020u, 0x01020010u, 0x00020000u, 0x00000020u, 0x00040000u, 0x00000040u}},
    {26u, 8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00000004u, 0x00002000u, 0x08004040u, 0x04020008u, 0x02008020u, 0x01010010u, 0x00010000u, 0x00000020u, 0x00020000u, 0x00000040u}},
    {26u, 16u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00001000u, 0x00000002u, 0x00002000u, 0x08004020u, 0x04020004u, 0x02008010u, 0x01010008u, 0x00010000u, 0x00000010u, 0x00020000u, 0x00000020u}}
};

struct PrecomputedSwizzleLut {
    u16 lutX[256]{};
    u16 lutY[256]{};
    u16 lutZ[64]{};
};

static PrecomputedSwizzleLut s_swizzleLuts[sizeof(kEquations) / sizeof(kEquations[0])];

static const SwizzleEquationTable* FindEquationWithIndex(TextureSwizzleMode mode, u32 elementBytes, size_t& outIdx) {
    u32 m = static_cast<u32>(mode);
    for (size_t i = 0; i < sizeof(kEquations) / sizeof(kEquations[0]); ++i) {
        if (kEquations[i].swizzleMode == m && kEquations[i].elementBytes == elementBytes) {
            outIdx = i;
            return &kEquations[i];
        }
    }
    // Fallback to mode 26 (R_X)
    for (size_t i = 0; i < sizeof(kEquations) / sizeof(kEquations[0]); ++i) {
        if (kEquations[i].swizzleMode == 26u && kEquations[i].elementBytes == elementBytes) {
            outIdx = i;
            return &kEquations[i];
        }
    }
    return nullptr;
}

TextureDetiler::TextureDetiler() {
    InitializeLuts();
}

void TextureDetiler::InitializeLuts() {
    static std::once_flag s_onceFlag;
    std::call_once(s_onceFlag, []() {
        for (size_t eqIdx = 0; eqIdx < sizeof(kEquations) / sizeof(kEquations[0]); ++eqIdx) {
            const auto& eq = kEquations[eqIdx];
            for (u32 x = 0; x < 256; ++x) {
                u16 offX = 0;
                for (size_t b = 0; b < 16; ++b) {
                    u32 mask = eq.bits[b];
                    if ((std::popcount(x & (mask & 0xFFFu)) & 1)) {
                        offX |= (1u << b);
                    }
                }
                s_swizzleLuts[eqIdx].lutX[x] = offX;
            }
            for (u32 y = 0; y < 256; ++y) {
                u16 offY = 0;
                for (size_t b = 0; b < 16; ++b) {
                    u32 mask = eq.bits[b];
                    if ((std::popcount(((y << 12) & (mask & 0xFFF000u)) >> 12) & 1)) {
                        offY |= (1u << b);
                    }
                }
                s_swizzleLuts[eqIdx].lutY[y] = offY;
            }
            for (u32 z = 0; z < 64; ++z) {
                u16 offZ = 0;
                for (size_t b = 0; b < 16; ++b) {
                    u32 mask = eq.bits[b];
                    if ((std::popcount(((z << 24) & (mask & 0xFF000000u)) >> 24) & 1)) {
                        offZ |= (1u << b);
                    }
                }
                s_swizzleLuts[eqIdx].lutZ[z] = offZ;
            }
        }
    });
}

TextureDetiler& TextureDetiler::Instance() {
    static TextureDetiler instance;
    return instance;
}

u32 TextureDetiler::ComputeBlockOffset(u32 x, u32 y, u32 z, TextureSwizzleMode mode, u32 elementBytes) const {
    size_t eqIdx = 0;
    const auto* eq = FindEquationWithIndex(mode, elementBytes, eqIdx);
    if (!eq) return (y * 8 + x) * elementBytes;

    if (x < 256 && y < 256 && z < 64) {
        return s_swizzleLuts[eqIdx].lutX[x] ^ s_swizzleLuts[eqIdx].lutY[y] ^ s_swizzleLuts[eqIdx].lutZ[z];
    }

    u32 offset = 0;
    for (size_t b = 0; b < 16; ++b) {
        u32 mask = eq->bits[b];
        if (mask == 0) continue;

        u32 bitVal = 0;
        bitVal ^= (x & (mask & 0xFFFu));
        bitVal ^= ((y << 12) & (mask & 0xFFF000u)) >> 12;
        bitVal ^= ((z << 24) & (mask & 0xFF000000u)) >> 24;

        if (std::popcount(bitVal) & 1) {
            offset |= (1u << b);
        }
    }
    return offset;
}

u64 TextureDetiler::ComputeTiledOffset(u32 x, u32 y, u32 z, const TextureSurfaceInfo& info) const {
    if (info.swizzleMode == TextureSwizzleMode::Linear) {
        u64 pitch = info.pitchBytes ? info.pitchBytes : (info.width * info.elementBytes);
        return z * (pitch * info.height) + y * pitch + x * info.elementBytes;
    }

    // 64 KiB block geometry
    u32 blockWidth = 64;
    u32 blockHeight = 64;
    if (info.elementBytes == 1) {
        blockWidth = 256; blockHeight = 256;
    } else if (info.elementBytes == 2) {
        blockWidth = 256; blockHeight = 128;
    } else if (info.elementBytes == 4) {
        blockWidth = 128; blockHeight = 128;
    } else if (info.elementBytes == 8) {
        blockWidth = 128; blockHeight = 64;
    } else if (info.elementBytes == 16) {
        blockWidth = 64; blockHeight = 64;
    }

    u32 blockX = x / blockWidth;
    u32 blockY = y / blockHeight;
    u32 blocksPerRow = (info.width + blockWidth - 1) / blockWidth;

    u64 blockIndex = (static_cast<u64>(z) * ((info.height + blockHeight - 1) / blockHeight) * blocksPerRow) +
                     (static_cast<u64>(blockY) * blocksPerRow) + blockX;

    u64 blockBase = blockIndex * 0x10000ULL; // 64 KiB per block
    u32 inBlockOffset = ComputeBlockOffset(x % blockWidth, y % blockHeight, z, info.swizzleMode, info.elementBytes);

    return blockBase + inBlockOffset;
}

bool TextureDetiler::DetileSurface(const void* tiledSrc, void* linearDst, const TextureSurfaceInfo& info) {
    if (!tiledSrc || !linearDst || info.width == 0 || info.height == 0) {
        return false;
    }

    const u8* src = static_cast<const u8*>(tiledSrc);
    u8* dst = static_cast<u8*>(linearDst);

    u32 bytesPerPixel = info.elementBytes;
    u64 linearPitch = info.pitchBytes ? info.pitchBytes : (info.width * bytesPerPixel);

    u32 numThreads = (info.height >= 128) ? std::clamp(std::thread::hardware_concurrency(), 1u, 8u) : 1u;
    if (numThreads > 1) {
        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);
        u32 rowsPerThread = (info.height + numThreads - 1) / numThreads;
        for (u32 t = 0; t < numThreads; ++t) {
            u32 yStart = t * rowsPerThread;
            u32 yEnd = (std::min)(yStart + rowsPerThread, info.height);
            if (yStart >= yEnd) break;
            futures.push_back(std::async(std::launch::async, [=, this]() {
                for (u32 z = 0; z < info.depth; ++z) {
                    for (u32 y = yStart; y < yEnd; ++y) {
                        u8* dstRow = dst + z * (linearPitch * info.height) + y * linearPitch;
                        for (u32 x = 0; x < info.width; ++x) {
                            u64 tiledOffset = ComputeTiledOffset(x, y, z, info);
                            std::memcpy(dstRow + x * bytesPerPixel, src + tiledOffset, bytesPerPixel);
                        }
                    }
                }
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
    } else {
        for (u32 z = 0; z < info.depth; ++z) {
            for (u32 y = 0; y < info.height; ++y) {
                u8* dstRow = dst + z * (linearPitch * info.height) + y * linearPitch;
                for (u32 x = 0; x < info.width; ++x) {
                    u64 tiledOffset = ComputeTiledOffset(x, y, z, info);
                    std::memcpy(dstRow + x * bytesPerPixel, src + tiledOffset, bytesPerPixel);
                }
            }
        }
    }

    SB_LOG_DEBUG("TextureDetiler", "Detiled surface: {}x{}x{}, bpp={}, mode={}",
        info.width, info.height, info.depth, info.elementBytes, static_cast<u32>(info.swizzleMode));
    return true;
}

bool TextureDetiler::RetileSurface(const void* linearSrc, void* tiledDst, const TextureSurfaceInfo& info) {
    if (!linearSrc || !tiledDst || info.width == 0 || info.height == 0) {
        return false;
    }

    const u8* src = static_cast<const u8*>(linearSrc);
    u8* dst = static_cast<u8*>(tiledDst);

    u32 bytesPerPixel = info.elementBytes;
    u64 linearPitch = info.pitchBytes ? info.pitchBytes : (info.width * bytesPerPixel);

    u32 numThreads = (info.height >= 128) ? std::clamp(std::thread::hardware_concurrency(), 1u, 8u) : 1u;
    if (numThreads > 1) {
        std::vector<std::future<void>> futures;
        futures.reserve(numThreads);
        u32 rowsPerThread = (info.height + numThreads - 1) / numThreads;
        for (u32 t = 0; t < numThreads; ++t) {
            u32 yStart = t * rowsPerThread;
            u32 yEnd = (std::min)(yStart + rowsPerThread, info.height);
            if (yStart >= yEnd) break;
            futures.push_back(std::async(std::launch::async, [=, this]() {
                for (u32 z = 0; z < info.depth; ++z) {
                    for (u32 y = yStart; y < yEnd; ++y) {
                        const u8* srcRow = src + z * (linearPitch * info.height) + y * linearPitch;
                        for (u32 x = 0; x < info.width; ++x) {
                            u64 tiledOffset = ComputeTiledOffset(x, y, z, info);
                            std::memcpy(dst + tiledOffset, srcRow + x * bytesPerPixel, bytesPerPixel);
                        }
                    }
                }
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
    } else {
        for (u32 z = 0; z < info.depth; ++z) {
            for (u32 y = 0; y < info.height; ++y) {
                const u8* srcRow = src + z * (linearPitch * info.height) + y * linearPitch;
                for (u32 x = 0; x < info.width; ++x) {
                    u64 tiledOffset = ComputeTiledOffset(x, y, z, info);
                    std::memcpy(dst + tiledOffset, srcRow + x * bytesPerPixel, bytesPerPixel);
                }
            }
        }
    }

    SB_LOG_DEBUG("TextureDetiler", "Retiled surface: {}x{}x{}, bpp={}, mode={}",
        info.width, info.height, info.depth, info.elementBytes, static_cast<u32>(info.swizzleMode));
    return true;
}

std::future<bool> TextureDetiler::DetileSurfaceAsync(const void* tiledSrc, void* linearDst, const TextureSurfaceInfo& info) {
    return std::async(std::launch::async, [this, tiledSrc, linearDst, info]() {
        return DetileSurface(tiledSrc, linearDst, info);
    });
}

std::future<bool> TextureDetiler::RetileSurfaceAsync(const void* linearSrc, void* tiledDst, const TextureSurfaceInfo& info) {
    return std::async(std::launch::async, [this, linearSrc, tiledDst, info]() {
        return RetileSurface(linearSrc, tiledDst, info);
    });
}

} // namespace Sailor
