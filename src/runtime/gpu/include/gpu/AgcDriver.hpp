#pragma once

#include "common/Types.hpp"
#include "Pm4Decoder.hpp"
#include <mutex>
#include <atomic>
#include <memory>
#include <unordered_map>

namespace Sailor {

namespace Shader {
    class ShaderRecompiler;
    enum class ShaderStage : u32;
}

struct AgcSubmitInfo {
    const u32* dcbStart{nullptr};
    u32 dcbSizeDwords{0};
    u32 queueId{0};
};

struct AgcPipelineEntry {
    void* shaderModule{nullptr};
    void* pipeline{nullptr};
};

class AgcDriver {
public:
    static AgcDriver& Instance();

    i32 Initialize();

    i32 SubmitCommandBuffer(const u32* dcbStart, u32 dcbSizeDwords, u32 queueId);
    i32 SubmitAcb(const u32* acbStart, u32 acbSizeDwords, u32 queueId);

    Pm4Decoder& GetDecoder() { return m_decoder; }
    Pm4Decoder& GetComputeDecoder() { return m_computeDecoder; }

    void* GetOrCreateShaderModule(u64 guestAddr, Shader::ShaderStage stage);
    void SetActivePipeline(void* pipeline) { m_activePipeline = pipeline; }
    void SetActiveIndirectBuffer(void* buf) { m_activeIndirectBuffer = buf; }
    void* GetActivePipeline() const { return m_activePipeline; }

private:
    AgcDriver();

    void RecordDraw(const DrawCommand& cmd);
    void RecordDispatch(const DispatchCommand& cmd);
    void RecordComputeDispatch(const DispatchCommand& cmd);
    u64 GetShReg64(u32 regLo, u32 regHi) const;
    u64 GetComputeShReg64(u32 regLo, u32 regHi) const;

    std::mutex m_submitMutex;
    std::mutex m_computeMutex;
    Pm4Decoder m_decoder;
    Pm4Decoder m_computeDecoder;
    std::atomic<u64> m_submittedFrames{0};
    std::shared_ptr<Shader::ShaderRecompiler> m_recompiler;
    std::unordered_map<u64, AgcPipelineEntry> m_pipelineCache;
    void* m_activePipeline{nullptr};
    void* m_activeIndirectBuffer{nullptr};
};

} // namespace Sailor
