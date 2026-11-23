#include "services/UserService.hpp"
#include "services/SaveData.hpp"
#include "services/NpTrophy.hpp"
#include "services/PlayGo.hpp"
#include "common/Types.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

using namespace Sailor;

extern "C" {

// =============================================================================
// libSceUserService Exports
// =============================================================================

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceInitialize(const SceUserServiceInitializeParams* params) {
    return UserServiceManager::Instance().Initialize(params);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceTerminate(void) {
    return UserServiceManager::Instance().Terminate();
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceGetInitialUser(SceUserServiceUserId* userId) {
    return UserServiceManager::Instance().GetInitialUser(userId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList* list) {
    return UserServiceManager::Instance().GetLoginUserIdList(list);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceGetUserName(SceUserServiceUserId userId, char* userName, size_t size) {
    return UserServiceManager::Instance().GetUserName(userId, userName, size);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceGetForegroundUser(SceUserServiceUserId* userId) {
    return UserServiceManager::Instance().GetForegroundUser(userId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceGetUserIdByPadId(i32 padId, SceUserServiceUserId* userId) {
    return UserServiceManager::Instance().GetUserIdByPadId(padId, userId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceGetPadIdByUserId(SceUserServiceUserId userId, i32* padId) {
    return UserServiceManager::Instance().GetPadIdByUserId(userId, padId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceRegisterEventCallback(const void* callback, void* userData) {
    (void)callback;
    (void)userData;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceUnregisterEventCallback(const void* callback) {
    (void)callback;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceUserServiceGetEvent(SceUserServiceEvent* event) {
    if (event) {
        event->eventType = 0;
        event->userId = SCE_USER_SERVICE_STATIC_USER_ID;
    }
    return SCE_OK;
}

// =============================================================================
// libSceSaveData Exports
// =============================================================================

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataInitialize(const void* params) {
    return SaveDataManager::Instance().Initialize(params);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataInitialize2(const void* params) {
    return SaveDataManager::Instance().Initialize2(params);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataTerminate(void) {
    return SaveDataManager::Instance().Terminate();
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataMount(const SceSaveDataMount* mount, SceSaveDataMountResult* result) {
    return SaveDataManager::Instance().Mount(mount, result);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataMount2(const SceSaveDataMount2* mount, SceSaveDataMountResult* result) {
    return SaveDataManager::Instance().Mount2(mount, result);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataUmount(const SceSaveDataMountPoint* mountPoint) {
    return SaveDataManager::Instance().Umount(mountPoint);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataDelete(const void* deleteParam) {
    return SaveDataManager::Instance().Delete(deleteParam);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataGetSaveDataMemory(const void* param, void* outMem) {
    (void)param;
    (void)outMem;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataDirNameSearch(const SceSaveDataDirNameSearchCond* cond, SceSaveDataDirNameSearchResult* result) {
    return SaveDataManager::Instance().DirNameSearch(cond, result);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataSave(const SceSaveDataMountPoint* mountPoint, const void* param) {
    (void)mountPoint;
    (void)param;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceSaveDataLoad(const SceSaveDataMountPoint* mountPoint, void* param) {
    (void)mountPoint;
    (void)param;
    return SCE_OK;
}

// =============================================================================
// libSceNpTrophy2 Exports
// =============================================================================

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyCreateContext(SceNpTrophyContext* context, const void* options) {
    return NpTrophyManager::Instance().CreateContext(context, options);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyDestroyContext(SceNpTrophyContext context) {
    return NpTrophyManager::Instance().DestroyContext(context);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle) {
    return NpTrophyManager::Instance().CreateHandle(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle) {
    return NpTrophyManager::Instance().DestroyHandle(handle);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyId trophyId, u64* platinumId) {
    return NpTrophyManager::Instance().UnlockTrophy(context, handle, trophyId, platinumId);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyFlagArray* flags, u32* count) {
    return NpTrophyManager::Instance().GetTrophyUnlockState(context, handle, flags, count);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyShowTrophyList(SceNpTrophyContext context, SceNpTrophyHandle handle) {
    (void)context;
    (void)handle;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyRegisterContext(SceNpTrophyContext context, const void* opt) {
    (void)context;
    (void)opt;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophySetScreenshot(SceNpTrophyContext context, SceNpTrophyHandle handle, const void* param) {
    (void)context;
    (void)handle;
    (void)param;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 sceNpTrophyGetProgress(SceNpTrophyContext context, SceNpTrophyHandle handle, u32 trophyId, void* progress) {
    (void)context;
    (void)handle;
    (void)trophyId;
    (void)progress;
    return SCE_OK;
}

// =============================================================================
// libScePlayGo Exports
// =============================================================================

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoInitialize(const ScePlayGoInitParams* params) {
    return PlayGoManager::Instance().Initialize(params);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoTerminate(void) {
    return PlayGoManager::Instance().Terminate();
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoGetChunkStatus(u64 a1, u64 a2, u64 a3) {
    ScePlayGoChunkStatus* status = nullptr;
    u32 chunkId = 0;
    if (a3 > 0x10000ULL && a3 < 0x7FFFFFFFFFFFLL) {
        chunkId = static_cast<u32>(a2);
        status = reinterpret_cast<ScePlayGoChunkStatus*>(a3);
    } else if (a2 > 0x10000ULL && a2 < 0x7FFFFFFFFFFFLL) {
        chunkId = static_cast<u32>(a1);
        status = reinterpret_cast<ScePlayGoChunkStatus*>(a2);
    }
    return PlayGoManager::Instance().GetChunkStatus(chunkId, status);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoGetProgress(u64 a1, u64 a2, u64 a3, u64 a4) {
    ScePlayGoProgress* progress = nullptr;
    if (a4 > 0x10000ULL && a4 < 0x7FFFFFFFFFFFLL) {
        progress = reinterpret_cast<ScePlayGoProgress*>(a4);
    } else if (a1 > 0x10000ULL && a1 < 0x7FFFFFFFFFFFLL) {
        progress = reinterpret_cast<ScePlayGoProgress*>(a1);
    }
    return PlayGoManager::Instance().GetProgress(static_cast<i32>(a1), reinterpret_cast<const u32*>(a2), static_cast<u32>(a3), progress);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoGetInstallSpeed(u64* speedBytesPerSec) {
    return PlayGoManager::Instance().GetInstallSpeed(speedBytesPerSec);
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoSetLanguage(const char* lang) {
    (void)lang;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoOpen(const void* opt) {
    (void)opt;
    return 1; // PlayGo handle 1
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoClose(i32 handle) {
    (void)handle;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoPrefetch(i32 handle, const u32* chunkIds, u32 count) {
    (void)handle;
    (void)chunkIds;
    (void)count;
    return SCE_OK;
}

SAILOR_EXPORT SAILOR_SYSV_ABI i32 scePlayGoGetLocus(u64 a1, u64 a2, u64 a3) {
    u32* locus = nullptr;
    u32 chunkId = 0;
    if (a3 > 0x10000ULL && a3 < 0x7FFFFFFFFFFFLL) {
        chunkId = static_cast<u32>(a2);
        locus = reinterpret_cast<u32*>(a3);
    } else if (a2 > 0x10000ULL && a2 < 0x7FFFFFFFFFFFLL) {
        chunkId = static_cast<u32>(a1);
        locus = reinterpret_cast<u32*>(a2);
    }
    return PlayGoManager::Instance().GetLocus(chunkId, locus);
}

} // extern "C"
