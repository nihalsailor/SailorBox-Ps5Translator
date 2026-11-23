#include "shader/RdnaDecoder.hpp"
#include <bit>
#include <stdexcept>
#include <format>

namespace Sailor::Shader {

RdnaFamily RdnaDecoder::GetInstructionFamily(u32 firstWord) {
    // Vector ALU 32-bit (Bit 31 == 0)
    if ((firstWord & 0x80000000u) == 0u) {
        const u32 sub = (firstWord >> 25u) & 0x3Fu;
        switch (sub) {
            case 0x3Eu: return RdnaFamily::VOPC;
            case 0x3Fu: return RdnaFamily::VOP1;
            default:   return RdnaFamily::VOP2;
        }
    }

    // Scalar ALU 32-bit (Bit 31:30 == 0b10)
    if ((firstWord & 0xC0000000u) == 0x80000000u) {
        const u32 sub = (firstWord >> 23u) & 0x7Fu;
        switch (sub) {
            case 0x7Du: return RdnaFamily::SOP1;
            case 0x7Eu: return RdnaFamily::SOPC;
            case 0x7Fu: return RdnaFamily::SOPP;
            default:
                return (sub >= 0x60u) ? RdnaFamily::SOPK : RdnaFamily::SOP2;
        }
    }

    // 64-bit Encodings (Bit 31:26)
    switch ((firstWord >> 26u) & 0x3Fu) {
        case 0x32u: return RdnaFamily::VINTRP;
        case 0x33u: return RdnaFamily::VOP3P;
        case 0x35u: return RdnaFamily::VOP3;
        case 0x36u: return RdnaFamily::DS;
        case 0x37u: return RdnaFamily::FLAT;
        case 0x38u: return RdnaFamily::MUBUF;
        case 0x3Au: return RdnaFamily::MTBUF;
        case 0x3Cu: return RdnaFamily::MIMG;
        case 0x3Du: return RdnaFamily::SMEM;
        case 0x3Eu: return RdnaFamily::EXP;
        default:   return RdnaFamily::Unknown;
    }
}

RdnaOperand RdnaDecoder::DecodeScalarSource(u32 code, u32 pc) {
    (void)pc;
    RdnaOperand op;
    if (code <= 105u) {
        op.kind = OperandKind::ScalarRegister;
        op.reg = code;
        return op;
    }
    if (code >= 128u && code <= 192u) {
        op.kind = OperandKind::InlineIntConstant;
        op.signedVal = static_cast<i32>(code - 128u);
        op.value = static_cast<u32>(op.signedVal);
        return op;
    }
    if (code >= 193u && code <= 208u) {
        op.kind = OperandKind::InlineIntConstant;
        op.signedVal = 192 - static_cast<i32>(code);
        op.value = static_cast<u32>(op.signedVal);
        return op;
    }
    if (code >= 240u && code <= 247u) {
        static constexpr float floats[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
        op.kind = OperandKind::InlineFloatConstant;
        op.value = std::bit_cast<u32>(floats[code - 240u]);
        return op;
    }
    if (code >= 256u && code <= 511u) {
        op.kind = OperandKind::VectorRegister;
        op.reg = code - 256u;
        return op;
    }
    switch (code) {
        case 106u: op.kind = OperandKind::VccLo; return op;
        case 107u: op.kind = OperandKind::VccHi; return op;
        case 124u: op.kind = OperandKind::M0; return op;
        case 125u: op.kind = OperandKind::Null; return op;
        case 126u: op.kind = OperandKind::ExecLo; return op;
        case 127u: op.kind = OperandKind::ExecHi; return op;
        case 235u: op.kind = OperandKind::SrcSharedBase; return op;
        case 236u: op.kind = OperandKind::SrcSharedLimit; return op;
        case 237u: op.kind = OperandKind::SrcPrivateBase; return op;
        case 238u: op.kind = OperandKind::SrcPrivateLimit; return op;
        case 253u: op.kind = OperandKind::Scc; return op;
        case 255u: op.kind = OperandKind::LiteralConstant; return op;
        default: break;
    }
    op.kind = OperandKind::InlineIntConstant;
    op.signedVal = static_cast<i32>(code);
    op.value = code;
    return op;
}

RdnaOperand RdnaDecoder::DecodeScalarDestination(u32 code, u32 pc) {
    (void)pc;
    RdnaOperand op;
    if (code <= 105u) {
        op.kind = OperandKind::ScalarRegister;
        op.reg = code;
        return op;
    }
    switch (code) {
        case 106u: op.kind = OperandKind::VccLo; return op;
        case 107u: op.kind = OperandKind::VccHi; return op;
        case 124u: op.kind = OperandKind::M0; return op;
        case 125u: op.kind = OperandKind::Null; return op;
        case 126u: op.kind = OperandKind::ExecLo; return op;
        case 127u: op.kind = OperandKind::ExecHi; return op;
        default:   op.kind = OperandKind::ScalarRegister; op.reg = code; return op;
    }
}

RdnaOperand RdnaDecoder::DecodeVectorSource(u32 code, u32 pc) {
    if (code <= 255u) {
        RdnaOperand op;
        op.kind = OperandKind::VectorRegister;
        op.reg = code;
        return op;
    }
    return DecodeScalarSource(code - 256u, pc);
}

RdnaOperand RdnaDecoder::DecodeVectorDestination(u32 code) {
    RdnaOperand op;
    op.kind = OperandKind::VectorRegister;
    op.reg = code & 0xFFu;
    return op;
}

static void ReadLiterals(std::span<const u32> code, u32 wordIndex, RdnaInstruction& inst) {
    bool hasLiteral = false;
    for (u8 i = 0; i < inst.sourceCount; ++i) {
        if (inst.src[i].kind == OperandKind::LiteralConstant) {
            hasLiteral = true;
            break;
        }
    }
    if (hasLiteral && wordIndex + inst.wordCount < code.size()) {
        const u32 lit = code[wordIndex + inst.wordCount];
        inst.rawWords[inst.wordCount] = lit;
        inst.wordCount++;
        for (u8 i = 0; i < inst.sourceCount; ++i) {
            if (inst.src[i].kind == OperandKind::LiteralConstant) {
                inst.src[i].value = lit;
                inst.src[i].signedVal = static_cast<i32>(lit);
            }
        }
    }
}

RdnaInstruction RdnaDecoder::DecodeSop1(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 8u) & 0xFFu;
    const u32 src0Code = word & 0xFFu;
    const u32 dstCode = (word >> 16u) & 0x7Fu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::SOP1;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;
    inst.sourceCount = 1;

    switch (opId) {
        case 0x03u: inst.op = RdnaOp::S_MOV_B32; break;
        case 0x04u: inst.op = RdnaOp::S_MOV_B64; break;
        case 0x07u: inst.op = RdnaOp::S_NOT_B32; break;
        case 0x08u: inst.op = RdnaOp::S_NOT_B64; break;
        case 0x0Bu: inst.op = RdnaOp::S_BREV_B32; break;
        case 0x0Fu: inst.op = RdnaOp::S_BCNT1_I32_B32; break;
        case 0x1Fu: inst.op = RdnaOp::S_GETPC_B64; inst.sourceCount = 0; break;
        case 0x20u: inst.op = RdnaOp::S_SETPC_B64; break;
        case 0x21u: inst.op = RdnaOp::S_SWAPPC_B64; break;
        case 0x24u: inst.op = RdnaOp::S_AND_SAVEEXEC_B64; break;
        case 0x25u: inst.op = RdnaOp::S_OR_SAVEEXEC_B64; break;
        case 0x26u: inst.op = RdnaOp::S_XOR_SAVEEXEC_B64; break;
        case 0x27u: inst.op = RdnaOp::S_ANDN2_SAVEEXEC_B64; break;
        case 0x34u: inst.op = RdnaOp::S_ABS_I32; break;
        case 0x3Cu: inst.op = RdnaOp::S_AND_SAVEEXEC_B32; break;
        case 0x3Du: inst.op = RdnaOp::S_OR_SAVEEXEC_B32; break;
        case 0x3Eu: inst.op = RdnaOp::S_XOR_SAVEEXEC_B32; break;
        default:    inst.op = RdnaOp::S_MOV_B32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeScalarDestination(dstCode, pc);
    if (inst.sourceCount > 0) {
        inst.src[0] = DecodeScalarSource(src0Code, pc);
        ReadLiterals(code, wordIndex, inst);
    }
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeSop2(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 23u) & 0x7Fu;
    const u32 src0Code = word & 0xFFu;
    const u32 src1Code = (word >> 8u) & 0xFFu;
    const u32 dstCode = (word >> 16u) & 0x7Fu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::SOP2;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;
    inst.sourceCount = 2;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::S_ADD_U32; break;
        case 0x01u: inst.op = RdnaOp::S_SUB_U32; break;
        case 0x02u: inst.op = RdnaOp::S_ADD_I32; break;
        case 0x03u: inst.op = RdnaOp::S_SUB_I32; break;
        case 0x04u: inst.op = RdnaOp::S_ADDC_U32; break;
        case 0x05u: inst.op = RdnaOp::S_SUBB_U32; break;
        case 0x06u: inst.op = RdnaOp::S_MIN_I32; break;
        case 0x07u: inst.op = RdnaOp::S_MIN_U32; break;
        case 0x08u: inst.op = RdnaOp::S_MAX_I32; break;
        case 0x09u: inst.op = RdnaOp::S_MAX_U32; break;
        case 0x0Au: inst.op = RdnaOp::S_CSELECT_B32; break;
        case 0x0Bu: inst.op = RdnaOp::S_CSELECT_B64; break;
        case 0x0Eu: inst.op = RdnaOp::S_AND_B32; break;
        case 0x0Fu: inst.op = RdnaOp::S_AND_B64; break;
        case 0x10u: inst.op = RdnaOp::S_OR_B32; break;
        case 0x11u: inst.op = RdnaOp::S_OR_B64; break;
        case 0x12u: inst.op = RdnaOp::S_XOR_B32; break;
        case 0x13u: inst.op = RdnaOp::S_XOR_B64; break;
        case 0x14u: inst.op = RdnaOp::S_ANDN2_B32; break;
        case 0x15u: inst.op = RdnaOp::S_ANDN2_B64; break;
        case 0x16u: inst.op = RdnaOp::S_ORN2_B32; break;
        case 0x17u: inst.op = RdnaOp::S_ORN2_B64; break;
        case 0x18u: inst.op = RdnaOp::S_NAND_B32; break;
        case 0x19u: inst.op = RdnaOp::S_NOR_B32; break;
        case 0x1Au: inst.op = RdnaOp::S_XNOR_B32; break;
        case 0x1Bu: inst.op = RdnaOp::S_LSHL_B32; break;
        case 0x1Cu: inst.op = RdnaOp::S_LSHL_B64; break;
        case 0x1Du: inst.op = RdnaOp::S_LSHR_B32; break;
        case 0x1Eu: inst.op = RdnaOp::S_ASHR_I32; break;
        case 0x21u: inst.op = RdnaOp::S_MUL_I32; break;
        case 0x22u: inst.op = RdnaOp::S_BFE_U32; break;
        case 0x23u: inst.op = RdnaOp::S_BFE_I32; break;
        default:    inst.op = RdnaOp::S_ADD_U32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeScalarDestination(dstCode, pc);
    inst.src[0] = DecodeScalarSource(src0Code, pc);
    inst.src[1] = DecodeScalarSource(src1Code, pc);
    ReadLiterals(code, wordIndex, inst);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeSopk(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 23u) & 0x1Fu;
    const i16 simm16 = static_cast<i16>(word & 0xFFFFu);
    const u32 dstCode = (word >> 16u) & 0x7Fu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::SOPK;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;
    inst.sourceCount = 1;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::S_MOVK_I32; break;
        case 0x0Fu: inst.op = RdnaOp::S_ADDK_I32; break;
        case 0x10u: inst.op = RdnaOp::S_MULK_I32; break;
        default:    inst.op = RdnaOp::S_MOVK_I32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeScalarDestination(dstCode, pc);
    inst.src[0].kind = OperandKind::InlineIntConstant;
    inst.src[0].signedVal = simm16;
    inst.src[0].value = static_cast<u32>(simm16);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeSopc(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 16u) & 0x7Fu;
    const u32 src0Code = word & 0xFFu;
    const u32 src1Code = (word >> 8u) & 0xFFu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::SOPC;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;
    inst.sourceCount = 2;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::S_CMP_EQ_I32; break;
        case 0x01u: inst.op = RdnaOp::S_CMP_LG_I32; break;
        case 0x02u: inst.op = RdnaOp::S_CMP_GT_I32; break;
        case 0x03u: inst.op = RdnaOp::S_CMP_GE_I32; break;
        case 0x04u: inst.op = RdnaOp::S_CMP_LT_I32; break;
        case 0x05u: inst.op = RdnaOp::S_CMP_LE_I32; break;
        case 0x06u: inst.op = RdnaOp::S_CMP_EQ_U32; break;
        case 0x07u: inst.op = RdnaOp::S_CMP_LG_U32; break;
        case 0x08u: inst.op = RdnaOp::S_CMP_GT_U32; break;
        case 0x09u: inst.op = RdnaOp::S_CMP_GE_U32; break;
        case 0x0Au: inst.op = RdnaOp::S_CMP_LT_U32; break;
        case 0x0Bu: inst.op = RdnaOp::S_CMP_LE_U32; break;
        default:    inst.op = RdnaOp::S_CMP_EQ_I32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst.kind = OperandKind::Scc;
    inst.src[0] = DecodeScalarSource(src0Code, pc);
    inst.src[1] = DecodeScalarSource(src1Code, pc);
    ReadLiterals(code, wordIndex, inst);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeSopp(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 16u) & 0x7Fu;
    const i16 simm16 = static_cast<i16>(word & 0xFFFFu);

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::SOPP;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::S_NOP; break;
        case 0x01u:
            inst.op = RdnaOp::S_BRANCH;
            inst.isBranch = true;
            inst.branchTarget = pc + 4 + (simm16 * 4);
            break;
        case 0x03u: inst.op = RdnaOp::S_WAKEUP; break;
        case 0x04u:
            inst.op = RdnaOp::S_CBRANCH_SCC0;
            inst.isBranch = true;
            inst.branchTarget = pc + 4 + (simm16 * 4);
            break;
        case 0x05u:
            inst.op = RdnaOp::S_CBRANCH_SCC1;
            inst.isBranch = true;
            inst.branchTarget = pc + 4 + (simm16 * 4);
            break;
        case 0x06u:
            inst.op = RdnaOp::S_CBRANCH_VCCZ;
            inst.isBranch = true;
            inst.branchTarget = pc + 4 + (simm16 * 4);
            break;
        case 0x07u:
            inst.op = RdnaOp::S_CBRANCH_VCCNZ;
            inst.isBranch = true;
            inst.branchTarget = pc + 4 + (simm16 * 4);
            break;
        case 0x08u:
            inst.op = RdnaOp::S_CBRANCH_EXECZ;
            inst.isBranch = true;
            inst.branchTarget = pc + 4 + (simm16 * 4);
            break;
        case 0x09u:
            inst.op = RdnaOp::S_CBRANCH_EXECNZ;
            inst.isBranch = true;
            inst.branchTarget = pc + 4 + (simm16 * 4);
            break;
        case 0x0Au: inst.op = RdnaOp::S_BARRIER; break;
        case 0x0Bu:
        case 0x1Eu:
            inst.op = RdnaOp::S_ENDPGM;
            inst.isEndProgram = true;
            break;
        case 0x0Cu: inst.op = RdnaOp::S_WAITCNT; break;
        case 0x0Du: inst.op = RdnaOp::S_SETHALT; break;
        case 0x0Eu: inst.op = RdnaOp::S_SLEEP; break;
        case 0x0Fu: inst.op = RdnaOp::S_SETPRIO; break;
        case 0x10u: inst.op = RdnaOp::S_SENDMSG; break;
        case 0x1Fu: inst.op = RdnaOp::S_CODE_END; inst.isEndProgram = true; break;
        default:    inst.op = RdnaOp::S_NOP; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.src[0].kind = OperandKind::InlineIntConstant;
    inst.src[0].signedVal = simm16;
    inst.src[0].value = static_cast<u32>(simm16);
    inst.sourceCount = 1;
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeVop1(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 9u) & 0xFFu;
    const u32 src0Code = word & 0x1FFu;
    const u32 dstCode = (word >> 17u) & 0xFFu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::VOP1;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;
    inst.sourceCount = 1;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::V_NOP; break;
        case 0x01u: inst.op = RdnaOp::V_MOV_B32; break;
        case 0x02u: inst.op = RdnaOp::V_READFIRSTLANE_B32; break;
        case 0x05u: inst.op = RdnaOp::V_CVT_F32_I32; break;
        case 0x06u: inst.op = RdnaOp::V_CVT_F32_U32; break;
        case 0x07u: inst.op = RdnaOp::V_CVT_U32_F32; break;
        case 0x08u: inst.op = RdnaOp::V_CVT_I32_F32; break;
        case 0x09u: inst.op = RdnaOp::V_CVT_F16_F32; break;
        case 0x0Au: inst.op = RdnaOp::V_CVT_F32_F16; break;
        case 0x10u: inst.op = RdnaOp::V_FRACT_F32; break;
        case 0x11u: inst.op = RdnaOp::V_TRUNC_F32; break;
        case 0x12u: inst.op = RdnaOp::V_CEIL_F32; break;
        case 0x13u: inst.op = RdnaOp::V_RNDNE_F32; break;
        case 0x14u: inst.op = RdnaOp::V_FLOOR_F32; break;
        case 0x15u: inst.op = RdnaOp::V_EXP_F32; break;
        case 0x16u: inst.op = RdnaOp::V_LOG_F32; break;
        case 0x17u: inst.op = RdnaOp::V_RCP_F32; break;
        case 0x19u: inst.op = RdnaOp::V_RSQ_F32; break;
        case 0x1Bu: inst.op = RdnaOp::V_SQRT_F32; break;
        case 0x1Eu: inst.op = RdnaOp::V_SIN_F32; break;
        case 0x1Fu: inst.op = RdnaOp::V_COS_F32; break;
        case 0x20u: inst.op = RdnaOp::V_NOT_B32; break;
        case 0x21u: inst.op = RdnaOp::V_BFREV_B32; break;
        default:    inst.op = RdnaOp::V_MOV_B32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = (inst.op == RdnaOp::V_READFIRSTLANE_B32)
        ? DecodeScalarDestination(dstCode, pc)
        : DecodeVectorDestination(dstCode);
    inst.src[0] = DecodeVectorSource(src0Code, pc);
    ReadLiterals(code, wordIndex, inst);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeVop2(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 25u) & 0x3Fu;
    const u32 src0Code = word & 0x1FFu;
    const u32 src1Code = (word >> 9u) & 0xFFu;
    const u32 dstCode = (word >> 17u) & 0xFFu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::VOP2;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;
    inst.sourceCount = 2;

    switch (opId) {
        case 0x01u: inst.op = RdnaOp::V_CNDMASK_B32; break;
        case 0x03u: inst.op = RdnaOp::V_ADD_F32; break;
        case 0x04u: inst.op = RdnaOp::V_SUB_F32; break;
        case 0x05u: inst.op = RdnaOp::V_SUBREV_F32; break;
        case 0x08u: inst.op = RdnaOp::V_MUL_F32; break;
        case 0x0Fu: inst.op = RdnaOp::V_MIN_F32; break;
        case 0x10u: inst.op = RdnaOp::V_MAX_F32; break;
        case 0x11u: inst.op = RdnaOp::V_MIN_I32; break;
        case 0x12u: inst.op = RdnaOp::V_MAX_I32; break;
        case 0x13u: inst.op = RdnaOp::V_MIN_U32; break;
        case 0x14u: inst.op = RdnaOp::V_MAX_U32; break;
        case 0x16u: inst.op = RdnaOp::V_LSHRREV_B32; break;
        case 0x18u: inst.op = RdnaOp::V_ASHRREV_I32; break;
        case 0x1Au: inst.op = RdnaOp::V_LSHLREV_B32; break;
        case 0x1Bu: inst.op = RdnaOp::V_AND_B32; break;
        case 0x1Cu: inst.op = RdnaOp::V_OR_B32; break;
        case 0x1Du: inst.op = RdnaOp::V_XOR_B32; break;
        case 0x1Eu: inst.op = RdnaOp::V_XNOR_B32; break;
        case 0x25u: inst.op = RdnaOp::V_ADD_NC_U32; break;
        case 0x26u: inst.op = RdnaOp::V_SUB_NC_U32; break;
        case 0x27u: inst.op = RdnaOp::V_SUBREV_NC_U32; break;
        case 0x28u: inst.op = RdnaOp::V_ADD_CO_CI_U32; break;
        case 0x29u: inst.op = RdnaOp::V_SUB_CO_CI_U32; break;
        case 0x2Au: inst.op = RdnaOp::V_SUBREV_CO_CI_U32; break;
        case 0x2Bu: inst.op = RdnaOp::V_FMAC_F32; break;
        default:    inst.op = RdnaOp::V_ADD_F32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeVectorDestination(dstCode);
    inst.src[0] = DecodeVectorSource(src0Code, pc);
    inst.src[1] = DecodeVectorSource(src1Code, pc);
    ReadLiterals(code, wordIndex, inst);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeVop3(std::span<const u32> code, u32 wordIndex) const {
    const u32 w0 = code[wordIndex];
    const u32 w1 = (wordIndex + 1 < code.size()) ? code[wordIndex + 1] : 0u;
    const u32 pc = wordIndex * 4;
    const u32 opId = (w0 >> 16u) & 0x3FFu;
    const u32 dstCode = w0 & 0xFFu;
    const u32 src0Code = w1 & 0x1FFu;
    const u32 src1Code = (w1 >> 9u) & 0x1FFu;
    const u32 src2Code = (w1 >> 18u) & 0x1FFu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::VOP3;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 2;
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;
    inst.sourceCount = 3;

    switch (opId) {
        case 0x1C0u: inst.op = RdnaOp::V_MAD_F32; break;
        case 0x1C3u: inst.op = RdnaOp::V_FMA_F32; break;
        case 0x1D0u: inst.op = RdnaOp::V_MIN3_F32; break;
        case 0x1D1u: inst.op = RdnaOp::V_MAX3_F32; break;
        case 0x1D2u: inst.op = RdnaOp::V_MED3_F32; break;
        default:     inst.op = RdnaOp::V_FMA_F32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeVectorDestination(dstCode);
    inst.src[0] = DecodeVectorSource(src0Code, pc);
    inst.src[1] = DecodeVectorSource(src1Code, pc);
    inst.src[2] = DecodeVectorSource(src2Code, pc);
    ReadLiterals(code, wordIndex, inst);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeVopc(std::span<const u32> code, u32 wordIndex) const {
    const u32 word = code[wordIndex];
    const u32 pc = wordIndex * 4;
    const u32 opId = (word >> 17u) & 0xFFu;
    const u32 src0Code = word & 0x1FFu;
    const u32 src1Code = (word >> 9u) & 0xFFu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::VOPC;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 1;
    inst.rawWords[0] = word;
    inst.sourceCount = 2;

    switch (opId) {
        case 0x01u: inst.op = RdnaOp::V_CMP_LT_F32; break;
        case 0x02u: inst.op = RdnaOp::V_CMP_EQ_F32; break;
        case 0x03u: inst.op = RdnaOp::V_CMP_LE_F32; break;
        case 0x04u: inst.op = RdnaOp::V_CMP_GT_F32; break;
        case 0x05u: inst.op = RdnaOp::V_CMP_LG_F32; break;
        case 0x06u: inst.op = RdnaOp::V_CMP_GE_F32; break;
        case 0x0Du: inst.op = RdnaOp::V_CMP_NEQ_F32; break;

        case 0x81u: inst.op = RdnaOp::V_CMP_LT_I32; break;
        case 0x82u: inst.op = RdnaOp::V_CMP_EQ_I32; break;
        case 0x83u: inst.op = RdnaOp::V_CMP_LE_I32; break;
        case 0x84u: inst.op = RdnaOp::V_CMP_GT_I32; break;
        case 0x85u: inst.op = RdnaOp::V_CMP_NE_I32; break;
        case 0x86u: inst.op = RdnaOp::V_CMP_GE_I32; break;

        case 0xC1u: inst.op = RdnaOp::V_CMP_LT_U32; break;
        case 0xC2u: inst.op = RdnaOp::V_CMP_EQ_U32; break;
        case 0xC3u: inst.op = RdnaOp::V_CMP_LE_U32; break;
        case 0xC4u: inst.op = RdnaOp::V_CMP_GT_U32; break;
        case 0xC5u: inst.op = RdnaOp::V_CMP_NE_U32; break;
        case 0xC6u: inst.op = RdnaOp::V_CMP_GE_U32; break;

        default:    inst.op = RdnaOp::V_CMP_EQ_F32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst.kind = OperandKind::VccLo;
    inst.src[0] = DecodeVectorSource(src0Code, pc);
    inst.src[1] = DecodeVectorSource(src1Code, pc);
    ReadLiterals(code, wordIndex, inst);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeSmem(std::span<const u32> code, u32 wordIndex) const {
    const u32 w0 = code[wordIndex];
    const u32 w1 = (wordIndex + 1 < code.size()) ? code[wordIndex + 1] : 0u;
    const u32 pc = wordIndex * 4;
    const u32 opId = (w0 >> 18u) & 0x7Fu;
    const u32 dstCode = (w0 >> 6u) & 0x7Fu;
    const u32 sbaseCode = (w0 >> 1u) & 0x1Fu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::SMEM;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 2;
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;
    inst.sourceCount = 2;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::S_LOAD_DWORD; break;
        case 0x01u: inst.op = RdnaOp::S_LOAD_DWORDX2; break;
        case 0x02u: inst.op = RdnaOp::S_LOAD_DWORDX4; break;
        case 0x03u: inst.op = RdnaOp::S_LOAD_DWORDX8; break;
        case 0x04u: inst.op = RdnaOp::S_LOAD_DWORDX16; break;
        case 0x08u: inst.op = RdnaOp::S_BUFFER_LOAD_DWORD; break;
        case 0x09u: inst.op = RdnaOp::S_BUFFER_LOAD_DWORDX2; break;
        case 0x0Au: inst.op = RdnaOp::S_BUFFER_LOAD_DWORDX4; break;
        case 0x10u: inst.op = RdnaOp::S_STORE_DWORD; break;
        case 0x11u: inst.op = RdnaOp::S_STORE_DWORDX2; break;
        case 0x12u: inst.op = RdnaOp::S_STORE_DWORDX4; break;
        case 0x18u: inst.op = RdnaOp::S_BUFFER_STORE_DWORD; break;
        // PS5 gfx1013 SMEM atomics
        case 0x40u: inst.op = RdnaOp::S_ATOMIC_ADD; break;
        case 0x41u: inst.op = RdnaOp::S_ATOMIC_SUB; break;
        case 0x50u: inst.op = RdnaOp::S_BUFFER_ATOMIC_ADD; break;
        case 0x51u: inst.op = RdnaOp::S_BUFFER_ATOMIC_SUB; break;
        default:    inst.op = RdnaOp::S_LOAD_DWORD; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeScalarDestination(dstCode, pc);
    inst.src[0] = DecodeScalarSource(sbaseCode * 2, pc); // sbase pairs
    i32 signedSmemOffset = static_cast<i32>(w1 & 0x1FFFFFu);
    if (signedSmemOffset & 0x100000) {
        signedSmemOffset |= ~0x1FFFFF; // Sign-extend 21-bit
    }
    inst.src[1].kind = OperandKind::InlineIntConstant;
    inst.src[1].value = static_cast<u32>(signedSmemOffset);
    inst.src[1].signedVal = signedSmemOffset;
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeMubuf(std::span<const u32> code, u32 wordIndex) const {
    const u32 w0 = code[wordIndex];
    const u32 w1 = (wordIndex + 1 < code.size()) ? code[wordIndex + 1] : 0u;
    const u32 pc = wordIndex * 4;
    const u32 opW0 = (w0 >> 18u) & 0x7Fu;
    const u32 opW1 = (w1 >> 18u) & 0x7Fu;
    const u32 opId = (opW0 != 0u) ? opW0 : opW1;
    // In RDNA 2 64-bit MUBUF: vaddr is in w1[7:0], vdata is in w1[15:8]
    const u32 vaddr = w1 & 0xFFu;
    const u32 vdata = (w1 >> 8u) & 0xFFu;
    const u32 srsrc = (w1 >> 16u) & 0x1Fu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::MUBUF;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 2;
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;
    inst.sourceCount = 3;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::BUFFER_LOAD_FORMAT_X; break;
        case 0x01u: inst.op = RdnaOp::BUFFER_LOAD_FORMAT_XY; break;
        case 0x02u: inst.op = RdnaOp::BUFFER_LOAD_FORMAT_XYZ; break;
        case 0x03u: inst.op = RdnaOp::BUFFER_LOAD_FORMAT_XYZW; break;
        case 0x04u: inst.op = RdnaOp::BUFFER_STORE_FORMAT_XYZW; break;
        case 0x08u: inst.op = RdnaOp::BUFFER_LOAD_DWORD; break;
        case 0x0Cu: inst.op = RdnaOp::BUFFER_LOAD_DWORDX4; break;
        case 0x18u: inst.op = RdnaOp::BUFFER_STORE_DWORD; break;
        case 0x1Cu: inst.op = RdnaOp::BUFFER_STORE_DWORDX4; break;
        default:    inst.op = RdnaOp::BUFFER_LOAD_FORMAT_XYZW; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeVectorDestination(vdata);
    inst.src[0] = DecodeVectorSource(vaddr, pc);
    inst.src[1] = DecodeScalarSource(srsrc * 4, pc); // resource descriptor
    i32 signedMubufOffset = static_cast<i32>(w0 & 0xFFFu);
    if (signedMubufOffset & 0x800) {
        signedMubufOffset |= ~0xFFF; // Sign-extend 12-bit
    }
    inst.src[2].kind = OperandKind::InlineIntConstant;
    inst.src[2].value = static_cast<u32>(signedMubufOffset);
    inst.src[2].signedVal = signedMubufOffset;
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeFlat(std::span<const u32> code, u32 wordIndex) const {
    const u32 w0 = code[wordIndex];
    const u32 w1 = (wordIndex + 1 < code.size()) ? code[wordIndex + 1] : 0u;
    const u32 pc = wordIndex * 4;
    const u32 opW0 = (w0 >> 18u) & 0x7Fu;
    const u32 opW1 = (w1 >> 18u) & 0x7Fu;
    const u32 opId = (opW0 != 0u) ? opW0 : opW1;
    // In RDNA 2 64-bit FLAT: vaddr is in w1[7:0], vdata is in w1[15:8]
    const u32 vaddr = w1 & 0xFFu;
    const u32 vdata = (w1 >> 8u) & 0xFFu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::FLAT;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 2;
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;
    inst.sourceCount = 2;

    switch (opId) {
        case 0x14u: inst.op = RdnaOp::GLOBAL_LOAD_DWORD; break;
        case 0x17u: inst.op = RdnaOp::GLOBAL_LOAD_DWORDX4; break;
        case 0x1Cu: inst.op = RdnaOp::GLOBAL_STORE_DWORD; break;
        case 0x1Fu: inst.op = RdnaOp::GLOBAL_STORE_DWORDX4; break;
        case 0x34u: inst.op = RdnaOp::SCRATCH_LOAD_DWORD; break;
        case 0x3Cu: inst.op = RdnaOp::SCRATCH_STORE_DWORD; break;
        default:    inst.op = RdnaOp::GLOBAL_LOAD_DWORD; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeVectorDestination(vdata);
    inst.src[0] = DecodeVectorSource(vaddr, pc);
    i32 signedFlatOffset = static_cast<i32>(w0 & 0x1FFFu);
    if (signedFlatOffset & 0x1000) {
        signedFlatOffset |= ~0x1FFF; // Sign-extend 13-bit
    }
    inst.src[1].kind = OperandKind::InlineIntConstant;
    inst.src[1].value = static_cast<u32>(signedFlatOffset);
    inst.src[1].signedVal = signedFlatOffset;
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeDs(std::span<const u32> code, u32 wordIndex) const {
    const u32 w0 = code[wordIndex];
    const u32 w1 = (wordIndex + 1 < code.size()) ? code[wordIndex + 1] : 0u;
    const u32 pc = wordIndex * 4;
    const u32 opId = (w0 >> 18u) & 0xFFu;
    const u32 addr = w1 & 0xFFu;
    const u32 data0 = (w1 >> 8u) & 0xFFu;
    const u32 vdst = (w1 >> 24u) & 0xFFu;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::DS;
    inst.opcode = static_cast<u16>(opId);
    inst.wordCount = 2;
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;
    inst.sourceCount = 2;

    switch (opId) {
        case 0x00u: inst.op = RdnaOp::DS_ADD_U32; break;
        case 0x01u: inst.op = RdnaOp::DS_SUB_U32; break;
        case 0x0Du: inst.op = RdnaOp::DS_WRITE_B32; break;
        case 0x36u: inst.op = RdnaOp::DS_READ_B32; break;
        case 0x1Eu: inst.op = RdnaOp::DS_WRITE_B64; break;
        case 0x76u: inst.op = RdnaOp::DS_READ_B64; break;
        // PS5 gfx1013 64-bit LDS instructions
        case 0x80u: inst.op = RdnaOp::DS_ADD_SRC2_U64; break;
        case 0x81u: inst.op = RdnaOp::DS_SUB_SRC2_U64; break;
        case 0x88u: inst.op = RdnaOp::DS_AND_SRC2_B64; break;
        case 0x89u: inst.op = RdnaOp::DS_OR_SRC2_B64; break;
        case 0x8Au: inst.op = RdnaOp::DS_XOR_SRC2_B64; break;
        case 0x8Du: inst.op = RdnaOp::DS_WRITE_SRC2_B64; break;
        default:    inst.op = RdnaOp::DS_ADD_U32; break;
    }
    inst.opcodeName = RdnaOpToString(inst.op);

    inst.dst = DecodeVectorDestination(vdst);
    inst.src[0] = DecodeVectorSource(addr, pc);
    inst.src[1] = DecodeVectorSource(data0, pc);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeExp(std::span<const u32> code, u32 wordIndex) const {
    const u32 w0 = code[wordIndex];
    const u32 w1 = (wordIndex + 1 < code.size()) ? code[wordIndex + 1] : 0u;
    const u32 pc = wordIndex * 4;

    RdnaInstruction inst;
    inst.pc = pc;
    inst.family = RdnaFamily::EXP;
    inst.op = RdnaOp::EXP;
    inst.opcodeName = "exp";
    inst.wordCount = 2;
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;
    inst.sourceCount = 4;

    inst.src[0] = DecodeVectorSource(w1 & 0xFFu, pc);
    inst.src[1] = DecodeVectorSource((w1 >> 8u) & 0xFFu, pc);
    inst.src[2] = DecodeVectorSource((w1 >> 16u) & 0xFFu, pc);
    inst.src[3] = DecodeVectorSource((w1 >> 24u) & 0xFFu, pc);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeInstruction(std::span<const u32> code, u32 wordIndex) const {
    if (wordIndex >= code.size()) {
        throw std::out_of_range("wordIndex out of bounds in DecodeInstruction");
    }

    const RdnaFamily family = GetInstructionFamily(code[wordIndex]);
    switch (family) {
        case RdnaFamily::SOP1:  return DecodeSop1(code, wordIndex);
        case RdnaFamily::SOP2:  return DecodeSop2(code, wordIndex);
        case RdnaFamily::SOPK:  return DecodeSopk(code, wordIndex);
        case RdnaFamily::SOPC:  return DecodeSopc(code, wordIndex);
        case RdnaFamily::SOPP:  return DecodeSopp(code, wordIndex);
        case RdnaFamily::VOP1:  return DecodeVop1(code, wordIndex);
        case RdnaFamily::VOP2:  return DecodeVop2(code, wordIndex);
        case RdnaFamily::VOP3:  return DecodeVop3(code, wordIndex);
        case RdnaFamily::VOP3P: return DecodeVop3p(code, wordIndex);
        case RdnaFamily::VOPC:  return DecodeVopc(code, wordIndex);
        case RdnaFamily::VINTRP: return DecodeVintrp(code, wordIndex);
        case RdnaFamily::SMEM:  return DecodeSmem(code, wordIndex);
        case RdnaFamily::MUBUF: return DecodeMubuf(code, wordIndex);
        case RdnaFamily::MTBUF: return DecodeMtbuf(code, wordIndex);
        case RdnaFamily::FLAT:  return DecodeFlat(code, wordIndex);
        case RdnaFamily::DS:    return DecodeDs(code, wordIndex);
        case RdnaFamily::EXP:   return DecodeExp(code, wordIndex);
        case RdnaFamily::MIMG:  return DecodeMimg(code, wordIndex);
        default: break;
    }

    // Fallback unknown instruction
    RdnaInstruction inst;
    inst.pc = wordIndex * 4;
    inst.family = RdnaFamily::Unknown;
    inst.op = RdnaOp::Invalid;
    inst.opcodeName = "unknown";
    inst.wordCount = 1;
    inst.rawWords[0] = code[wordIndex];
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeMimg(std::span<const u32> code, u32 wordIndex) const {
    RdnaInstruction inst;
    inst.family = RdnaFamily::MIMG;
    inst.pc = wordIndex * sizeof(u32);
    inst.wordCount = 2; // 64-bit instruction

    if (wordIndex + 1 >= code.size()) return inst;

    u32 w0 = code[wordIndex];
    u32 w1 = code[wordIndex + 1];
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;

    u32 op = (w0 >> 18) & 0x7F;
    u32 dmask = (w0 >> 8) & 0xF;

    u32 vdata = w1 & 0xFF;
    u32 vaddr = (w1 >> 8) & 0xFF;
    u32 srsrc = (w1 >> 16) & 0x1F;
    u32 ssamp = (w1 >> 21) & 0x1F;

    switch (op) {
        case 0x00: inst.op = RdnaOp::IMAGE_LOAD; inst.opcodeName = "image_load"; break;
        case 0x01: inst.op = RdnaOp::IMAGE_LOAD_MIP; inst.opcodeName = "image_load_mip"; break;
        case 0x02: inst.op = RdnaOp::IMAGE_STORE; inst.opcodeName = "image_store"; break;
        case 0x03: inst.op = RdnaOp::IMAGE_STORE_MIP; inst.opcodeName = "image_store_mip"; break;
        case 0x20: inst.op = RdnaOp::IMAGE_SAMPLE; inst.opcodeName = "image_sample"; break;
        case 0x21: inst.op = RdnaOp::IMAGE_SAMPLE_CL; inst.opcodeName = "image_sample_cl"; break;
        case 0x22: inst.op = RdnaOp::IMAGE_SAMPLE_D; inst.opcodeName = "image_sample_d"; break;
        case 0x24: inst.op = RdnaOp::IMAGE_SAMPLE_L; inst.opcodeName = "image_sample_l"; break;
        case 0x25: inst.op = RdnaOp::IMAGE_SAMPLE_B; inst.opcodeName = "image_sample_b"; break;
        case 0x26: inst.op = RdnaOp::IMAGE_SAMPLE_LZ; inst.opcodeName = "image_sample_lz"; break;
        case 0x27: inst.op = RdnaOp::IMAGE_SAMPLE_C; inst.opcodeName = "image_sample_c"; break;
        case 0x40: inst.op = RdnaOp::IMAGE_GATHER4; inst.opcodeName = "image_gather4"; break;
        default:   inst.op = RdnaOp::IMAGE_SAMPLE; inst.opcodeName = "image_sample"; break;
    }

    inst.dst.kind = OperandKind::VectorRegister;
    inst.dst.reg = vdata;

    inst.sourceCount = 3;
    inst.src[0].kind = OperandKind::VectorRegister;
    inst.src[0].reg = vaddr;
    inst.src[1].kind = OperandKind::ScalarRegister;
    inst.src[1].reg = srsrc * 4; // 256-bit T# descriptor in 8 SGPRs
    inst.src[2].kind = OperandKind::ScalarRegister;
    inst.src[2].reg = ssamp * 4; // 128-bit S# descriptor in 4 SGPRs

    (void)dmask;
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeVop3p(std::span<const u32> code, u32 wordIndex) const {
    RdnaInstruction inst;
    inst.family = RdnaFamily::VOP3P;
    inst.pc = wordIndex * 4;
    inst.wordCount = 2;
    if (wordIndex + 1 >= code.size()) return inst;

    const u32 w0 = code[wordIndex];
    const u32 w1 = code[wordIndex + 1];
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;

    const u32 opId = (w0 >> 16u) & 0x7Fu;
    const u32 vd = w0 & 0xFFu;
    const u32 s0 = w1 & 0x1FFu;
    const u32 s1 = (w1 >> 9u) & 0x1FFu;
    const u32 s2 = (w1 >> 18u) & 0x1FFu;

    switch (opId) {
        case 0x00: inst.op = RdnaOp::V_PK_MAD_I16; inst.opcodeName = "v_pk_mad_i16"; break;
        case 0x01: inst.op = RdnaOp::V_PK_MUL_LO_U16; inst.opcodeName = "v_pk_mul_lo_u16"; break;
        case 0x02: inst.op = RdnaOp::V_PK_ADD_I16; inst.opcodeName = "v_pk_add_i16"; break;
        case 0x03: inst.op = RdnaOp::V_PK_SUB_I16; inst.opcodeName = "v_pk_sub_i16"; break;
        case 0x04: inst.op = RdnaOp::V_PK_LSHLREV_B16; inst.opcodeName = "v_pk_lshlrev_b16"; break;
        case 0x05: inst.op = RdnaOp::V_PK_LSHRREV_B16; inst.opcodeName = "v_pk_lshrrev_b16"; break;
        case 0x06: inst.op = RdnaOp::V_PK_ASHRREV_I16; inst.opcodeName = "v_pk_ashrrev_i16"; break;
        case 0x07: inst.op = RdnaOp::V_PK_MAX_I16; inst.opcodeName = "v_pk_max_i16"; break;
        case 0x08: inst.op = RdnaOp::V_PK_MIN_I16; inst.opcodeName = "v_pk_min_i16"; break;
        case 0x09: inst.op = RdnaOp::V_PK_MAD_U16; inst.opcodeName = "v_pk_mad_u16"; break;
        case 0x0A: inst.op = RdnaOp::V_PK_ADD_U16; inst.opcodeName = "v_pk_add_u16"; break;
        case 0x0B: inst.op = RdnaOp::V_PK_SUB_U16; inst.opcodeName = "v_pk_sub_u16"; break;
        case 0x0C: inst.op = RdnaOp::V_PK_MAX_U16; inst.opcodeName = "v_pk_max_u16"; break;
        case 0x0D: inst.op = RdnaOp::V_PK_MIN_U16; inst.opcodeName = "v_pk_min_u16"; break;
        case 0x0E: inst.op = RdnaOp::V_PK_FMA_F16; inst.opcodeName = "v_pk_fma_f16"; break;
        case 0x0F: inst.op = RdnaOp::V_PK_ADD_F16; inst.opcodeName = "v_pk_add_f16"; break;
        case 0x10: inst.op = RdnaOp::V_PK_MUL_F16; inst.opcodeName = "v_pk_mul_f16"; break;
        case 0x11: inst.op = RdnaOp::V_PK_MIN_F16; inst.opcodeName = "v_pk_min_f16"; break;
        case 0x12: inst.op = RdnaOp::V_PK_MAX_F16; inst.opcodeName = "v_pk_max_f16"; break;
        default:   inst.op = RdnaOp::V_PK_FMA_F16; inst.opcodeName = "v_pk_fma_f16"; break;
    }

    inst.dst = DecodeVectorDestination(vd);
    inst.sourceCount = 3;
    inst.src[0] = DecodeVectorSource(s0, inst.pc);
    inst.src[1] = DecodeVectorSource(s1, inst.pc);
    inst.src[2] = DecodeVectorSource(s2, inst.pc);
    ReadLiterals(code, wordIndex, inst);
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeVintrp(std::span<const u32> code, u32 wordIndex) const {
    RdnaInstruction inst;
    inst.family = RdnaFamily::VINTRP;
    inst.pc = wordIndex * 4;
    inst.wordCount = 1;
    const u32 word = code[wordIndex];
    inst.rawWords[0] = word;

    const u32 opId = (word >> 16u) & 0x3u;
    const u32 vd = (word >> 8u) & 0xFFu;
    const u32 vsrc = word & 0xFFu;
    const u32 attrChan = (word >> 10u) & 0x3u;
    const u32 attr = (word >> 12u) & 0x3Fu;

    switch (opId) {
        case 0: inst.op = RdnaOp::V_INTERP_P1_F32; inst.opcodeName = "v_interp_p1_f32"; break;
        case 1: inst.op = RdnaOp::V_INTERP_P2_F32; inst.opcodeName = "v_interp_p2_f32"; break;
        case 2: inst.op = RdnaOp::V_INTERP_MOV_F32; inst.opcodeName = "v_interp_mov_f32"; break;
        default: inst.op = RdnaOp::V_INTERP_P1_F32; inst.opcodeName = "v_interp_p1_f32"; break;
    }

    inst.dst = DecodeVectorDestination(vd);
    inst.sourceCount = 3;
    inst.src[0] = DecodeVectorSource(vsrc, inst.pc);
    inst.src[1] = RdnaOperand{.kind = OperandKind::InlineIntConstant, .value = attrChan, .signedVal = static_cast<i32>(attrChan)};
    inst.src[2] = RdnaOperand{.kind = OperandKind::InlineIntConstant, .value = attr, .signedVal = static_cast<i32>(attr)};
    return inst;
}

RdnaInstruction RdnaDecoder::DecodeMtbuf(std::span<const u32> code, u32 wordIndex) const {
    RdnaInstruction inst;
    inst.family = RdnaFamily::MTBUF;
    inst.pc = wordIndex * 4;
    inst.wordCount = 2;
    if (wordIndex + 1 >= code.size()) return inst;

    const u32 w0 = code[wordIndex];
    const u32 w1 = code[wordIndex + 1];
    inst.rawWords[0] = w0;
    inst.rawWords[1] = w1;

    const u32 opId = (w0 >> 15u) & 0x1Fu;
    const u32 vdata = w1 & 0xFFu;
    const u32 vaddr = (w1 >> 8u) & 0xFFu;
    const u32 srsrc = (w1 >> 16u) & 0x1Fu;

    switch (opId) {
        case 0: inst.op = RdnaOp::TBUFFER_LOAD_FORMAT_X; inst.opcodeName = "tbuffer_load_format_x"; break;
        case 1: inst.op = RdnaOp::TBUFFER_LOAD_FORMAT_XY; inst.opcodeName = "tbuffer_load_format_xy"; break;
        case 2: inst.op = RdnaOp::TBUFFER_LOAD_FORMAT_XYZ; inst.opcodeName = "tbuffer_load_format_xyz"; break;
        case 3: inst.op = RdnaOp::TBUFFER_LOAD_FORMAT_XYZW; inst.opcodeName = "tbuffer_load_format_xyzw"; break;
        case 4: inst.op = RdnaOp::TBUFFER_STORE_FORMAT_X; inst.opcodeName = "tbuffer_store_format_x"; break;
        case 5: inst.op = RdnaOp::TBUFFER_STORE_FORMAT_XY; inst.opcodeName = "tbuffer_store_format_xy"; break;
        case 6: inst.op = RdnaOp::TBUFFER_STORE_FORMAT_XYZ; inst.opcodeName = "tbuffer_store_format_xyz"; break;
        case 7: inst.op = RdnaOp::TBUFFER_STORE_FORMAT_XYZW; inst.opcodeName = "tbuffer_store_format_xyzw"; break;
        default: inst.op = RdnaOp::TBUFFER_LOAD_FORMAT_XYZW; inst.opcodeName = "tbuffer_load_format_xyzw"; break;
    }

    inst.dst = DecodeVectorDestination(vdata);
    inst.sourceCount = 2;
    inst.src[0] = DecodeVectorSource(vaddr, inst.pc);
    inst.src[1] = DecodeScalarSource(srsrc * 4, inst.pc);
    return inst;
}

std::vector<RdnaInstruction> RdnaDecoder::DecodeProgram(std::span<const u32> code) const {
    std::vector<RdnaInstruction> program;
    u32 wordIndex = 0;
    while (wordIndex < code.size()) {
        RdnaInstruction inst = DecodeInstruction(code, wordIndex);
        program.push_back(inst);
        wordIndex += inst.wordCount;
        if (inst.isEndProgram) {
            break;
        }
    }
    return program;
}

} // namespace Sailor::Shader
