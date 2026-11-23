#pragma once

#include "common/Types.hpp"
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <chrono>
#include <vector>

namespace Sailor {

// Mutex Types
enum class MutexType {
    Normal = 0,
    Recursive = 1,
    ErrorCheck = 2
};

struct ScePthreadMutexAttr {
    int type{static_cast<int>(MutexType::Normal)};
    int protocol{0};
    int prioceiling{0};
    char name[32]{0};
};

class Mutex {
public:
    explicit Mutex(MutexType type = MutexType::Normal, std::string name = "");

    i32 Lock();
    i32 TryLock();
    i32 Unlock();

    std::recursive_mutex& GetNative() { return m_native; }

    int PreWait() {
        if (!this || (uintptr_t)this < 0x10000) return 0;
        auto self = std::this_thread::get_id();
        if (m_owner.load(std::memory_order_relaxed) != self) {
            return 0;
        }
        int saved = m_lockCount.load(std::memory_order_relaxed);
        if (saved <= 0) return 0;
        m_owner.store(std::thread::id{}, std::memory_order_relaxed);
        m_lockCount.store(0, std::memory_order_relaxed);
        for (int i = 0; i < saved - 1; ++i) {
            m_native.unlock();
        }
        return saved;
    }

    void PostWait(int saved) {
        if (!this || (uintptr_t)this < 0x10000 || saved <= 0) return;
        for (int i = 0; i < saved - 1; ++i) {
            m_native.lock();
        }
        m_owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
        m_lockCount.store(saved, std::memory_order_relaxed);
    }

private:
    MutexType m_type{MutexType::Normal};
    std::string m_name;
    std::recursive_mutex m_native;
    std::atomic<std::thread::id> m_owner{};
    std::atomic<int> m_lockCount{0};
};

using ScePthreadMutex = Mutex*;

// Condition Variable
struct ScePthreadCondAttr {
    char name[32]{0};
};

class ConditionVariable {
public:
    explicit ConditionVariable(std::string name = "");

    i32 Wait(Mutex* mutex);
    i32 TimedWait(Mutex* mutex, u64 usec);
    i32 Signal();
    i32 Broadcast();

private:
    std::string m_name;
    std::condition_variable_any m_cond;
};

using ScePthreadCond = ConditionVariable*;

// Read-Write Lock
class RwLock {
public:
    explicit RwLock(std::string name = "") : m_name(std::move(name)) {}

    i32 LockRead() {
        m_native.lock_shared();
        m_readerCount.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    i32 TryLockRead() {
        if (m_native.try_lock_shared()) {
            m_readerCount.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        return 16 /* EBUSY */;
    }
    i32 LockWrite() {
        m_native.lock();
        m_writer.store(std::this_thread::get_id(), std::memory_order_relaxed);
        return 0;
    }
    i32 TryLockWrite() {
        if (m_native.try_lock()) {
            m_writer.store(std::this_thread::get_id(), std::memory_order_relaxed);
            return 0;
        }
        return 16 /* EBUSY */;
    }
    i32 Unlock() {
        if (m_writer.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
            m_writer.store(std::thread::id{}, std::memory_order_relaxed);
            m_native.unlock();
            return 0;
        }
        if (m_readerCount.load(std::memory_order_relaxed) > 0) {
            m_readerCount.fetch_sub(1, std::memory_order_relaxed);
            m_native.unlock_shared();
            return 0;
        }
        return 1 /* EPERM */;
    }

private:
    std::string m_name;
    std::shared_mutex m_native;
    std::atomic<std::thread::id> m_writer{std::thread::id{}};
    std::atomic<int> m_readerCount{0};
};

using ScePthreadRwlock = RwLock*;

// Semaphore
class Semaphore {
public:
    Semaphore(i32 initCount, i32 maxCount, std::string name);

    i32 Wait(i32 need, u64* timeoutUsec);
    i32 Poll(i32 need);
    i32 Signal(i32 count);

private:
    std::string m_name;
    i32 m_maxCount;
    i32 m_tokens;
    std::mutex m_mutex;
    std::condition_variable m_cond;
};

using SceKernelSema = Semaphore*;

// ==========================================
// Event Queue (SceKernelEqueue)
// ==========================================

constexpr i16 SCE_KERNEL_EVFILT_READ      = -1;
constexpr i16 SCE_KERNEL_EVFILT_WRITE     = -2;
constexpr i16 SCE_KERNEL_EVFILT_USER      = -7;
constexpr i16 SCE_KERNEL_EVFILT_GRAPHICS  = -8;
constexpr i16 SCE_KERNEL_EVFILT_VIDEO_OUT = -9;

struct SceKernelEvent {
    u64 ident{0};
    i16 filter{0};
    u16 flags{0};
    u32 fflags{0};
    i64 data{0};
    void* udata{nullptr};
};

class EventQueue {
public:
    explicit EventQueue(std::string name);

    i32 AddUserEvent(int id);
    i32 TriggerUserEvent(int id, void* udata);
    i32 PushEvent(const SceKernelEvent& ev);
    i32 Wait(SceKernelEvent* events, int num, int* outNum, const u64* timeoutUsec);

private:
    std::string m_name;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    static constexpr size_t QUEUE_CAPACITY = 1024;
    SceKernelEvent m_events[QUEUE_CAPACITY]{};
    size_t m_head{0};
    size_t m_tail{0};
    size_t m_count{0};
    std::vector<int> m_registeredUserEvents;
};

using SceKernelEqueue = EventQueue*;

// ==========================================
// Event Flag (SceKernelEventFlag)
// ==========================================

constexpr u32 SCE_KERNEL_EVF_WAITMODE_AND       = 0x01;
constexpr u32 SCE_KERNEL_EVF_WAITMODE_OR        = 0x02;
constexpr u32 SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL = 0x10;
constexpr u32 SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT = 0x20;

class EventFlag {
public:
    explicit EventFlag(std::string name, u32 attr, u64 initPattern);

    i32 Set(u64 bits);
    i32 Clear(u64 bits);
    i32 Poll(u64 bits, u32 waitMode, u64* resultBits);
    i32 Wait(u64 bits, u32 waitMode, u64* resultBits, const u32* timeoutUsec);

private:
    std::string m_name;
    u32 m_attr{0};
    u64 m_pattern{0};
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

using SceKernelEventFlag = EventFlag*;

} // namespace Sailor
