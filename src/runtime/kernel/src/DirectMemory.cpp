#include "kernel/DirectMemory.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Sailor {

DirectMemoryManager& DirectMemoryManager::Instance() {
    static DirectMemoryManager instance;
    return instance;
}

i32 DirectMemoryManager::Allocate(SceSize size, SceSize alignment, int memoryType, u64* outPhysicalOffset) {
    if (!outPhysicalOffset) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (size == 0) {
        size = 0x10000;
    }

    std::lock_guard lock(m_mutex);

    // Align size to 64KB page boundary
    SceSize pageMask = 0xFFFF;
    SceSize alignedSize = (size + pageMask) & ~pageMask;

    u64 offset = m_nextPhysicalOffset;
    if (alignment > 0) {
        offset = (offset + alignment - 1) & ~(alignment - 1);
    }
    m_nextPhysicalOffset = offset + alignedSize;

    DirectMemoryBlock block;
    block.physicalOffset = offset;
    block.size = alignedSize;
    block.memoryType = memoryType;

#ifdef _WIN32
    DWORD sizeHigh = static_cast<DWORD>((alignedSize >> 32) & 0xFFFFFFFF);
    DWORD sizeLow = static_cast<DWORD>(alignedSize & 0xFFFFFFFF);
    HANDLE hMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_EXECUTE_READWRITE | SEC_COMMIT,
        sizeHigh,
        sizeLow,
        nullptr
    );
    if (!hMapping || hMapping == INVALID_HANDLE_VALUE) {
        SB_LOG_ERROR("DirectMemory", "CreateFileMappingW failed for size 0x{:X}", alignedSize);
        return SCE_KERNEL_ERROR_ENOMEM;
    }
    block.hSection = hMapping;
#endif

    m_allocatedBlocks[offset] = block;
    *outPhysicalOffset = offset;

    SB_LOG_DEBUG("DirectMemory", "Allocated DirectMemory: size=0x{:X}, align=0x{:X}, offset=0x{:X}",
        size, alignment, offset);
    return SCE_OK;
}

i32 DirectMemoryManager::Release(u64 physicalOffset) {
    std::lock_guard lock(m_mutex);
    auto it = m_allocatedBlocks.find(physicalOffset);
    if (it == m_allocatedBlocks.end()) {
        for (auto iter = m_allocatedBlocks.begin(); iter != m_allocatedBlocks.end(); ++iter) {
            if (physicalOffset >= iter->second.physicalOffset &&
                physicalOffset < iter->second.physicalOffset + iter->second.size) {
                it = iter;
                break;
            }
        }
    }
    if (it == m_allocatedBlocks.end()) {
        SB_LOG_WARN("DirectMemory", "Released DirectMemory at offset 0x{:X} (offset not tracked)", physicalOffset);
        return SCE_OK;
    }

#ifdef _WIN32
    if (it->second.hSection) {
        CloseHandle(static_cast<HANDLE>(it->second.hSection));
        it->second.hSection = nullptr;
    }
#endif

    m_allocatedBlocks.erase(it);
    SB_LOG_INFO("DirectMemory", "Released DirectMemory at offset 0x{:X}", physicalOffset);
    return SCE_OK;
}

i32 DirectMemoryManager::Map(void** outVirtualAddress, SceSize size, int protection, int flags, u64 physicalOffset, SceSize alignment) {
    if (!outVirtualAddress) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (size == 0) {
        size = 0x10000;
    }

    std::lock_guard lock(m_mutex);

    // Locate enclosing allocation block to support sub-block offset slicing
    const DirectMemoryBlock* foundBlock = nullptr;
    for (const auto& [baseOff, blk] : m_allocatedBlocks) {
        if (physicalOffset >= blk.physicalOffset && (physicalOffset + size) <= (blk.physicalOffset + blk.size)) {
            foundBlock = &blk;
            break;
        }
    }

    if (!foundBlock) {
        SB_LOG_ERROR("DirectMemory", "Cannot map unknown physical offset 0x{:X} (size 0x{:X})", physicalOffset, size);
        return SCE_KERNEL_ERROR_EINVAL;
    }

    (void)protection;
    (void)flags;
    (void)alignment;

#ifdef _WIN32
    void* targetAddr = ((flags & 0x10 /* SCE_KERNEL_MAP_FIXED */) && *outVirtualAddress) ? *outVirtualAddress : nullptr;
    DWORD access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;

    u64 subOffset = physicalOffset - foundBlock->physicalOffset;
    u64 alignedOffset = subOffset & ~0xFFFFULL; // Align down to 64 KB host granularity
    u64 delta = subOffset - alignedOffset;
    SceSize mapSize = size + static_cast<SceSize>(delta);

    // Subtract delta from targetAddr so static_cast<u8*>(ptr) + delta lands exactly on targetAddr
    void* alignedTargetAddr = targetAddr ? (static_cast<u8*>(targetAddr) - delta) : nullptr;

    // Check if targetAddr/alignedTargetAddr is already enclosed within an existing mapping view
    // to avoid overlapping view handles on Windows
    if (alignedTargetAddr) {
        for (const auto& [uAddr, slice] : m_virtualSlices) {
            u8* base = static_cast<u8*>(slice.hostBlock->baseAddress);
            u8* target = static_cast<u8*>(alignedTargetAddr);
            if (target >= base && (target + mapSize) <= (base + slice.hostBlock->mappedSize)) {
                void* userVirtAddr = static_cast<u8*>(targetAddr);
                MemorySlice newSlice;
                newSlice.userAddress = userVirtAddr;
                newSlice.size = size;
                newSlice.hostBlock = slice.hostBlock;
                m_virtualSlices[userVirtAddr] = newSlice;
                *outVirtualAddress = userVirtAddr;
                SB_LOG_INFO("DirectMemory", "Reusing existing view for sub-allocation offset 0x{:X} -> host virtual address {:p} (size 0x{:X})",
                    physicalOffset, userVirtAddr, size);
                return SCE_OK;
            }
        }
    }

    DWORD offsetHigh = static_cast<DWORD>((alignedOffset >> 32) & 0xFFFFFFFF);
    DWORD offsetLow = static_cast<DWORD>(alignedOffset & 0xFFFFFFFF);

    void* ptr = MapViewOfFileEx(
        static_cast<HANDLE>(foundBlock->hSection),
        access,
        offsetHigh,
        offsetLow,
        mapSize,
        alignedTargetAddr
    );
    if (!ptr && alignedTargetAddr) {
        ptr = MapViewOfFileEx(
            static_cast<HANDLE>(foundBlock->hSection),
            access,
            offsetHigh,
            offsetLow,
            mapSize,
            nullptr
        );
    }
#else
    void* targetAddr = ((flags & 0x10 /* SCE_KERNEL_MAP_FIXED */) && *outVirtualAddress) ? *outVirtualAddress : nullptr;
    u64 subOffset = physicalOffset - foundBlock->physicalOffset;
    u64 alignedOffset = subOffset & ~0xFFFFULL;
    u64 delta = subOffset - alignedOffset;
    SceSize mapSize = size + static_cast<SceSize>(delta);
    void* alignedTargetAddr = targetAddr ? (static_cast<u8*>(targetAddr) - delta) : nullptr;
    if (alignedTargetAddr) {
        for (const auto& [uAddr, slice] : m_virtualSlices) {
            u8* base = static_cast<u8*>(slice.hostBlock->baseAddress);
            u8* target = static_cast<u8*>(alignedTargetAddr);
            if (target >= base && (target + mapSize) <= (base + slice.hostBlock->mappedSize)) {
                void* userVirtAddr = static_cast<u8*>(targetAddr);
                MemorySlice newSlice;
                newSlice.userAddress = userVirtAddr;
                newSlice.size = size;
                newSlice.hostBlock = slice.hostBlock;
                m_virtualSlices[userVirtAddr] = newSlice;
                *outVirtualAddress = userVirtAddr;
                return SCE_OK;
            }
        }
    }
    void* ptr = mmap(alignedTargetAddr, mapSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (ptr == MAP_FAILED) ptr = nullptr;
#endif

    if (!ptr) {
        SB_LOG_ERROR("DirectMemory", "Host MapViewOfFileEx failed for size 0x{:X}", size);
        return SCE_KERNEL_ERROR_ENOMEM;
    }

    void* userVirtAddr = static_cast<u8*>(ptr) + delta;
    auto hostBlock = std::make_shared<HostBaseBlock>();
    hostBlock->baseAddress = ptr;
    hostBlock->mappedSize = mapSize;
    hostBlock->isFileMapping = true;

    MemorySlice slice;
    slice.userAddress = userVirtAddr;
    slice.size = size;
    slice.hostBlock = hostBlock;
    m_virtualSlices[userVirtAddr] = slice;
    *outVirtualAddress = userVirtAddr;

    SB_LOG_INFO("DirectMemory", "Mapped DirectMemory offset 0x{:X} -> host virtual address {:p} (size 0x{:X}, base {:p}, delta 0x{:X})",
        physicalOffset, userVirtAddr, size, ptr, delta);
    return SCE_OK;
}

i32 DirectMemoryManager::Unmap(void* virtualAddress, SceSize size) {
    (void)size;
    if (!virtualAddress) return SCE_KERNEL_ERROR_EINVAL;

    std::lock_guard lock(m_mutex);
    auto it = m_virtualSlices.find(virtualAddress);
    if (it == m_virtualSlices.end()) {
        // Fallback: search if virtualAddress falls inside any slice's user address range [userAddress, userAddress + size)
        for (auto iter = m_virtualSlices.begin(); iter != m_virtualSlices.end(); ++iter) {
            u8* uBase = static_cast<u8*>(iter->second.userAddress);
            u8* target = static_cast<u8*>(virtualAddress);
            if (target >= uBase && target < uBase + iter->second.size) {
                it = iter;
                break;
            }
        }
    }
    if (it == m_virtualSlices.end()) {
        // Second fallback: if caller passed host base address, match if slice references that host base
        for (auto iter = m_virtualSlices.begin(); iter != m_virtualSlices.end(); ++iter) {
            if (iter->second.hostBlock && iter->second.hostBlock->baseAddress == virtualAddress) {
                it = iter;
                break;
            }
        }
    }
    if (it == m_virtualSlices.end()) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    auto hostBlock = it->second.hostBlock;
    m_virtualSlices.erase(it);

    // If no other slices are referencing this host base block, release host virtual memory
    if (hostBlock && hostBlock.use_count() == 1) {
#ifdef _WIN32
        if (hostBlock->isFileMapping) {
            UnmapViewOfFile(hostBlock->baseAddress);
        } else if (!VirtualFree(hostBlock->baseAddress, 0, MEM_RELEASE)) {
            VirtualFree(hostBlock->baseAddress, hostBlock->mappedSize, MEM_DECOMMIT);
        }
#else
        munmap(hostBlock->baseAddress, hostBlock->mappedSize);
#endif
    }

    SB_LOG_DEBUG("DirectMemory", "Unmapped host virtual address {:p}", virtualAddress);
    return SCE_OK;
}

i32 DirectMemoryManager::Mprotect(void* addr, SceSize size, int prot) {
    if (!addr) return SCE_KERNEL_ERROR_EINVAL;
    if (size == 0) size = 0x10000;

#ifdef _WIN32
    DWORD winProt = PAGE_NOACCESS;
    constexpr int PROT_READ  = 1;
    constexpr int PROT_WRITE = 2;
    constexpr int PROT_EXEC  = 4;

    if (prot & PROT_EXEC) {
        winProt = (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE
                : (prot & PROT_READ)  ? PAGE_EXECUTE_READ
                : PAGE_EXECUTE;
    } else if (prot & PROT_WRITE) {
        winProt = PAGE_READWRITE;
    } else if (prot & PROT_READ) {
        winProt = PAGE_READONLY;
    } else if (prot & 0x30 /* SCE_KERNEL_PROT_GPU_READ | SCE_KERNEL_PROT_GPU_WRITE */) {
        winProt = PAGE_READWRITE;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(addr, size, winProt, &oldProt)) {
        SB_LOG_ERROR("DirectMemory", "VirtualProtect failed for {:p} (size 0x{:X})", addr, size);
        return SCE_KERNEL_ERROR_EFAULT;
    }
#else
    if (mprotect(addr, size, prot) != 0) {
        return SCE_KERNEL_ERROR_EFAULT;
    }
#endif

    SB_LOG_DEBUG("DirectMemory", "Changed memory protection {:p} (size 0x{:X}) to prot=0x{:X}", addr, size, prot);
    return SCE_OK;
}

i32 DirectMemoryManager::ReserveVirtualRange(void** outVirtualAddress, SceSize size, int flags, SceSize alignment) {
    if (!outVirtualAddress) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (size == 0) {
        size = 0x10000;
    }
    std::lock_guard lock(m_mutex);

    SceSize pageMask = 0xFFFF; // 64KB page granularity
    SceSize alignedSize = (size + pageMask) & ~pageMask;

    void* baseAddr = nullptr;
#ifdef _WIN32
    baseAddr = VirtualAlloc(nullptr, alignedSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!baseAddr) {
        SB_LOG_ERROR("DirectMemory", "ReserveVirtualRange failed for size 0x{:X}", alignedSize);
        return SCE_KERNEL_ERROR_ENOMEM;
    }
#else
    baseAddr = mmap(nullptr, alignedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (baseAddr == MAP_FAILED) return SCE_KERNEL_ERROR_ENOMEM;
#endif

    auto hostBlock = std::make_shared<HostBaseBlock>();
    hostBlock->baseAddress = baseAddr;
    hostBlock->mappedSize = alignedSize;
    hostBlock->isFileMapping = false;

    MemorySlice slice;
    slice.userAddress = baseAddr;
    slice.size = alignedSize;
    slice.hostBlock = hostBlock;
    m_virtualSlices[baseAddr] = slice;

    *outVirtualAddress = baseAddr;
    SB_LOG_INFO("DirectMemory", "Reserved virtual address range {:p} (size 0x{:X}, flags=0x{:X}, align=0x{:X})",
        baseAddr, alignedSize, flags, alignment);
    return SCE_OK;
}

i32 DirectMemoryManager::MapFlexibleMemory(void** outVirtualAddress, SceSize size, int protection, int flags) {
    if (!outVirtualAddress) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (size == 0) {
        size = 0x10000;
    }
    std::lock_guard lock(m_mutex);

    SceSize pageMask = 0xFFFF;
    SceSize alignedSize = (size + pageMask) & ~pageMask;

    void* target = *outVirtualAddress;
    void* resultAddr = nullptr;

#ifdef _WIN32
    DWORD winProt = PAGE_EXECUTE_READWRITE;
    resultAddr = VirtualAlloc(target, alignedSize, MEM_COMMIT | (target ? 0 : MEM_RESERVE), winProt);
    if (!resultAddr) {
        resultAddr = VirtualAlloc(nullptr, alignedSize, MEM_COMMIT | MEM_RESERVE, winProt);
    }
    if (!resultAddr) {
        SB_LOG_ERROR("DirectMemory", "MapFlexibleMemory failed for size 0x{:X}", alignedSize);
        return SCE_KERNEL_ERROR_ENOMEM;
    }
#else
    int mprot = PROT_READ | PROT_WRITE | PROT_EXEC;
    resultAddr = mmap(target, alignedSize, mprot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (resultAddr == MAP_FAILED) return SCE_KERNEL_ERROR_ENOMEM;
#endif

    auto hostBlock = std::make_shared<HostBaseBlock>();
    hostBlock->baseAddress = resultAddr;
    hostBlock->mappedSize = alignedSize;
    hostBlock->isFileMapping = false;

    MemorySlice slice;
    slice.userAddress = resultAddr;
    slice.size = alignedSize;
    slice.hostBlock = hostBlock;
    m_virtualSlices[resultAddr] = slice;

    *outVirtualAddress = resultAddr;
    SB_LOG_INFO("DirectMemory", "Mapped flexible memory {:p} (size 0x{:X}, prot=0x{:X}, flags=0x{:X})",
        resultAddr, alignedSize, protection, flags);
    return SCE_OK;
}

i32 DirectMemoryManager::BatchMap(SceKernelBatchMapEntry* entries, u32 count, u32* outCount) {
    if (!entries && count > 0) return SCE_KERNEL_ERROR_EINVAL;
    std::lock_guard lock(m_mutex);

    for (u32 i = 0; i < count; ++i) {
        auto& entry = entries[i];
        if (entry.vaddr != 0 && entry.length != 0) {
#ifdef _WIN32
            VirtualAlloc(reinterpret_cast<void*>(entry.vaddr), entry.length, MEM_COMMIT, PAGE_READWRITE);
#else
            mprotect(reinterpret_cast<void*>(entry.vaddr), entry.length, PROT_READ | PROT_WRITE);
#endif
        }
        entry.result = SCE_OK;
    }

    if (outCount) *outCount = count;
    SB_LOG_INFO("DirectMemory", "sceKernelBatchMap: processed {} entries", count);
    return SCE_OK;
}

} // namespace Sailor
