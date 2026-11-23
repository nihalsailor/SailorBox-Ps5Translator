#pragma once

#include "common/Types.hpp"
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <string>

namespace Sailor {

struct SceSaveDataMount {
    i32 userId{0};
    const char* dirName{nullptr};
    const char* mountPoint{nullptr}; // e.g. "/savedata0"
    u32 blocks{0};
    u32 mountMode{0}; // 0 = ReadOnly, 1 = ReadWrite
};

struct SceSaveDataMountResult {
    char mountPoint[32]{0};
    u32 requiredBlocks{0};
};

struct SceSaveDataDirNameSearch {
    i32 userId{0};
    const char* titleId{nullptr};
    const char* dirNamePrefix{nullptr};
    u32 maxResults{0};
};

struct SceSaveDataDirNameSearchResult {
    u32 hitNum{0};
    char dirNames[16][32]{{0}};
};

class SaveDataManager {
public:
    static SaveDataManager& Instance();

    i32 Initialize();
    i32 Terminate();
    i32 Mount(const SceSaveDataMount* mount, SceSaveDataMountResult* result);
    i32 Umount(const char* mountPoint);
    i32 SearchDirNames(const SceSaveDataDirNameSearch* search, SceSaveDataDirNameSearchResult* result);

    std::filesystem::path GetSaveRoot() const { return m_saveRoot; }

private:
    SaveDataManager();
    std::filesystem::path m_saveRoot;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::filesystem::path> m_activeMounts;
};

} // namespace Sailor
