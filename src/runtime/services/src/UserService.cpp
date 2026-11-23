#include "services/UserService.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstring>
#include <algorithm>

namespace Sailor {

UserServiceManager& UserServiceManager::Instance() {
    static UserServiceManager instance;
    return instance;
}

i32 UserServiceManager::Initialize(const SceUserServiceInitializeParams* params) {
    (void)params;
    std::lock_guard lock(m_mutex);
    m_initialized = true;
    SB_LOG_INFO("UserService", "Initialized libSceUserService (Active User ID=1, Username='{}')", m_userName);
    return SCE_OK;
}

i32 UserServiceManager::Terminate() {
    std::lock_guard lock(m_mutex);
    m_initialized = false;
    SB_LOG_DEBUG("UserService", "Terminated libSceUserService");
    return SCE_OK;
}

i32 UserServiceManager::GetInitialUser(SceUserServiceUserId* userId) {
    if (!userId) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    std::lock_guard lock(m_mutex);
    *userId = m_activeUserId;
    SB_LOG_DEBUG("UserService", "GetInitialUser: returned userId={}", m_activeUserId);
    return SCE_OK;
}

i32 UserServiceManager::GetLoginUserIdList(SceUserServiceLoginUserIdList* list) {
    if (!list) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    std::lock_guard lock(m_mutex);
    list->userId[0] = m_activeUserId;
    for (size_t i = 1; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; ++i) {
        list->userId[i] = SCE_USER_SERVICE_USER_ID_INVALID;
    }
    return SCE_OK;
}

i32 UserServiceManager::GetUserName(SceUserServiceUserId userId, char* userName, size_t size) {
    if (!userName || size == 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    std::lock_guard lock(m_mutex);
    if (userId != m_activeUserId && userId != SCE_USER_SERVICE_STATIC_USER_ID) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    const size_t copyLen = std::min(size - 1, m_userName.size());
    std::memcpy(userName, m_userName.data(), copyLen);
    userName[copyLen] = '\0';
    return SCE_OK;
}

i32 UserServiceManager::GetForegroundUser(SceUserServiceUserId* userId) {
    return GetInitialUser(userId);
}

i32 UserServiceManager::GetUserIdByPadId(i32 padId, SceUserServiceUserId* userId) {
    (void)padId;
    return GetInitialUser(userId);
}

i32 UserServiceManager::GetPadIdByUserId(SceUserServiceUserId userId, i32* padId) {
    if (!padId) return SCE_KERNEL_ERROR_EINVAL;
    (void)userId;
    *padId = 0; // Pad index 0
    return SCE_OK;
}

} // namespace Sailor
