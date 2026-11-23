#pragma once

#include "common/Types.hpp"
#include <mutex>
#include <memory>
#include <unordered_map>

namespace Sailor {

struct DirectMemoryBlock {
    u64 physicalOffset{0};
    SceSize size{0};
    int memoryType{0};
    void* hSection{nullptr};
};

struct HostBaseBlock {
    void* baseAddress{nullptr};
    SceSize mappedSize{0};
    bool isFileMapping{true};
};

struct MemorySlice {
    void* userAddress{nullptr};
    SceSize size{0};
    std::shared_ptr<HostBaseBlock> hostBlock;
};

struct SceKernelBatchMapEntry {
    u64 vaddr{0};
    u64 offset{0};
    u64 length{0};
    u32 prot{0};
    i32 result{0};
};

class DirectMemoryManager {
public:
    static DirectMemoryManager& Instance();

    i32 Allocate(SceSize size, SceSize alignment, int memoryType, u64* outPhysicalOffset);
    i32 Release(u64 physicalOffset);

    i32 Map(void** outVirtualAddress, SceSize size, int protection, int flags, u64 physicalOffset, SceSize alignment);
    i32 Unmap(void* virtualAddress, SceSize size);
    i32 Mprotect(void* addr, SceSize size, int prot);

    i32 ReserveVirtualRange(void** outVirtualAddress, SceSize size, int flags, SceSize alignment);
    i32 MapFlexibleMemory(void** outVirtualAddress, SceSize size, int protection, int flags);
    i32 BatchMap(SceKernelBatchMapEntry* entries, u32 count, u32* outCount);

private:
    DirectMemoryManager() = default;

    std::mutex m_mutex;
    u64 m_nextPhysicalOffset{0x100000000ULL}; // 4GB virtual physical base
    std::unordered_map<u64, DirectMemoryBlock> m_allocatedBlocks;
    std::unordered_map<void*, MemorySlice> m_virtualSlices;
};

} // namespace Sailor
