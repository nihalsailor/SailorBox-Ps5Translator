#include "services/PlayGo.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstring>

namespace Sailor {

PlayGoManager& PlayGoManager::Instance() {
    static PlayGoManager instance;
    return instance;
}

i32 PlayGoManager::Initialize(const ScePlayGoInitParams* params) {
    (void)params;
    std::lock_guard lock(m_mutex);
    m_initialized = true;
    SB_LOG_INFO("PlayGo", "Initialized libScePlayGo (All game chunks reported 100% installed)");
    return SCE_OK;
}

i32 PlayGoManager::Terminate() {
    std::lock_guard lock(m_mutex);
    m_initialized = false;
    SB_LOG_DEBUG("PlayGo", "Terminated libScePlayGo");
    return SCE_OK;
}

i32 PlayGoManager::GetChunkStatus(u32 chunkId, ScePlayGoChunkStatus* status) {
    if (!status) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    status->chunkId = chunkId;
    status->progress = 100.0f; // 100% installed
    status->state = 2;         // 2 = Installed
    std::memset(status->reserved, 0, sizeof(status->reserved));
    return SCE_OK;
}

i32 PlayGoManager::GetProgress(i32 handle, const u32* chunkIds, u32 numChunks, ScePlayGoProgress* progress) {
    (void)handle;
    (void)chunkIds;
    (void)numChunks;
    if (!progress) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    constexpr u64 kDefaultGameSize = 50ULL * 1024 * 1024 * 1024; // 50 GB
    progress->installedBytes = kDefaultGameSize;
    progress->totalBytes = kDefaultGameSize;
    progress->totalProgress = 100.0f;
    std::memset(progress->reserved, 0, sizeof(progress->reserved));
    return SCE_OK;
}

i32 PlayGoManager::GetInstallSpeed(u64* speedBytesPerSec) {
    if (!speedBytesPerSec) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    *speedBytesPerSec = 100'000'000ULL; // 100 MB/s
    return SCE_OK;
}

i32 PlayGoManager::GetLocus(u32 chunkId, u32* locus) {
    (void)chunkId;
    if (!locus) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    *locus = 0; // Local NVMe / Fast storage
    return SCE_OK;
}

} // namespace Sailor
