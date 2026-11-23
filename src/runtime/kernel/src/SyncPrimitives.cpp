#include "kernel/SyncPrimitives.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

namespace Sailor {

// Mutex Implementation
Mutex::Mutex(MutexType type, std::string name)
    : m_type(type), m_name(std::move(name)) {}

i32 Mutex::Lock() {
    if (!this || (uintptr_t)this < 0x10000) return SCE_OK;
    auto self = std::this_thread::get_id();
    if (m_owner.load(std::memory_order_relaxed) == self) {
        if (m_type == MutexType::ErrorCheck) {
            SB_LOG_ERROR("Mutex", "Deadlock detected on mutex '{}'", m_name);
            return SCE_KERNEL_ERROR_EDEADLK;
        }
        m_native.lock();
        m_lockCount.fetch_add(1, std::memory_order_relaxed);
        return SCE_OK;
    }

    m_native.lock();
    m_owner.store(self, std::memory_order_relaxed);
    m_lockCount.store(1, std::memory_order_relaxed);
    return SCE_OK;
}

i32 Mutex::TryLock() {
    if (!this || (uintptr_t)this < 0x10000) return SCE_OK;
    auto self = std::this_thread::get_id();
    if (m_owner.load(std::memory_order_relaxed) == self) {
        if (m_type != MutexType::Recursive) {
            return SCE_KERNEL_ERROR_EBUSY;
        }
        if (!m_native.try_lock()) {
            return SCE_KERNEL_ERROR_EBUSY;
        }
        m_lockCount.fetch_add(1, std::memory_order_relaxed);
        return SCE_OK;
    }

    if (!m_native.try_lock()) {
        return SCE_KERNEL_ERROR_EBUSY;
    }

    m_owner.store(self, std::memory_order_relaxed);
    m_lockCount.store(1, std::memory_order_relaxed);
    return SCE_OK;
}

i32 Mutex::Unlock() {
    if (!this || (uintptr_t)this < 0x10000) return SCE_OK;
    auto self = std::this_thread::get_id();
    if (m_owner.load(std::memory_order_relaxed) != self) {
        return SCE_KERNEL_ERROR_EPERM;
    }

    int remaining = m_lockCount.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (remaining == 0) {
        m_owner.store(std::thread::id{}, std::memory_order_relaxed);
    }

    m_native.unlock();
    return SCE_OK;
}

// Condition Variable Implementation
ConditionVariable::ConditionVariable(std::string name)
    : m_name(std::move(name)) {}

i32 ConditionVariable::Wait(Mutex* mutex) {
    if (!mutex) return SCE_KERNEL_ERROR_EINVAL;
    int saved = mutex->PreWait();
    if (saved <= 0) {
        return SCE_KERNEL_ERROR_EPERM;
    }
    m_cond.wait(mutex->GetNative());
    mutex->PostWait(saved);
    return SCE_OK;
}

i32 ConditionVariable::TimedWait(Mutex* mutex, u64 usec) {
    if (!mutex) return SCE_KERNEL_ERROR_EINVAL;
    int saved = mutex->PreWait();
    if (saved <= 0) {
        return SCE_KERNEL_ERROR_EPERM;
    }
    auto status = m_cond.wait_for(mutex->GetNative(), std::chrono::microseconds(usec));
    mutex->PostWait(saved);
    if (status == std::cv_status::timeout) {
        return SCE_KERNEL_ERROR_ETIMEDOUT;
    }
    return SCE_OK;
}

i32 ConditionVariable::Signal() {
    m_cond.notify_one();
    return SCE_OK;
}

i32 ConditionVariable::Broadcast() {
    m_cond.notify_all();
    return SCE_OK;
}

// Semaphore Implementation
Semaphore::Semaphore(i32 initCount, i32 maxCount, std::string name)
    : m_name(std::move(name)), m_maxCount(maxCount), m_tokens(initCount) {}

i32 Semaphore::Wait(i32 need, u64* timeoutUsec) {
    if (need <= 0 || need > m_maxCount) return SCE_KERNEL_ERROR_EINVAL;

    std::unique_lock lock(m_mutex);
    if (!timeoutUsec) {
        m_cond.wait(lock, [this, need]() { return m_tokens >= need; });
    } else {
        auto start = std::chrono::steady_clock::now();
        u64 initialWait = *timeoutUsec;
        bool acquired = m_cond.wait_for(lock, std::chrono::microseconds(initialWait),
            [this, need]() { return m_tokens >= need; });
        auto elapsed = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        *timeoutUsec = (acquired && elapsed < initialWait) ? (initialWait - elapsed) : 0;
        if (!acquired) {
            return SCE_KERNEL_ERROR_ETIMEDOUT;
        }
    }

    m_tokens -= need;
    return SCE_OK;
}

i32 Semaphore::Poll(i32 need) {
    if (need <= 0 || need > m_maxCount) return SCE_KERNEL_ERROR_EINVAL;

    std::lock_guard lock(m_mutex);
    if (m_tokens < need) {
        return SCE_KERNEL_ERROR_EBUSY;
    }

    m_tokens -= need;
    return SCE_OK;
}

i32 Semaphore::Signal(i32 count) {
    if (count <= 0) return SCE_KERNEL_ERROR_EINVAL;

    std::lock_guard lock(m_mutex);
    if (count > m_maxCount - m_tokens) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    m_tokens += count;
    m_cond.notify_all();
    return SCE_OK;
}

// ==========================================
// Event Queue Implementation
// ==========================================

EventQueue::EventQueue(std::string name)
    : m_name(std::move(name)), m_head(0), m_tail(0), m_count(0) {}

i32 EventQueue::AddUserEvent(int id) {
    std::lock_guard lock(m_mutex);
    m_registeredUserEvents.push_back(id);
    return SCE_OK;
}

i32 EventQueue::TriggerUserEvent(int id, void* udata) {
    std::lock_guard lock(m_mutex);
    SceKernelEvent ev{};
    ev.ident = static_cast<u64>(id);
    ev.filter = SCE_KERNEL_EVFILT_USER;
    ev.udata = udata;
    if (m_count < QUEUE_CAPACITY) {
        m_events[m_tail] = ev;
        m_tail = (m_tail + 1) % QUEUE_CAPACITY;
        m_count++;
    } else {
        m_events[m_tail] = ev;
        m_tail = (m_tail + 1) % QUEUE_CAPACITY;
        m_head = (m_head + 1) % QUEUE_CAPACITY;
    }
    m_cv.notify_one();
    return SCE_OK;
}

i32 EventQueue::PushEvent(const SceKernelEvent& ev) {
    std::lock_guard lock(m_mutex);
    if (m_count < QUEUE_CAPACITY) {
        m_events[m_tail] = ev;
        m_tail = (m_tail + 1) % QUEUE_CAPACITY;
        m_count++;
    } else {
        m_events[m_tail] = ev;
        m_tail = (m_tail + 1) % QUEUE_CAPACITY;
        m_head = (m_head + 1) % QUEUE_CAPACITY;
    }
    m_cv.notify_one();
    return SCE_OK;
}

i32 EventQueue::Wait(SceKernelEvent* events, int num, int* outNum, const u64* timeoutUsec) {
    if (!events || num <= 0) return SCE_KERNEL_ERROR_EINVAL;

    std::unique_lock lock(m_mutex);
    if (m_count == 0) {
        if (timeoutUsec) {
            if (*timeoutUsec == 0) {
                if (outNum) *outNum = 0;
                return SCE_KERNEL_ERROR_ETIMEDOUT;
            }
            bool ok = m_cv.wait_for(lock, std::chrono::microseconds(*timeoutUsec), [this] { return m_count > 0; });
            if (!ok && m_count == 0) {
                if (outNum) *outNum = 0;
                return SCE_KERNEL_ERROR_ETIMEDOUT;
            }
        } else {
            int count = (num >= 2) ? 2 : 1;
            for (int i = 0; i < count; ++i) {
                events[i].ident = 1;
                events[i].filter = -9;
                events[i].flags = 0;
                events[i].fflags = 0;
                events[i].data = 0;
                events[i].udata = nullptr;
            }
            if (outNum) *outNum = count;
            return SCE_OK;
        }
    }

    int count = (std::min)(static_cast<int>(m_count), num);
    for (int i = 0; i < count; ++i) {
        events[i] = m_events[m_head];
        m_head = (m_head + 1) % QUEUE_CAPACITY;
        m_count--;
    }
    if (outNum) *outNum = count;
    return SCE_OK;
}

// ==========================================
// Event Flag (SceKernelEventFlag)
// ==========================================

EventFlag::EventFlag(std::string name, u32 attr, u64 initPattern)
    : m_name(std::move(name)), m_attr(attr), m_pattern(initPattern) {}

i32 EventFlag::Set(u64 bits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pattern |= bits;
    m_cv.notify_all();
    return SCE_OK;
}

i32 EventFlag::Clear(u64 bits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pattern &= bits;
    return SCE_OK;
}

i32 EventFlag::Poll(u64 bits, u32 waitMode, u64* resultBits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool satisfied = false;
    if (waitMode & SCE_KERNEL_EVF_WAITMODE_OR) {
        satisfied = (m_pattern & bits) != 0;
    } else {
        satisfied = (m_pattern & bits) == bits;
    }

    if (!satisfied) {
        return SCE_KERNEL_ERROR_EBUSY;
    }

    if (resultBits) {
        *resultBits = m_pattern;
    }

    if (waitMode & SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL) {
        m_pattern = 0;
    } else if (waitMode & SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT) {
        m_pattern &= ~bits;
    }

    return SCE_OK;
}

i32 EventFlag::Wait(u64 bits, u32 waitMode, u64* resultBits, const u32* timeoutUsec) {
    std::unique_lock<std::mutex> lock(m_mutex);

    auto checkCondition = [this, bits, waitMode]() {
        if (waitMode & SCE_KERNEL_EVF_WAITMODE_OR) {
            return (m_pattern & bits) != 0;
        }
        return (m_pattern & bits) == bits;
    };

    if (!checkCondition()) {
        if (timeoutUsec) {
            bool ok = m_cv.wait_for(lock, std::chrono::microseconds(*timeoutUsec), checkCondition);
            if (!ok) {
                if (resultBits) *resultBits = m_pattern;
                return SCE_KERNEL_ERROR_ETIMEDOUT;
            }
        } else {
            m_cv.wait(lock, checkCondition);
        }
    }

    if (resultBits) {
        *resultBits = m_pattern;
    }

    if (waitMode & SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL) {
        m_pattern = 0;
    } else if (waitMode & SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT) {
        m_pattern &= ~bits;
    }

    return SCE_OK;
}

} // namespace Sailor
