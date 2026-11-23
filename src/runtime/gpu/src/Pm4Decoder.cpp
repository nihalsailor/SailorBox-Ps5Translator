#include "gpu/Pm4Decoder.hpp"
#include "gpu/VideoOut.hpp"
#include "common/Logger.hpp"

#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>

namespace Sailor {

namespace {

inline bool SafeWriteMemoryU32(u64 dstAddr, u32 val) {
    if (dstAddr == 0 || dstAddr < 0x10000) return false;
#ifdef _WIN32
    __try {
        *reinterpret_cast<volatile u32*>(dstAddr) = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *reinterpret_cast<volatile u32*>(dstAddr) = val;
    return true;
#endif
}

inline bool SafeWriteMemoryU64(u64 dstAddr, u64 val) {
    if (dstAddr == 0 || dstAddr < 0x10000) return false;
#ifdef _WIN32
    __try {
        *reinterpret_cast<volatile u64*>(dstAddr) = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *reinterpret_cast<volatile u64*>(dstAddr) = val;
    return true;
#endif
}

} // namespace

Pm4Decoder::Pm4Decoder()
    : m_contextRegs(0x10000, 0)
    , m_shRegs(0x10000, 0) {
}

u32 Pm4Decoder::GetContextReg(u32 reg) const {
    return reg < m_contextRegs.size() ? m_contextRegs[reg] : 0;
}

u32 Pm4Decoder::GetShReg(u32 reg) const {
    return reg < m_shRegs.size() ? m_shRegs[reg] : 0;
}

bool Pm4Decoder::DecodeBuffer(std::span<const u32> dwords) {
    std::size_t offset = 0;
    while (offset < dwords.size()) {
        u32 header = dwords[offset++];
        if (header == 0) {
            // Trailing 0x00000000 NOP padding
            continue;
        }
        u32 type = (header >> 30) & 0x3;

        if (type == 3) {
            u32 countField = (header >> 16) & 0x3FFF;
            u32 opcode = (header >> 8) & 0xFF;
            // Per AMD PM4 Type-3 specification: COUNT field encodes (N - 1) body DWORDs
            u32 payloadCount = countField + 1;

            if (offset + payloadCount > dwords.size()) {
                SB_LOG_ERROR("Pm4Decoder", "Packet 0x{:02X} payload extends beyond buffer bounds (count={}, remaining={})",
                    opcode, payloadCount, dwords.size() - offset);
                return false;
            }

            std::span<const u32> payload(dwords.data() + offset, payloadCount);
            ProcessPacket(header, opcode, payload);
            offset += payloadCount;
        } else if (type == 2) {
            // Type-2 is NOP / filler
            continue;
        } else if (type == 0) {
            // Type-0 register write packet: COUNT field encodes (N - 1) body DWORDs
            u32 count = ((header >> 16) & 0x3FFF) + 1;
            if (offset + count > dwords.size()) {
                break;
            }
            offset += count;
        } else {
            SB_LOG_WARN("Pm4Decoder", "Encountered unhandled PM4 packet type {} at offset 0x{:X}", type, offset - 1);
            break;
        }
    }
    return true;
}

void Pm4Decoder::ProcessPacket(u32 header, u32 opcode, std::span<const u32> payload) {
    switch (opcode) {
        case Pm4::IT_NOP: {
            u32 tag = header & 0xFFFF;
            if (tag == 0x105C) {
                // Sony AGC PM4 Flip Packet: 0xC004105C (1 header + 5 payload DWORDs)
                if (payload.size() >= 5) {
                    u32 handle = payload[0];
                    i32 bufferIndex = static_cast<i32>(payload[1]);
                    u32 mode = payload[2];
                    i64 flipArg = static_cast<i64>((static_cast<u64>(payload[4]) << 32) | payload[3]);
                    SB_LOG_INFO("Pm4Decoder", "PM4 Embedded Flip (0xC004105C): handle={}, bufferIndex={}, mode={}, flipArg=0x{:X}",
                        handle, bufferIndex, mode, flipArg);
                    if (m_onFlip) {
                        m_onFlip(handle, bufferIndex, mode, flipArg);
                    }
                    VideoOutManager::Instance().SubmitFlip(handle, bufferIndex, mode, flipArg);
                }
            } else if (tag == 0x1018) {
                // Sony AGC Rendering Wait: 0xC0021018 (1 header + 3 payload DWORDs)
                SB_LOG_DEBUG("Pm4Decoder", "PM4 Embedded RenderingWait (0xC0021018): words={}", payload.size());
            }
            break;
        }

        case Pm4::IT_SET_CONTEXT_REG: {
            if (!payload.empty()) {
                u32 reg = (payload[0] & 0xFFFF);
                for (std::size_t i = 1; i < payload.size(); ++i) {
                    if (reg + i - 1 < m_contextRegs.size()) {
                        m_contextRegs[reg + i - 1] = payload[i];
                    }
                }
            }
            break;
        }

        case Pm4::IT_SET_SH_REG: {
            if (!payload.empty()) {
                u32 reg = (payload[0] & 0xFFFF);
                for (std::size_t i = 1; i < payload.size(); ++i) {
                    if (reg + i - 1 < m_shRegs.size()) {
                        m_shRegs[reg + i - 1] = payload[i];
                    }
                }
            }
            break;
        }

        case Pm4::IT_INDEX_BASE: {
            if (payload.size() >= 2) {
                m_indexBufferAddress = (static_cast<u64>(payload[1]) << 32) | payload[0];
            }
            break;
        }

        case Pm4::IT_INDEX_BUFFER_SIZE: {
            if (!payload.empty()) {
                m_indexBufferSize = payload[0];
            }
            break;
        }

        case Pm4::IT_INDEX_TYPE: {
            if (!payload.empty()) {
                m_indexType = payload[0] & 1; // 0 = 16-bit, 1 = 32-bit
            }
            break;
        }

        case Pm4::IT_NUM_INSTANCES: {
            if (!payload.empty()) {
                m_numInstances = payload[0] ? payload[0] : 1;
            }
            break;
        }

        case Pm4::IT_DRAW_INDEX_AUTO: {
            if (!payload.empty()) {
                DrawCommand cmd;
                cmd.isIndexed = false;
                cmd.vertexCount = payload[0];
                cmd.instanceCount = m_numInstances;
                cmd.firstIndex = 0;
                cmd.vertexOffset = 0;

                SB_LOG_DEBUG("Pm4Decoder", "DRAW_INDEX_AUTO: count={}, instances={}",
                    cmd.vertexCount, cmd.instanceCount);

                if (m_onDraw) {
                    m_onDraw(cmd);
                }
            }
            break;
        }

        case Pm4::IT_DRAW_INDEX_2: {
            if (payload.size() >= 4) {
                DrawCommand cmd;
                cmd.isIndexed = true;
                cmd.indexCount = payload[3];
                cmd.instanceCount = m_numInstances;
                cmd.indexBufferAddress = (static_cast<u64>(payload[2]) << 32) | payload[1];
                cmd.indexType = m_indexType;

                SB_LOG_DEBUG("Pm4Decoder", "DRAW_INDEX_2: indexCount={}, indexBase=0x{:X}",
                    cmd.indexCount, cmd.indexBufferAddress);

                if (m_onDraw) {
                    m_onDraw(cmd);
                }
            }
            break;
        }

        case Pm4::IT_DISPATCH_DIRECT: {
            if (payload.size() >= 3) {
                DispatchCommand cmd;
                cmd.threadsX = payload[0];
                cmd.threadsY = payload[1];
                cmd.threadsZ = payload[2];

                SB_LOG_DEBUG("Pm4Decoder", "DISPATCH_DIRECT: groups=({}, {}, {})",
                    cmd.threadsX, cmd.threadsY, cmd.threadsZ);

                if (m_onDispatch) {
                    m_onDispatch(cmd);
                }
            }
            break;
        }

        case Pm4::IT_WRITE_DATA: {
            if (payload.size() >= 4) {
                u64 dstAddr = (static_cast<u64>(payload[2]) << 32) | payload[1];
                if (dstAddr != 0) {
                    for (std::size_t i = 3; i < payload.size(); ++i) {
                        SafeWriteMemoryU32(dstAddr + (i - 3) * sizeof(u32), payload[i]);
                    }
                }
                SB_LOG_DEBUG("Pm4Decoder", "IT_WRITE_DATA: wrote {} dwords to 0x{:X}",
                    payload.size() - 3, dstAddr);
            }
            break;
        }

        case Pm4::IT_RELEASE_MEM: {
            // Write 32-bit, 64-bit fence, or timestamp per AMD/Sony AGC PM4 specification
            if (payload.size() >= 4) {
                u32 dataSelect = (payload[1] >> 29) & 0x7;
                u64 dstAddr = (static_cast<u64>(payload[3]) << 32) | (payload[2] & ~3u);
                if (dstAddr != 0) {
                    if (dataSelect == 1) {
                        // 32-bit inline write
                        u32 val32 = (payload.size() >= 5) ? payload[4] : 1;
                        SafeWriteMemoryU32(dstAddr, val32);
                        SB_LOG_DEBUG("Pm4Decoder", "IT_RELEASE_MEM (32-bit): wrote 0x{:X} to 0x{:X}", val32, dstAddr);
                    } else if (dataSelect == 3) {
                        // GPU Timestamp write
                        u64 ts = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                        SafeWriteMemoryU64(dstAddr, ts);
                        SB_LOG_DEBUG("Pm4Decoder", "IT_RELEASE_MEM (timestamp): wrote 0x{:X} to 0x{:X}", ts, dstAddr);
                    } else {
                        // 64-bit fence write (dataSelect == 2 or default)
                        u64 fenceVal = (payload.size() >= 6) ?
                            ((static_cast<u64>(payload[5]) << 32) | payload[4]) :
                            ((payload.size() >= 5) ? static_cast<u64>(payload[4]) : 1ULL);
                        SafeWriteMemoryU64(dstAddr, fenceVal);
                        SB_LOG_DEBUG("Pm4Decoder", "IT_RELEASE_MEM (64-bit fence): wrote 0x{:X} to 0x{:X}", fenceVal, dstAddr);
                    }
                }
            }
            break;
        }

        case Pm4::IT_WAIT_REG_MEM: {
            if (payload.size() >= 4) {
                u64 pollAddr = (static_cast<u64>(payload[2]) << 32) | payload[1];
                u32 refVal = (payload.size() >= 4) ? payload[3] : 0;
                SB_LOG_DEBUG("Pm4Decoder", "IT_WAIT_REG_MEM: polling 0x{:X} for ref 0x{:X}", pollAddr, refVal);
            }
            break;
        }

        case Pm4::IT_DRAW_INDIRECT: {
            if (payload.size() >= 2) {
                DrawCommand cmd;
                cmd.isIndexed = false;
                cmd.isIndirect = true;
                cmd.indirectBufferAddress = (static_cast<u64>(payload[1]) << 32) | payload[0];
                cmd.instanceCount = m_numInstances;
                SB_LOG_DEBUG("Pm4Decoder", "IT_DRAW_INDIRECT: indirectBase=0x{:X}", cmd.indirectBufferAddress);
                if (m_onDraw) m_onDraw(cmd);
            }
            break;
        }

        case Pm4::IT_DRAW_INDEX_INDIRECT: {
            if (payload.size() >= 2) {
                DrawCommand cmd;
                cmd.isIndexed = true;
                cmd.isIndirect = true;
                cmd.indirectBufferAddress = (static_cast<u64>(payload[1]) << 32) | payload[0];
                cmd.indexBufferAddress = m_indexBufferAddress;
                cmd.indexType = m_indexType;
                SB_LOG_DEBUG("Pm4Decoder", "IT_DRAW_INDEX_INDIRECT: indirectBase=0x{:X}", cmd.indirectBufferAddress);
                if (m_onDraw) m_onDraw(cmd);
            }
            break;
        }

        case Pm4::IT_EVENT_WRITE: {
            SB_LOG_DEBUG("Pm4Decoder", "IT_EVENT_WRITE: payload size={}", payload.size());
            break;
        }

        case Pm4::IT_EVENT_WRITE_EOP: {
            if (payload.size() >= 3) {
                u64 dstAddr = (static_cast<u64>(payload[2]) << 32) | (payload[1] & ~3u);
                u64 fenceVal = (payload.size() >= 5) ? ((static_cast<u64>(payload[4]) << 32) | payload[3]) : 1ULL;
                if (dstAddr != 0) {
                    SafeWriteMemoryU64(dstAddr, fenceVal);
                    SB_LOG_DEBUG("Pm4Decoder", "IT_EVENT_WRITE_EOP: wrote fence 0x{:X} to memory 0x{:X}", fenceVal, dstAddr);
                }
            }
            break;
        }

        case Pm4::IT_DISPATCH_INDIRECT: {
            if (payload.size() >= 2) {
                DispatchCommand cmd;
                cmd.isIndirect = true;
                cmd.indirectBufferAddress = (static_cast<u64>(payload[1]) << 32) | payload[0];
                SB_LOG_DEBUG("Pm4Decoder", "IT_DISPATCH_INDIRECT: indirectBase=0x{:X}", cmd.indirectBufferAddress);
                if (m_onDispatch) m_onDispatch(cmd);
            }
            break;
        }

        default:
            SB_LOG_TRACE("Pm4Decoder", "Processed PM4 packet: 0x{:02X} ({}) with {} dwords",
                opcode, Pm4::GetOpcodeName(opcode), payload.size());
            break;
    }
}

} // namespace Sailor
