#pragma once

#include "common/Types.hpp"
#include <mutex>

namespace Sailor {

#pragma pack(push, 1)

struct ScePlayGoInitParams {
    u32 bufferSize{0};
};

struct ScePlayGoChunkStatus {
    u32 chunkId{0};
    float progress{100.0f}; // 100% installed
    u32 state{2}; // 0 = not installed, 1 = downloading, 2 = installed
    u8 reserved[16]{0};
};

struct ScePlayGoProgress {
    u64 installedBytes{50ULL * 1024 * 1024 * 1024}; // 50 GB (offset 0x00)
    u64 totalBytes{50ULL * 1024 * 1024 * 1024};     // 50 GB (offset 0x08)
    float totalProgress{100.0f};                    // 100% installed (offset 0x10)
    u8 reserved[28]{0};
};

#pragma pack(pop)

class SAILOR_EXPORT PlayGoManager {
public:
    static PlayGoManager& Instance();

    i32 Initialize(const ScePlayGoInitParams* params);
    i32 Terminate();

    i32 GetChunkStatus(u32 chunkId, ScePlayGoChunkStatus* status);
    i32 GetProgress(ScePlayGoProgress* progress) {
        return GetProgress(-1, nullptr, 0, progress);
    }
    i32 GetProgress(i32 handle, const u32* chunkIds, u32 numChunks, ScePlayGoProgress* progress);
    i32 GetInstallSpeed(u64* speedBytesPerSec);
    i32 GetLocus(u32 chunkId, u32* locus);

    bool IsInitialized() const { return m_initialized; }

private:
    PlayGoManager() = default;
    ~PlayGoManager() = default;

    std::mutex m_mutex;
    bool m_initialized{false};
};

} // namespace Sailor
