#pragma once

#include "common/Types.hpp"
#include <atomic>
#include <string>

namespace Sailor {

using SceFiberEntry = void (*)(u64 argOnInitialize, u64 argOnRun);

struct SceFiberInternal {
    u64 magic{0x5343454649424552ull}; // "SCEFIBER"
    char name[64]{0};
    SceFiberEntry entry{nullptr};
    u64 argOnInitialize{0};
    void* contextMemory{nullptr};
    u64 contextSize{0};
    void* hostFiber{nullptr};
    std::atomic<u32> state{0}; // 0 = Idle, 1 = Running, 2 = Suspended
    u64 lastRunArg{0};
    u64 returnArg{0};
    void* callerFiber{nullptr};
};

class FiberManager {
public:
    static FiberManager& Instance();

    i32 InitializeFiber(SceFiberInternal* fiber, const char* name, SceFiberEntry entry,
                        u64 argOnInitialize, void* addrContext, u64 sizeContext);
    i32 FinalizeFiber(SceFiberInternal* fiber);
    i32 RunFiber(SceFiberInternal* fiber, u64 argOnRun, u64* argOnReturn);
    i32 SwitchFiber(SceFiberInternal* fiber, u64 argOnRun, u64* argOnRunOut);
    i32 ReturnToThread(u64 argOnReturn, u64* argOnRun);
    SceFiberInternal* GetCurrentFiber();

    void RegisterCurrentThreadAsFiber();

private:
    FiberManager();
    ~FiberManager();

    void EnsureThreadConverted();
    static void __stdcall FiberEntryPoint(void* param);
};

} // namespace Sailor
