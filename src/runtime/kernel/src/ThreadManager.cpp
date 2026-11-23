#include "kernel/ThreadManager.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <cstring>
#include <cstdlib>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <immintrin.h>
#endif

namespace Sailor {

namespace {
thread_local ScePthread t_currentThreadId = 1;
thread_local ThreadState* t_currentThreadState = nullptr;
thread_local void* t_guestTcb = nullptr;

#ifdef _WIN32
using SysVTrampolineFn = void* (*)(void* entryFunc, void* arg);

SysVTrampolineFn GetSysVTrampoline() {
    static SysVTrampolineFn s_trampoline = nullptr;
    if (s_trampoline) return s_trampoline;

    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return nullptr;

    // Machine code for MS x64 -> System V AMD64 call:
    // RCX = entryFunc, RDX = arg
    const u8 code[] = {
        0x55,                               // push rbp
        0x48, 0x89, 0xE5,                   // mov rbp, rsp
        0x57,                               // push rdi
        0x56,                               // push rsi
        0x53,                               // push rbx
        0x41, 0x54,                         // push r12
        0x41, 0x55,                         // push r13
        0x41, 0x56,                         // push r14
        0x41, 0x57,                         // push r15
        0x48, 0x83, 0xEC, 0x28,             // sub rsp, 40 (32 bytes shadow space + 8 bytes alignment padding to ensure RSP % 16 == 0)
        0x49, 0x89, 0xCA,                   // mov r10, rcx (entry)
        0x48, 0x89, 0xD7,                   // mov rdi, rdx (arg in RDI for SysV!)
        0x48, 0x89, 0xD1,                   // mov rcx, rdx (arg in RCX for MS x64 compatibility)
        0x31, 0xF6,                         // xor esi, esi (arg2 = 0)
        0x31, 0xD2,                         // xor edx, edx (arg3 = 0)
        0x45, 0x31, 0xC0,                   // xor r8d, r8d (arg5 = 0)
        0x45, 0x31, 0xC9,                   // xor r9d, r9d (arg6 = 0)
        0x41, 0xFF, 0xD2,                   // call r10
        0x48, 0x83, 0xC4, 0x28,             // add rsp, 40
        0x41, 0x5F,                         // pop r15
        0x41, 0x5E,                         // pop r14
        0x41, 0x5D,                         // pop r13
        0x41, 0x5C,                         // pop r12
        0x5B,                               // pop rbx
        0x5E,                               // pop rsi
        0x5F,                               // pop rdi
        0x48, 0x89, 0xEC,                   // mov rsp, rbp
        0x5D,                               // pop rbp
        0xC3                                // ret
    };

    std::memcpy(mem, code, sizeof(code));
    DWORD oldProtect = 0;
    VirtualProtect(mem, sizeof(code), PAGE_EXECUTE_READ, &oldProtect);

    s_trampoline = reinterpret_cast<SysVTrampolineFn>(mem);
    return s_trampoline;
}

void* InvokeGuestThreadEntry(ScePthreadEntry entry, void* arg) {
    auto trampoline = GetSysVTrampoline();
    if (trampoline) {
        return trampoline(reinterpret_cast<void*>(entry), arg);
    }
    return entry(arg);
}
#else
void* InvokeGuestThreadEntry(ScePthreadEntry entry, void* arg) {
    return entry(arg);
}
#endif

static void* SafeReadPtr(const void* p) {
    if (!p) return nullptr;
#ifdef _WIN32
    __try {
        void* val = *(void**)p;
        return val;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    return *(void**)p;
#endif
}

} // namespace

ThreadManager& ThreadManager::Instance() {
    static ThreadManager instance;
    return instance;
}

i32 ThreadManager::CreateThread(ScePthread* thread, const ScePthreadAttr* attr, ScePthreadEntry entry, void* arg, const char* name) {
    if (!thread || !entry) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::string threadName = name ? name : "SailorThread";
    (void)attr;

    void* candidate = SafeReadPtr(arg);
    void* realEntry = (candidate > (void*)0x140000000 && candidate < (void*)0x150000000) ? candidate : (void*)entry;

    std::lock_guard lock(m_mutex);
    ScePthread threadId = m_nextThreadId++;
    *thread = threadId;

    SB_LOG_INFO("ThreadManager", "Creating guest thread #{} ('{}') with entry={:p} (real={:p})",
        threadId, threadName, reinterpret_cast<void*>(entry), realEntry);

    auto state = std::make_shared<ThreadState>();
    m_threads[threadId] = state;

    ThreadState* rawState = state.get();
    char nameBuf[64] = {};
    if (name) {
        std::strncpy(nameBuf, name, sizeof(nameBuf) - 1);
    } else {
        std::strncpy(nameBuf, "SailorThread", sizeof(nameBuf) - 1);
    }

    state->handle = std::thread([this, rawState, entry, arg, threadId, nameBuf]() {
        t_currentThreadId = threadId;
        t_currentThreadState = rawState;
        SetupCurrentThreadTls();

        struct ThreadTlsGuard {
            ThreadManager* mgr;
            u32 tid;
            ~ThreadTlsGuard() {
                mgr->CleanupCurrentThreadTls(t_guestTcb, tid);
                t_currentThreadState = nullptr;
            }
        } guard{this, ::GetCurrentThreadId()};

        SB_LOG_DEBUG("ThreadManager", "Thread #{} ('{}') started", threadId, nameBuf);
        rawState->exitValue = InvokeGuestThreadEntry(entry, arg);
        SB_LOG_DEBUG("ThreadManager", "Thread #{} ('{}') finished with result {:p}",
            threadId, nameBuf, rawState->exitValue);
    });

    return SCE_OK;
}

ScePthread ThreadManager::GetCurrentThreadId() const {
    return t_currentThreadId;
}

i32 ThreadManager::JoinThread(ScePthread thread, void** valuePtr) {
    std::shared_ptr<ThreadState> state;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_threads.find(thread);
        if (it == m_threads.end()) {
            return SCE_KERNEL_ERROR_ESRCH;
        }
        state = it->second;
        m_threads.erase(it);
    }

    if (state && state->handle.joinable()) {
        state->handle.join();
    }

    if (valuePtr && state) {
        *valuePtr = state->exitValue;
    }
    return SCE_OK;
}

void ThreadManager::ExitThread(void* value) {
    SB_LOG_DEBUG("ThreadManager", "Thread exit requested with value {:p}", value);
    if (t_currentThreadState) {
        t_currentThreadState->exitValue = value;
        t_currentThreadState = nullptr;
    }
#ifdef _WIN32
    void* tcb = t_guestTcb;
    u32 win32Tid = ::GetCurrentThreadId();
    CleanupCurrentThreadTls(tcb, win32Tid);
    ::ExitThread(0);
#else
    void* tcb = t_guestTcb;
    CleanupCurrentThreadTls(tcb);
    pthread_exit(value);
#endif
}

void ThreadManager::RegisterTlsTemplate(const void* templateData, SceSize fileSize, SceSize memSize, SceSize align) {
    std::lock_guard lock(m_mutex);
    m_tlsFileSize = fileSize;
    m_tlsMemSize = memSize;
    m_tlsAlign = (align > 0) ? align : 16;
    m_tlsTemplate.clear();
    if (templateData && fileSize > 0) {
        const u8* bytes = static_cast<const u8*>(templateData);
        m_tlsTemplate.assign(bytes, bytes + fileSize);
    }
    SB_LOG_INFO("ThreadManager", "Registered Guest TLS template (fileSize=0x{:X}, memSize=0x{:X}, align=0x{:X})",
        fileSize, memSize, m_tlsAlign);
}

void* ThreadManager::AllocateGuestTlsBlock() const {
    SceSize align = m_tlsAlign ? m_tlsAlign : 16;
    SceSize rawAlignedSize = (m_tlsMemSize + align - 1) & ~(align - 1);
    constexpr SceSize MinTlsSize = 0x20000; // 128 KB minimum guest TLS buffer
    SceSize tlsAlignedSize = (rawAlignedSize < MinTlsSize) ? MinTlsSize : rawAlignedSize;

    // Variant II TLS layout:
    // [ .tdata (filesz) | .tbss (memsz - filesz) | padding | TCB (offset 0: self-ptr) ]
    SceSize tcbSize = 4096;
    u8* block = static_cast<u8*>(std::calloc(1, tlsAlignedSize + tcbSize));
    if (!block) return nullptr;

    u8* tcbPtr = block + tlsAlignedSize;
    if (!m_tlsTemplate.empty() && m_tlsFileSize > 0 && rawAlignedSize > 0) {
        std::memcpy(tcbPtr - rawAlignedSize, m_tlsTemplate.data(),
                    (std::min)(m_tlsFileSize, static_cast<SceSize>(m_tlsTemplate.size())));
    }

    *reinterpret_cast<u64*>(tcbPtr) = reinterpret_cast<u64>(tcbPtr); // fs:[0] -> TCB self pointer

    return tcbPtr;
}

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_WIN32)
static bool WriteFsBaseSafe(u64 val) {
    __try {
        _writefsbase_u64(val);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif
#endif

void ThreadManager::SetupCurrentThreadTls() {
    void* tcb = AllocateGuestTlsBlock();
    if (!tcb) return;
    t_guestTcb = tcb;

#ifdef _WIN32
    u32 win32Tid = ::GetCurrentThreadId();
    {
        std::unique_lock lock(m_tcbMutex);
        m_win32TcbMap[win32Tid] = tcb;
    }
#endif

#if defined(_M_X64) || defined(__x86_64__)
    #if defined(_WIN32)
    if (WriteFsBaseSafe(reinterpret_cast<u64>(tcb))) {
        SB_LOG_DEBUG("ThreadManager", "Set %fs base to {:p} (Variant II TCB) for TID {}", tcb, win32Tid);
    } else {
        SB_LOG_WARN("ThreadManager", "FSGSBASE instruction trapped or unsupported on current CPU");
    }
    #endif
#endif
}

void ThreadManager::CleanupCurrentThreadTls(void* tcb, u32 win32ThreadId) {
    if (!tcb) return;
#ifdef _WIN32
    u32 tid = (win32ThreadId != 0) ? win32ThreadId : ::GetCurrentThreadId();
    {
        std::unique_lock lock(m_tcbMutex);
        m_win32TcbMap.erase(tid);
    }
#else
    (void)win32ThreadId;
#endif
    constexpr SceSize MinTlsSize = 0x20000;
    SceSize tlsAlignedSize = MinTlsSize;
    {
        std::lock_guard lock(m_mutex);
        SceSize align = m_tlsAlign ? m_tlsAlign : 16;
        SceSize rawAlignedSize = (m_tlsMemSize + align - 1) & ~(align - 1);
        tlsAlignedSize = (rawAlignedSize < MinTlsSize) ? MinTlsSize : rawAlignedSize;
    }
    u8* block = static_cast<u8*>(tcb) - tlsAlignedSize;
    std::free(block);
    if (t_guestTcb == tcb) {
        t_guestTcb = nullptr;
    }
}

void* ThreadManager::GetCurrentThreadTcb(u32 win32ThreadId) const {
#ifdef _WIN32
    if ((win32ThreadId == 0 || win32ThreadId == ::GetCurrentThreadId()) && t_guestTcb != nullptr) {
        return t_guestTcb;
    }
    u32 targetTid = (win32ThreadId != 0) ? win32ThreadId : ::GetCurrentThreadId();
    std::shared_lock lock(m_tcbMutex);
    auto it = m_win32TcbMap.find(targetTid);
    if (it != m_win32TcbMap.end()) {
        return it->second;
    }
#else
    if (t_guestTcb != nullptr) {
        return t_guestTcb;
    }
#endif
    return t_guestTcb;
}

} // namespace Sailor
