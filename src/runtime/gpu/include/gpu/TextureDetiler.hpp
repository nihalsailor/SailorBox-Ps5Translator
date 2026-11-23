#pragma once

#include "common/Types.hpp"
#include <vector>
#include <cstddef>
#include <span>
#include <future>

namespace Sailor {

enum class TextureSwizzleMode : u32 {
    Linear = 0,
    Sw256B_S = 1,
    Sw256B_D = 2,
    Sw4KB_S = 3,
    Sw4KB_D = 4,
    Sw64KB_S = 5,
    Sw64KB_D = 6,
    Sw64KB_R = 7,
    Sw64KB_Z = 8,
    Sw64KB_S_X = 24,
    Sw64KB_D_X = 25,
    Sw64KB_R_X = 26,
    Sw64KB_Z_X = 27
};

struct TextureSurfaceInfo {
    u32 width{0};
    u32 height{0};
    u32 depth{1};
    u32 elementBytes{4}; // 1 = R8, 2 = R16, 4 = RGBA8, 8 = BC1, 16 = BC7/RGBA32F
    TextureSwizzleMode swizzleMode{TextureSwizzleMode::Sw64KB_R_X};
    u32 pitchBytes{0};
    u64 totalSize{0};
};

class TextureDetiler {
public:
    static TextureDetiler& Instance();

    // Detiles AMD RDNA2 / GCN tiled memory into a linear byte buffer
    bool DetileSurface(const void* tiledSrc, void* linearDst, const TextureSurfaceInfo& info);

    // Retiles linear memory into AMD RDNA2 / GCN tiled memory
    bool RetileSurface(const void* linearSrc, void* tiledDst, const TextureSurfaceInfo& info);

    // Asynchronously detiles AMD RDNA2 / GCN tiled memory in the background
    std::future<bool> DetileSurfaceAsync(const void* tiledSrc, void* linearDst, const TextureSurfaceInfo& info);

    // Asynchronously retiles linear memory in the background
    std::future<bool> RetileSurfaceAsync(const void* linearSrc, void* tiledDst, const TextureSurfaceInfo& info);

    // Computes tiled offset inside surface for pixel (x, y, z)
    u64 ComputeTiledOffset(u32 x, u32 y, u32 z, const TextureSurfaceInfo& info) const;

private:
    TextureDetiler();
    void InitializeLuts();

    u32 ComputeBlockOffset(u32 x, u32 y, u32 z, TextureSwizzleMode mode, u32 elementBytes) const;
};

} // namespace Sailor
