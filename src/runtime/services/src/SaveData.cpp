#include "services/SaveData.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstring>
#include <system_error>

namespace Sailor {

SaveDataManager::SaveDataManager() {
    m_hostSaveDir = std::filesystem::current_path() / "savedata";
    std::error_code ec;
    std::filesystem::create_directories(m_hostSaveDir, ec);
}

SaveDataManager& SaveDataManager::Instance() {
    static SaveDataManager instance;
    return instance;
}

i32 SaveDataManager::Initialize(const void* params) {
    (void)params;
    std::lock_guard lock(m_mutex);
    m_initialized = true;
    std::error_code ec;
    std::filesystem::create_directories(m_hostSaveDir, ec);
    SB_LOG_INFO("SaveData", "Initialized libSceSaveData (Routing mounts to '{}')", m_hostSaveDir.string());
    return SCE_OK;
}

i32 SaveDataManager::Initialize2(const void* params) {
    return Initialize(params);
}

i32 SaveDataManager::Terminate() {
    std::lock_guard lock(m_mutex);
    m_activeMounts.clear();
    m_initialized = false;
    SB_LOG_DEBUG("SaveData", "Terminated libSceSaveData");
    return SCE_OK;
}

i32 SaveDataManager::Mount(const SceSaveDataMount* mount, SceSaveDataMountResult* result) {
    if (!mount || !result) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    const std::string dirName = mount->dirName ? mount->dirName : "default";
    const std::filesystem::path targetDir = m_hostSaveDir / dirName;

    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);

    // Route mount point to guest /savedata directory
    std::strncpy(result->mountPoint.data, "/savedata", sizeof(result->mountPoint.data) - 1);
    result->mountPoint.data[sizeof(result->mountPoint.data) - 1] = '\0';
    result->requiredBlocks = 0;
    std::memset(result->reserved, 0, sizeof(result->reserved));

    m_activeMounts[result->mountPoint.data] = true;
    SB_LOG_INFO("SaveData", "Mounted savedata dir '{}' (User={}) -> Guest '{}' (Host: '{}')",
        dirName, mount->userId, result->mountPoint.data, targetDir.string());
    return SCE_OK;
}

i32 SaveDataManager::Mount2(const SceSaveDataMount2* mount, SceSaveDataMountResult* result) {
    if (!mount || !result) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    const std::string dirName = mount->dirName ? mount->dirName : "default";
    const std::filesystem::path targetDir = m_hostSaveDir / dirName;

    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);

    std::strncpy(result->mountPoint.data, "/savedata", sizeof(result->mountPoint.data) - 1);
    result->mountPoint.data[sizeof(result->mountPoint.data) - 1] = '\0';
    result->requiredBlocks = 0;
    std::memset(result->reserved, 0, sizeof(result->reserved));

    m_activeMounts[result->mountPoint.data] = true;
    SB_LOG_INFO("SaveData", "Mounted savedata2 dir '{}' (User={}) -> Guest '{}' (Host: '{}')",
        dirName, mount->userId, result->mountPoint.data, targetDir.string());
    return SCE_OK;
}

i32 SaveDataManager::Umount(const SceSaveDataMountPoint* mountPoint) {
    if (!mountPoint) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    m_activeMounts.erase(mountPoint->data);
    SB_LOG_DEBUG("SaveData", "Unmounted savedata mount '{}'", mountPoint->data);
    return SCE_OK;
}

i32 SaveDataManager::Delete(const void* deleteParam) {
    (void)deleteParam;
    SB_LOG_DEBUG("SaveData", "SaveData deleted successfully");
    return SCE_OK;
}

i32 SaveDataManager::DirNameSearch(const SceSaveDataDirNameSearchCond* cond, SceSaveDataDirNameSearchResult* result) {
    if (!result) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    (void)cond;
    std::lock_guard lock(m_mutex);
    result->hitNum = 0;
    std::memset(result->reserved, 0, sizeof(result->reserved));
    return SCE_OK;
}

size_t SaveDataManager::GetActiveMountCount() {
    std::lock_guard lock(m_mutex);
    return m_activeMounts.size();
}

} // namespace Sailor
