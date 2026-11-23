#pragma once

#include "ShaderTypes.hpp"
#include <vector>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <shared_mutex>
#include <filesystem>

namespace Sailor::Shader {

class ShaderCache {
public:
    ShaderCache();
    explicit ShaderCache(std::string_view diskCacheDir);
    ~ShaderCache() = default;

    // Computes a fast 64-bit content hash of shader bytecode and stage
    [[nodiscard]] static u64 ComputeHash(std::span<const u32> code, ShaderStage stage);

    // Lookups SPIR-V from memory cache, falling back to disk cache
    bool Lookup(u64 hash, std::vector<u32>& outSpirv);

    // Stores compiled SPIR-V into both memory and disk cache
    void Store(u64 hash, ShaderStage stage, std::span<const u32> spirv);

    // Configures disk cache root path
    void SetDiskDirectory(std::string_view dir);

    // Clears in-memory cache
    void Clear();

    [[nodiscard]] size_t EntryCount() const;

private:
    std::filesystem::path GetCacheFilePath(u64 hash) const;
    bool LoadFromDisk(u64 hash, std::vector<u32>& outSpirv);
    void SaveToDisk(u64 hash, ShaderStage stage, std::span<const u32> spirv);

    mutable std::shared_mutex m_mutex;
    std::unordered_map<u64, std::vector<u32>> m_memoryCache;
    std::filesystem::path m_diskDir;
};

} // namespace Sailor::Shader
