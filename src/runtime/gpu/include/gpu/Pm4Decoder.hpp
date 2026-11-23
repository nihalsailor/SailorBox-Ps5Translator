#pragma once

#include "Pm4Opcodes.hpp"
#include <vector>
#include <span>
#include <array>
#include <functional>

namespace Sailor {

struct DrawCommand {
    bool isIndexed{false};
    bool isIndirect{false};
    u32 indexCount{0};
    u32 vertexCount{0};
    u32 instanceCount{1};
    u32 firstIndex{0};
    i32 vertexOffset{0};
    u32 firstInstance{0};
    u64 indexBufferAddress{0};
    u64 indirectBufferAddress{0};
    u32 indexType{0}; // 0 = 16-bit, 1 = 32-bit
};

struct DispatchCommand {
    bool isIndirect{false};
    u32 threadsX{1};
    u32 threadsY{1};
    u32 threadsZ{1};
    u64 indirectBufferAddress{0};
};

class Pm4Decoder {
public:
    using DrawCallback = std::function<void(const DrawCommand&)>;
    using DispatchCallback = std::function<void(const DispatchCommand&)>;
    using FlipCallback = std::function<void(u32 handle, i32 bufferIndex, u32 mode, i64 flipArg)>;

    Pm4Decoder();

    void SetDrawCallback(DrawCallback cb) { m_onDraw = std::move(cb); }
    void SetDispatchCallback(DispatchCallback cb) { m_onDispatch = std::move(cb); }
    void SetFlipCallback(FlipCallback cb) { m_onFlip = std::move(cb); }

    bool DecodeBuffer(std::span<const u32> dwords);

    u32 GetContextReg(u32 reg) const;
    u32 GetShReg(u32 reg) const;

private:
    void ProcessPacket(u32 header, u32 opcode, std::span<const u32> payload);

    std::vector<u32> m_contextRegs;
    std::vector<u32> m_shRegs;

    u64 m_indexBufferAddress{0};
    u32 m_indexBufferSize{0};
    u32 m_indexType{0}; // 0 = 16-bit, 1 = 32-bit
    u32 m_numInstances{1};

    DrawCallback m_onDraw;
    DispatchCallback m_onDispatch;
    FlipCallback m_onFlip;
};

} // namespace Sailor
