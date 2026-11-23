#include "shader/ShaderCache.hpp"
#include <fstream>
#include <cstdlib>
#include <format>

namespace Sailor::Shader {

namespace {

constexpr u32 CacheMagic = 0x43534253; // 'SBSC'
constexpr u32 RecompilerSchemaVersion = 0x20261116;
constexpr u32 CacheVersion = RecompilerSchemaVersion;

struct CacheHeader {
    u32 magic = CacheMagic;
    u32 version = CacheVersion;
    u32 stage = 0;
    u32 wordCount = 0;
    u64 hash = 0;
};

std::filesystem::path GetDefaultCacheDir() {
    const char* appData = std::getenv("APPDATA");
    if (appData) {
        return std::filesystem::path(appData) / "SailorBox" / "shader_cache";
    }
    return std::filesystem::temp_directory_path() / "SailorBox" / "shader_cache";
}

} // anonymous namespace

ShaderCache::ShaderCache()
    : m_diskDir(GetDefaultCacheDir()) {
    std::error_code ec;
    std::filesystem::create_directories(m_diskDir, ec);
}

ShaderCache::ShaderCache(std::string_view diskCacheDir)
    : m_diskDir(diskCacheDir.empty() ? GetDefaultCacheDir() : std::filesystem::path(diskCacheDir)) {
    std::error_code ec;
    std::filesystem::create_directories(m_diskDir, ec);
}

u64 ShaderCache::ComputeHash(std::span<const u32> code, ShaderStage stage) {
    // 64-bit FNV-1a hash with recompiler schema validation seed
    u64 hash = 14695981039346656037ULL;
    const u64 prime = 1099511628211ULL;

    const u8* schemaBytes = reinterpret_cast<const u8*>(&RecompilerSchemaVersion);
    for (size_t i = 0; i < sizeof(RecompilerSchemaVersion); ++i) {
        hash ^= schemaBytes[i];
        hash *= prime;
    }

    const u32 stageVal = static_cast<u32>(stage);
    const u8* stageBytes = reinterpret_cast<const u8*>(&stageVal);
    for (size_t i = 0; i < sizeof(stageVal); ++i) {
        hash ^= stageBytes[i];
        hash *= prime;
    }

    const u8* codeBytes = reinterpret_cast<const u8*>(code.data());
    const size_t byteSize = code.size() * sizeof(u32);
    for (size_t i = 0; i < byteSize; ++i) {
        hash ^= codeBytes[i];
        hash *= prime;
    }

    return hash;
}

std::filesystem::path ShaderCache::GetCacheFilePath(u64 hash) const {
    return m_diskDir / std::format("{:016x}.sbcache", hash);
}

bool ShaderCache::Lookup(u64 hash, std::vector<u32>& outSpirv) {
    {
        std::shared_lock lock(m_mutex);
        auto it = m_memoryCache.find(hash);
        if (it != m_memoryCache.end()) {
            outSpirv = it->second;
            return true;
        }
    }

    if (LoadFromDisk(hash, outSpirv)) {
        std::unique_lock lock(m_mutex);
        m_memoryCache[hash] = outSpirv;
        return true;
    }

    return false;
}

void ShaderCache::Store(u64 hash, ShaderStage stage, std::span<const u32> spirv) {
    std::vector<u32> spirvCopy(spirv.begin(), spirv.end());
    {
        std::unique_lock lock(m_mutex);
        m_memoryCache[hash] = spirvCopy;
    }

    SaveToDisk(hash, stage, spirv);
}

void ShaderCache::SetDiskDirectory(std::string_view dir) {
    std::unique_lock lock(m_mutex);
    m_diskDir = dir.empty() ? GetDefaultCacheDir() : std::filesystem::path(dir);
    std::error_code ec;
    std::filesystem::create_directories(m_diskDir, ec);
}

void ShaderCache::Clear() {
    std::unique_lock lock(m_mutex);
    m_memoryCache.clear();
}

size_t ShaderCache::EntryCount() const {
    std::shared_lock lock(m_mutex);
    return m_memoryCache.size();
}

bool ShaderCache::LoadFromDisk(u64 hash, std::vector<u32>& outSpirv) {
    const std::filesystem::path path = GetCacheFilePath(hash);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    CacheHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.magic != CacheMagic || header.version != CacheVersion || header.hash != hash) {
        return false;
    }

    outSpirv.resize(header.wordCount);
    file.read(reinterpret_cast<char*>(outSpirv.data()), header.wordCount * sizeof(u32));
    return static_cast<bool>(file);
}

void ShaderCache::SaveToDisk(u64 hash, ShaderStage stage, std::span<const u32> spirv) {
    const std::filesystem::path path = GetCacheFilePath(hash);
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return;
    }

    CacheHeader header;
    header.stage = static_cast<u32>(stage);
    header.wordCount = static_cast<u32>(spirv.size());
    header.hash = hash;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(u32));
}

} // namespace Sailor::Shader
