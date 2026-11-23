#include "services/NpTrophy.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstring>

namespace Sailor {

NpTrophyManager& NpTrophyManager::Instance() {
    static NpTrophyManager instance;
    return instance;
}

i32 NpTrophyManager::CreateContext(SceNpTrophyContext* context, const void* options) {
    if (!context) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    (void)options;
    std::lock_guard lock(m_mutex);
    *context = m_nextContext++;
    SB_LOG_INFO("NpTrophy", "Created NpTrophy Context handle={}", *context);
    return SCE_OK;
}

i32 NpTrophyManager::DestroyContext(SceNpTrophyContext context) {
    SB_LOG_DEBUG("NpTrophy", "Destroyed NpTrophy Context handle={}", context);
    return SCE_OK;
}

i32 NpTrophyManager::CreateHandle(SceNpTrophyHandle* handle) {
    if (!handle) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    std::lock_guard lock(m_mutex);
    *handle = m_nextHandle++;
    SB_LOG_DEBUG("NpTrophy", "Created NpTrophy Handle={}", *handle);
    return SCE_OK;
}

i32 NpTrophyManager::DestroyHandle(SceNpTrophyHandle handle) {
    SB_LOG_DEBUG("NpTrophy", "Destroyed NpTrophy Handle={}", handle);
    return SCE_OK;
}

i32 NpTrophyManager::UnlockTrophy(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyId trophyId, u64* platinumId) {
    (void)context;
    (void)handle;
    std::lock_guard lock(m_mutex);
    m_unlockedTrophies.insert(trophyId);
    if (platinumId) {
        *platinumId = 0;
    }
    SB_LOG_INFO("NpTrophy", "🏆 Unlocked Trophy ID={} (Total Unlocked: {})", trophyId, m_unlockedTrophies.size());
    return SCE_OK;
}

i32 NpTrophyManager::GetTrophyUnlockState(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyFlagArray* flags, u32* count) {
    (void)context;
    (void)handle;
    if (!flags || !count) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_mutex);
    std::memset(flags, 0, sizeof(SceNpTrophyFlagArray));
    for (SceNpTrophyId id : m_unlockedTrophies) {
        if (id < 128) {
            u32 word = id / 32;
            u32 bit = id % 32;
            flags->flagBits[word] |= (1u << bit);
        }
    }
    *count = static_cast<u32>(m_unlockedTrophies.size());
    return SCE_OK;
}

bool NpTrophyManager::IsTrophyUnlocked(SceNpTrophyId trophyId) {
    std::lock_guard lock(m_mutex);
    return m_unlockedTrophies.find(trophyId) != m_unlockedTrophies.end();
}

size_t NpTrophyManager::GetUnlockedTrophyCount() {
    std::lock_guard lock(m_mutex);
    return m_unlockedTrophies.size();
}

} // namespace Sailor
