#pragma once

#include "common/Types.hpp"
#include "services/UserService.hpp"
#include <string>
#include <mutex>
#include <filesystem>
#include <unordered_map>

namespace Sailor {

#pragma pack(push, 1)

struct SceSaveDataMountPoint {
    char data[16]{"/savedata"};
};

struct SceSaveDataMount {
    SceUserServiceUserId userId{1};
    const char* dirName{nullptr};
    u64 blocks{0};
    u32 mountMode{0};
    u8 reserved[32]{0};
};

struct SceSaveDataMount2 {
    SceUserServiceUserId userId{1};
    const char* dirName{nullptr};
    const char* fingerPrint{nullptr};
    u64 blocks{0};
    u32 mountMode{0};
    u8 reserved[32]{0};
};

struct SceSaveDataMountResult {
    SceSaveDataMountPoint mountPoint;
    u64 requiredBlocks{0};
    u8 reserved[32]{0};
};

struct SceSaveDataDirNameSearchCond {
    SceUserServiceUserId userId{1};
    const char* dirName{nullptr};
    const char* titleId{nullptr};
    u32 maxResults{16};
};

struct SceSaveDataDirNameSearchResult {
    u32 hitNum{0};
    u8 reserved[28]{0};
};

#pragma pack(pop)

class SAILOR_EXPORT SaveDataManager {
public:
    static SaveDataManager& Instance();

    i32 Initialize(const void* params);
    i32 Initialize2(const void* params);
    i32 Terminate();

    i32 Mount(const SceSaveDataMount* mount, SceSaveDataMountResult* result);
    i32 Mount2(const SceSaveDataMount2* mount, SceSaveDataMountResult* result);
    i32 Umount(const SceSaveDataMountPoint* mountPoint);
    i32 Delete(const void* deleteParam);
    i32 DirNameSearch(const SceSaveDataDirNameSearchCond* cond, SceSaveDataDirNameSearchResult* result);

    // Diagnostics & testing
    std::string GetHostSaveDirectory() const { return m_hostSaveDir.string(); }
    void SetHostSaveDirectory(const std::filesystem::path& dir) { m_hostSaveDir = dir; }
    size_t GetActiveMountCount();

private:
    SaveDataManager();
    ~SaveDataManager() = default;

    std::mutex m_mutex;
    bool m_initialized{false};
    std::filesystem::path m_hostSaveDir;
    std::unordered_map<std::string, bool> m_activeMounts;
};

} // namespace Sailor
