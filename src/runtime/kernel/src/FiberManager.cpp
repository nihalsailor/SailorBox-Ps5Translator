#include "kernel/FiberManager.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sailor {

thread_local void* t_threadFiber = nullptr;
thread_local SceFiberInternal* t_currentRunningFiber = nullptr;

FiberManager& FiberManager::Instance() {
    static FiberManager instance;
    return instance;
}

FiberManager::FiberManager() = default;
FiberManager::~FiberManager() = default;

void FiberManager::EnsureThreadConverted() {
#ifdef _WIN32
    if (!t_threadFiber) {
        t_threadFiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        if (!t_threadFiber) {
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_FIBER) {
                t_threadFiber = GetCurrentFiber();
            }
        }
    }
#endif
}

void FiberManager::RegisterCurrentThreadAsFiber() {
    EnsureThreadConverted();
}

void __stdcall FiberManager::FiberEntryPoint(void* param) {
    auto* fiber = static_cast<SceFiberInternal*>(param);
    if (!fiber || !fiber->entry) return;

    while (true) {
        t_currentRunningFiber = fiber;
        fiber->state.store(1, std::memory_order_release); // Running

        u64 argInit = fiber->argOnInitialize;
        u64 argRun = fiber->lastRunArg;

        // Invoke guest fiber entry function
        fiber->entry(argInit, argRun);

        // If fiber entry returns, transition to Idle and return to thread
        fiber->state.store(0, std::memory_order_release);
        FiberManager::Instance().ReturnToThread(0, nullptr);
    }
}

i32 FiberManager::InitializeFiber(SceFiberInternal* fiber, const char* name, SceFiberEntry entry,
                                  u64 argOnInitialize, void* addrContext, u64 sizeContext) {
    if (!fiber || !entry) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    EnsureThreadConverted();

    fiber->magic = 0x5343454649424552ull;
    if (name) {
        std::strncpy(fiber->name, name, sizeof(fiber->name) - 1);
        fiber->name[sizeof(fiber->name) - 1] = '\0';
    } else {
        std::strcpy(fiber->name, "SceFiber");
    }

    fiber->entry = entry;
    fiber->argOnInitialize = argOnInitialize;
    fiber->contextMemory = addrContext;
    fiber->contextSize = sizeContext;
    fiber->state.store(0, std::memory_order_relaxed); // Idle
    fiber->callerFiber = nullptr;

#ifdef _WIN32
    size_t stackCommit = (sizeContext > 0 && sizeContext < 0x1000000) ? sizeContext : 0x100000;
    fiber->hostFiber = CreateFiberEx(stackCommit, stackCommit, FIBER_FLAG_FLOAT_SWITCH, FiberEntryPoint, fiber);
    if (!fiber->hostFiber) {
        SB_LOG_ERROR("Fiber", "Failed to create Win32 fiber for '{}'", fiber->name);
        return SCE_KERNEL_ERROR_ENOMEM;
    }
#endif

    SB_LOG_INFO("Fiber", "Initialized SceFiber '{}' at {:p} (hostFiber={:p})",
        fiber->name, static_cast<void*>(fiber), fiber->hostFiber);
    return SCE_OK;
}

i32 FiberManager::FinalizeFiber(SceFiberInternal* fiber) {
    if (!fiber || fiber->magic != 0x5343454649424552ull) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

#ifdef _WIN32
    if (fiber->hostFiber) {
        DeleteFiber(fiber->hostFiber);
        fiber->hostFiber = nullptr;
    }
#endif

    fiber->magic = 0;
    fiber->state.store(0, std::memory_order_release);
    SB_LOG_INFO("Fiber", "Finalized SceFiber '{}'", fiber->name);
    return SCE_OK;
}

i32 FiberManager::RunFiber(SceFiberInternal* fiber, u64 argOnRun, u64* argOnReturn) {
    if (!fiber || fiber->magic != 0x5343454649424552ull || !fiber->hostFiber) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    EnsureThreadConverted();

#ifdef _WIN32
    void* caller = GetCurrentFiber();
    fiber->callerFiber = caller ? caller : t_threadFiber;
    fiber->lastRunArg = argOnRun;

    t_currentRunningFiber = fiber;
    fiber->state.store(1, std::memory_order_release); // Running

    SwitchToFiber(fiber->hostFiber);

    // Returned back to this thread context
    t_currentRunningFiber = nullptr;
    if (argOnReturn) {
        *argOnReturn = fiber->returnArg;
    }
#endif

    return SCE_OK;
}

i32 FiberManager::SwitchFiber(SceFiberInternal* targetFiber, u64 argOnRun, u64* argOnRunOut) {
    if (!targetFiber || targetFiber->magic != 0x5343454649424552ull || !targetFiber->hostFiber) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    EnsureThreadConverted();

#ifdef _WIN32
    SceFiberInternal* current = t_currentRunningFiber;
    if (current) {
        current->state.store(2, std::memory_order_release); // Suspended
    }

    targetFiber->callerFiber = t_threadFiber;
    targetFiber->lastRunArg = argOnRun;

    t_currentRunningFiber = targetFiber;
    targetFiber->state.store(1, std::memory_order_release); // Running

    SwitchToFiber(targetFiber->hostFiber);

    // Resumed back into this fiber
    t_currentRunningFiber = current;
    if (current && argOnRunOut) {
        *argOnRunOut = current->lastRunArg;
    }
#endif

    return SCE_OK;
}

i32 FiberManager::ReturnToThread(u64 argOnReturn, u64* argOnRun) {
    EnsureThreadConverted();

#ifdef _WIN32
    SceFiberInternal* current = t_currentRunningFiber;
    if (!current) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    current->returnArg = argOnReturn;
    current->state.store(2, std::memory_order_release); // Suspended

    void* returnTarget = current->callerFiber ? current->callerFiber : t_threadFiber;
    if (!returnTarget) returnTarget = t_threadFiber;
    SwitchToFiber(returnTarget);

    // Resumed later
    if (argOnRun) {
        *argOnRun = current->lastRunArg;
    }
#endif

    return SCE_OK;
}

SceFiberInternal* FiberManager::GetCurrentFiber() {
    return t_currentRunningFiber;
}

} // namespace Sailor
