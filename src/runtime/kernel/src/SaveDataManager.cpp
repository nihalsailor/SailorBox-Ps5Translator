#include "kernel/SaveDataManager.hpp"
#include "kernel/VirtualFileSystem.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstring>

namespace Sailor {

SaveDataManager& SaveDataManager::Instance() {
    static SaveDataManager instance;
    return instance;
}

SaveDataManager::SaveDataManager() {
    m_saveRoot = std::filesystem::current_path() / "savedata";
    std::error_code ec;
    std::filesystem::create_directories(m_saveRoot, ec);
}

i32 SaveDataManager::Initialize() {
    SB_LOG_INFO("SaveData", "SaveDataManager initialized (root: '{}')", m_saveRoot.string());
    return SCE_OK;
}

i32 SaveDataManager::Terminate() {
    std::lock_guard lock(m_mutex);
    m_activeMounts.clear();
    return SCE_OK;
}

i32 SaveDataManager::Mount(const SceSaveDataMount* mount, SceSaveDataMountResult* result) {
    if (!mount || !mount->dirName || !result) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);

    std::string mountPoint = mount->mountPoint ? mount->mountPoint : "/savedata0";
    std::filesystem::path dirPath = m_saveRoot / std::to_string(mount->userId) / mount->dirName;

    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        SB_LOG_ERROR("SaveData", "Failed to create save directory '{}': {}", dirPath.string(), ec.message());
        return SCE_KERNEL_ERROR_EIO;
    }

    VirtualFileSystem::Instance().Mount(mountPoint, dirPath);
    m_activeMounts[mountPoint] = dirPath;

    std::strncpy(result->mountPoint, mountPoint.c_str(), sizeof(result->mountPoint) - 1);
    result->mountPoint[sizeof(result->mountPoint) - 1] = '\0';
    result->requiredBlocks = 1024;

    SB_LOG_INFO("SaveData", "Mounted save data '{}' -> '{}' (host path '{}')",
        mount->dirName, mountPoint, dirPath.string());
    return SCE_OK;
}

i32 SaveDataManager::Umount(const char* mountPoint) {
    if (!mountPoint) return SCE_KERNEL_ERROR_EINVAL;

    std::lock_guard lock(m_mutex);
    auto it = m_activeMounts.find(mountPoint);
    if (it != m_activeMounts.end()) {
        m_activeMounts.erase(it);
        VirtualFileSystem::Instance().Unmount(mountPoint);
        SB_LOG_INFO("SaveData", "Unmounted save data from '{}'", mountPoint);
        return SCE_OK;
    }
    return SCE_KERNEL_ERROR_ENOENT;
}

i32 SaveDataManager::SearchDirNames(const SceSaveDataDirNameSearch* search, SceSaveDataDirNameSearchResult* result) {
    if (!search || !result) return SCE_KERNEL_ERROR_EINVAL;

    std::lock_guard lock(m_mutex);
    std::filesystem::path userDir = m_saveRoot / std::to_string(search->userId);
    result->hitNum = 0;

    std::error_code ec;
    if (std::filesystem::exists(userDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(userDir, ec)) {
            if (entry.is_directory()) {
                std::string name = entry.path().filename().string();
                if (search->dirNamePrefix && !name.starts_with(search->dirNamePrefix)) {
                    continue;
                }
                if (result->hitNum < 16 && result->hitNum < search->maxResults) {
                    std::strncpy(result->dirNames[result->hitNum], name.c_str(), 31);
                    result->dirNames[result->hitNum][31] = '\0';
                    result->hitNum++;
                }
            }
        }
    }

    SB_LOG_INFO("SaveData", "DirNameSearch for userId {} found {} entries", search->userId, result->hitNum);
    return SCE_OK;
}

} // namespace Sailor
