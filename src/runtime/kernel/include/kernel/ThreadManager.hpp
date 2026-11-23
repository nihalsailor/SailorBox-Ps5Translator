#pragma once

#include "common/Types.hpp"
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <unordered_map>
#include <string>

namespace Sailor {

using ScePthread = u64;
using ScePthreadEntry = void* (SAILOR_SYSV_ABI *)(void*);

struct ScePthreadAttr {
    SceSize stackSize{0x100000}; // 1 MB
    int priority{0};
    int inheritSched{0};
    int schedPolicy{0};
    char name[32]{0};
};

struct ThreadState {
    std::thread handle;
    void* exitValue{nullptr};
};

class ThreadManager {
public:
    static ThreadManager& Instance();

    i32 CreateThread(ScePthread* thread, const ScePthreadAttr* attr, ScePthreadEntry entry, void* arg, const char* name);
    i32 JoinThread(ScePthread thread, void** valuePtr);
    void ExitThread(void* value);

    ScePthread GetCurrentThreadId() const;

    void RegisterTlsTemplate(const void* templateData, SceSize fileSize, SceSize memSize, SceSize align);
    void* AllocateGuestTlsBlock() const;
    void SetupCurrentThreadTls();
    void CleanupCurrentThreadTls(void* tcb, u32 win32ThreadId = 0);
    void* GetCurrentThreadTcb(u32 win32ThreadId = 0) const;

private:
    ThreadManager() = default;

    std::mutex m_mutex;
    mutable std::shared_mutex m_tcbMutex;
    std::unordered_map<u32, void*> m_win32TcbMap;
    ScePthread m_nextThreadId{1};
    std::unordered_map<ScePthread, std::shared_ptr<ThreadState>> m_threads;
    std::vector<u8> m_tlsTemplate;
    SceSize m_tlsFileSize{0};
    SceSize m_tlsMemSize{0};
    SceSize m_tlsAlign{16};
};

} // namespace Sailor
