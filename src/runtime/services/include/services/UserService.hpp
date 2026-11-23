#pragma once

#include "common/Types.hpp"
#include <string>
#include <mutex>
#include <vector>

namespace Sailor {

using SceUserServiceUserId = i32;

constexpr SceUserServiceUserId SCE_USER_SERVICE_USER_ID_INVALID = -1;
constexpr SceUserServiceUserId SCE_USER_SERVICE_STATIC_USER_ID = 1;
constexpr size_t SCE_USER_SERVICE_MAX_LOGIN_USERS = 4;
constexpr size_t SCE_USER_SERVICE_MAX_USER_NAME_LENGTH = 16;

#pragma pack(push, 1)

struct SceUserServiceInitializeParams {
    u32 priority{0};
};

struct SceUserServiceLoginUserIdList {
    SceUserServiceUserId userId[SCE_USER_SERVICE_MAX_LOGIN_USERS]{
        SCE_USER_SERVICE_STATIC_USER_ID,
        SCE_USER_SERVICE_USER_ID_INVALID,
        SCE_USER_SERVICE_USER_ID_INVALID,
        SCE_USER_SERVICE_USER_ID_INVALID
    };
};

struct SceUserServiceEvent {
    i32 eventType{0};
    SceUserServiceUserId userId{SCE_USER_SERVICE_STATIC_USER_ID};
};

#pragma pack(pop)

class SAILOR_EXPORT UserServiceManager {
public:
    static UserServiceManager& Instance();

    i32 Initialize(const SceUserServiceInitializeParams* params);
    i32 Terminate();
    i32 GetInitialUser(SceUserServiceUserId* userId);
    i32 GetLoginUserIdList(SceUserServiceLoginUserIdList* list);
    i32 GetUserName(SceUserServiceUserId userId, char* userName, size_t size);
    i32 GetForegroundUser(SceUserServiceUserId* userId);
    i32 GetUserIdByPadId(i32 padId, SceUserServiceUserId* userId);
    i32 GetPadIdByUserId(SceUserServiceUserId userId, i32* padId);

    // Diagnostics / testing
    bool IsInitialized() const { return m_initialized; }
    void SetMockUserName(const std::string& name) { m_userName = name; }

private:
    UserServiceManager() = default;
    ~UserServiceManager() = default;

    std::mutex m_mutex;
    bool m_initialized{false};
    SceUserServiceUserId m_activeUserId{SCE_USER_SERVICE_STATIC_USER_ID};
    std::string m_userName{"SailorUser"};
};

} // namespace Sailor
