#pragma once

#include "common/Types.hpp"
#include <vector>
#include <unordered_set>
#include <mutex>
#include <string>

namespace Sailor {

using SceNpTrophyContext = i32;
using SceNpTrophyHandle = i32;
using SceNpTrophyId = u32;

constexpr u32 SCE_NP_TROPHY_FLAG_UNLOCKED = 0x00000001;

#pragma pack(push, 1)

struct SceNpTrophyFlagArray {
    u32 flagBits[4]{0};
};

struct SceNpTrophyDetails {
    SceNpTrophyId trophyId{0};
    u32 trophyGrade{1}; // 1 = Bronze, 2 = Silver, 3 = Gold, 4 = Platinum
    char name[128]{0};
    char description[256]{0};
    bool unlocked{false};
};

#pragma pack(pop)

class SAILOR_EXPORT NpTrophyManager {
public:
    static NpTrophyManager& Instance();

    i32 CreateContext(SceNpTrophyContext* context, const void* options);
    i32 DestroyContext(SceNpTrophyContext context);
    i32 CreateHandle(SceNpTrophyHandle* handle);
    i32 DestroyHandle(SceNpTrophyHandle handle);

    i32 UnlockTrophy(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyId trophyId, u64* platinumId);
    i32 GetTrophyUnlockState(SceNpTrophyContext context, SceNpTrophyHandle handle, SceNpTrophyFlagArray* flags, u32* count);
    bool IsTrophyUnlocked(SceNpTrophyId trophyId);
    size_t GetUnlockedTrophyCount();

private:
    NpTrophyManager() = default;
    ~NpTrophyManager() = default;

    std::mutex m_mutex;
    SceNpTrophyContext m_nextContext{1};
    SceNpTrophyHandle m_nextHandle{1};
    std::unordered_set<SceNpTrophyId> m_unlockedTrophies;
};

} // namespace Sailor
