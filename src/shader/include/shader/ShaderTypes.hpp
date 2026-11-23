#pragma once

#include "common/Types.hpp"
#include "RdnaOpcodes.hpp"
#include <string>
#include <vector>
#include <span>
#include <array>
#include <string_view>

namespace Sailor::Shader {

enum class ShaderStage : u32 {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
    Geometry = 3,
    TessControl = 4,
    TessEvaluation = 5,
    Mesh = 6
};

inline const char* ShaderStageToString(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return "Vertex";
        case ShaderStage::Fragment: return "Fragment";
        case ShaderStage::Compute: return "Compute";
        case ShaderStage::Geometry: return "Geometry";
        case ShaderStage::TessControl: return "TessControl";
        case ShaderStage::TessEvaluation: return "TessEvaluation";
        case ShaderStage::Mesh: return "Mesh";
        default: return "Unknown";
    }
}

enum class RdnaFamily : u8 {
    Unknown = 0,
    SOP1,
    SOP2,
    SOPK,
    SOPC,
    SOPP,
    VOP1,
    VOP2,
    VOP3,
    VOP3P,
    VOPC,
    VINTRP,
    SMEM,
    MUBUF,
    MTBUF,
    MIMG,
    FLAT,
    GLOBAL,
    SCRATCH,
    DS,
    EXP
};

inline const char* RdnaFamilyToString(RdnaFamily family) {
    switch (family) {
        case RdnaFamily::SOP1: return "SOP1";
        case RdnaFamily::SOP2: return "SOP2";
        case RdnaFamily::SOPK: return "SOPK";
        case RdnaFamily::SOPC: return "SOPC";
        case RdnaFamily::SOPP: return "SOPP";
        case RdnaFamily::VOP1: return "VOP1";
        case RdnaFamily::VOP2: return "VOP2";
        case RdnaFamily::VOP3: return "VOP3";
        case RdnaFamily::VOP3P: return "VOP3P";
        case RdnaFamily::VOPC: return "VOPC";
        case RdnaFamily::VINTRP: return "VINTRP";
        case RdnaFamily::SMEM: return "SMEM";
        case RdnaFamily::MUBUF: return "MUBUF";
        case RdnaFamily::MTBUF: return "MTBUF";
        case RdnaFamily::MIMG: return "MIMG";
        case RdnaFamily::FLAT: return "FLAT";
        case RdnaFamily::GLOBAL: return "GLOBAL";
        case RdnaFamily::SCRATCH: return "SCRATCH";
        case RdnaFamily::DS: return "DS";
        case RdnaFamily::EXP: return "EXP";
        default: return "Unknown";
    }
}

enum class OperandKind : u8 {
    None = 0,
    ScalarRegister,
    VectorRegister,
    VccLo,
    VccHi,
    ExecLo,
    ExecHi,
    Scc,
    M0,
    Null,
    LiteralConstant,
    InlineIntConstant,
    InlineFloatConstant,
    SrcSharedBase,
    SrcSharedLimit,
    SrcPrivateBase,
    SrcPrivateLimit
};

struct RdnaOperand {
    OperandKind kind = OperandKind::None;
    u32 reg = 0;
    u32 value = 0;
    i32 signedVal = 0;

    [[nodiscard]] std::string ToString() const;
};

struct RdnaInstruction {
    u32 pc = 0;
    RdnaFamily family = RdnaFamily::Unknown;
    u16 opcode = 0;
    RdnaOp op = RdnaOp::Invalid;
    const char* opcodeName = "unknown";
    u8 wordCount = 1;
    u8 sourceCount = 0;
    RdnaOperand dst;
    std::array<RdnaOperand, 4> src{};
    std::array<u32, 5> rawWords{};
    u32 branchTarget = 0;
    bool isBranch = false;
    bool isEndProgram = false;

    [[nodiscard]] std::string Disassemble() const;
};

struct ShaderBinary {
    ShaderStage stage = ShaderStage::Vertex;
    std::vector<u32> code;
    u64 guestAddress = 0;
    u32 workgroupSizeX = 1;
    u32 workgroupSizeY = 1;
    u32 workgroupSizeZ = 1;
};

} // namespace Sailor::Shader
