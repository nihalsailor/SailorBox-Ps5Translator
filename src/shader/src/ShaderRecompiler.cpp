#include "shader/ShaderRecompiler.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <iostream>
#include <bit>
#include <cstring>

namespace Sailor::Shader {

ShaderRecompiler::ShaderRecompiler(std::shared_ptr<ShaderCache> cache)
    : m_cache(cache ? cache : std::make_shared<ShaderCache>()) {
}

static IrValue ResolveOperand(const RdnaOperand& op, IrProgram& ir) {
    switch (op.kind) {
        case OperandKind::ScalarRegister: {
            auto it = ir.sgprs.find(op.reg);
            if (it != ir.sgprs.end()) {
                return it->second;
            }
            IrValue val = ir.AllocateValue(IrType::U32);
            ir.sgprs[op.reg] = val;
            return val;
        }
        case OperandKind::VectorRegister: {
            auto it = ir.vgprs.find(op.reg);
            if (it != ir.vgprs.end()) {
                return it->second;
            }
            if (ir.stage == ShaderStage::Vertex && op.reg == 0) {
                IrValue val = ir.Emit(IrOp::LoadVertexIndex, IrType::U32, {}).result;
                ir.vgprs[op.reg] = val;
                return val;
            }
            if (ir.stage == ShaderStage::Compute && op.reg <= 2) {
                IrValue val = ir.Emit(IrOp::LoadGlobalInvocationId, IrType::U32, {IrValue::ImmU32(op.reg)}).result;
                ir.vgprs[op.reg] = val;
                return val;
            }
            IrValue val = ir.AllocateValue(IrType::F32);
            ir.vgprs[op.reg] = val;
            return val;
        }
        case OperandKind::InlineIntConstant: {
            return IrValue::ImmU32(op.value);
        }
        case OperandKind::InlineFloatConstant: {
            return IrValue::ImmF32(std::bit_cast<float>(op.value));
        }
        case OperandKind::LiteralConstant: {
            return IrValue::ImmU32(op.value);
        }
        case OperandKind::Scc: {
            if (ir.scc.id != 0 || ir.scc.isImmediate) {
                if (ir.scc.type == IrType::Bool) return ir.scc;
                return ir.Emit(IrOp::CompareNeq, IrType::Bool, {ir.scc, IrValue::ImmU32(0)}).result;
            }
            return IrValue::ImmBool(false);
        }
        case OperandKind::VccLo:
        case OperandKind::VccHi: {
            if (ir.vcc.id != 0 || ir.vcc.isImmediate) {
                if (ir.vcc.type == IrType::Bool) return ir.vcc;
                return ir.Emit(IrOp::CompareNeq, IrType::Bool, {ir.vcc, IrValue::ImmU32(0)}).result;
            }
            return IrValue::ImmBool(true);
        }
        default:
            return IrValue::ImmU32(0);
    }
}

static void StoreDestination(const RdnaOperand& dst, IrValue val, IrProgram& ir) {
    switch (dst.kind) {
        case OperandKind::ScalarRegister:
            ir.sgprs[dst.reg] = val;
            break;
        case OperandKind::VectorRegister:
            ir.vgprs[dst.reg] = val;
            break;
        case OperandKind::Scc:
            if (val.type != IrType::Bool) {
                val = ir.Emit(IrOp::CompareNeq, IrType::Bool, {val, IrValue::ImmU32(0)}).result;
            }
            ir.scc = val;
            break;
        case OperandKind::VccLo:
        case OperandKind::VccHi:
            if (val.type != IrType::Bool) {
                val = ir.Emit(IrOp::CompareNeq, IrType::Bool, {val, IrValue::ImmU32(0)}).result;
            }
            ir.vcc = val;
            break;
        default:
            break;
    }
}

void ShaderRecompiler::TranslateInstruction(const RdnaInstruction& inst, IrProgram& ir) {
    auto ComputeBufferVaddr = [&](const RdnaInstruction& ins) -> IrValue {
        IrValue vaddr = ResolveOperand(ins.src[0], ir);
        u32 immOffset = (ins.family == RdnaFamily::MUBUF) ? ins.src[2].value : ins.src[1].value;
        if (immOffset != 0) {
            vaddr = ir.Emit(IrOp::Add, IrType::U32, {vaddr, IrValue::ImmU32(immOffset)}).result;
        }
        return vaddr;
    };

    switch (inst.op) {
        // Scalar SOP2
        case RdnaOp::S_ADD_U32:
        case RdnaOp::S_ADD_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Add, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_SUB_U32:
        case RdnaOp::S_SUB_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Sub, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_MUL_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Mul, IrType::I32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_MIN_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::Min, IrType::I32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_MIN_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::Min, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_MAX_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::Max, IrType::I32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_MAX_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::Max, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_AND_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::BitAnd, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_OR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::BitOr, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_XOR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::BitXor, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_ANDN2_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& inv = ir.Emit(IrOp::BitNot, IrType::U32, {s1});
            IrInst& res = ir.Emit(IrOp::BitAnd, IrType::U32, {s0, inv.result});
            StoreDestination(inst.dst, res.result, ir);
            break;
        }
        case RdnaOp::S_ORN2_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& inv = ir.Emit(IrOp::BitNot, IrType::U32, {s1});
            IrInst& res = ir.Emit(IrOp::BitOr, IrType::U32, {s0, inv.result});
            StoreDestination(inst.dst, res.result, ir);
            break;
        }
        case RdnaOp::S_NAND_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& a = ir.Emit(IrOp::BitAnd, IrType::U32, {s0, s1});
            IrInst& res = ir.Emit(IrOp::BitNot, IrType::U32, {a.result});
            StoreDestination(inst.dst, res.result, ir);
            break;
        }
        case RdnaOp::S_NOR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& o = ir.Emit(IrOp::BitOr, IrType::U32, {s0, s1});
            IrInst& res = ir.Emit(IrOp::BitNot, IrType::U32, {o.result});
            StoreDestination(inst.dst, res.result, ir);
            break;
        }
        case RdnaOp::S_XNOR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& x = ir.Emit(IrOp::BitXor, IrType::U32, {s0, s1});
            IrInst& res = ir.Emit(IrOp::BitNot, IrType::U32, {x.result});
            StoreDestination(inst.dst, res.result, ir);
            break;
        }

        case RdnaOp::S_LSHL_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftLeft, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_LSHR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftRightLogical, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_ASHR_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftRightArithmetic, IrType::I32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_CSELECT_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue cond = ir.scc;
            if (cond.id == 0 && !cond.isImmediate) {
                cond = IrValue::ImmBool(false);
            } else if (cond.type != IrType::Bool) {
                cond = ir.Emit(IrOp::CompareNeq, IrType::Bool, {cond, IrValue::ImmU32(0)}).result;
            }
            IrInst& emitted = ir.Emit(IrOp::Select, IrType::U32, {cond, s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }

        // Scalar SOP1
        case RdnaOp::S_MOV_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            StoreDestination(inst.dst, s0, ir);
            break;
        }
        case RdnaOp::S_MOV_B64: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            StoreDestination(inst.dst, s0, ir);
            if (inst.dst.kind == OperandKind::ScalarRegister && inst.src[0].kind == OperandKind::ScalarRegister) {
                IrValue s1 = ResolveOperand(RdnaOperand{OperandKind::ScalarRegister, inst.src[0].reg + 1}, ir);
                ir.sgprs[inst.dst.reg + 1] = s1;
            }
            break;
        }
        case RdnaOp::S_ABS_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Abs, IrType::I32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_NOT_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::BitNot, IrType::U32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }

        // Scalar SOPK
        case RdnaOp::S_MOVK_I32: {
            IrValue s0 = IrValue::ImmU32(inst.src[0].value);
            StoreDestination(inst.dst, s0, ir);
            break;
        }
        case RdnaOp::S_ADDK_I32: {
            IrValue current = ResolveOperand(inst.dst, ir);
            IrValue imm = IrValue::ImmU32(inst.src[0].value);
            IrInst& emitted = ir.Emit(IrOp::Add, IrType::I32, {current, imm});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::S_MULK_I32: {
            IrValue current = ResolveOperand(inst.dst, ir);
            IrValue imm = IrValue::ImmU32(inst.src[0].value);
            IrInst& emitted = ir.Emit(IrOp::Mul, IrType::I32, {current, imm});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }

        // Scalar SOPC
        case RdnaOp::S_CMP_EQ_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareEq, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_EQ_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareEq, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_LG_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareNeq, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_LG_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareNeq, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_LT_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareLt, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_LT_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareLt, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_LE_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareLte, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_LE_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareLte, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_GT_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareGt, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_GT_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareGt, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_GE_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareGte, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }
        case RdnaOp::S_CMP_GE_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareGte, IrType::Bool, {s0, s1});
            ir.scc = emitted.result;
            break;
        }

        // Scalar SOPP
        case RdnaOp::S_NOP:
        case RdnaOp::S_WAITCNT:
        case RdnaOp::S_SLEEP:
            break;
        case RdnaOp::S_BARRIER:
            ir.Emit(IrOp::Barrier, IrType::Void, {});
            break;
        case RdnaOp::S_ENDPGM:
        case RdnaOp::S_CODE_END:
            ir.Emit(IrOp::Return, IrType::Void, {});
            break;

        // Vector VOP1
        case RdnaOp::V_MOV_B32:
        case RdnaOp::V_READFIRSTLANE_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            StoreDestination(inst.dst, s0, ir);
            break;
        }
        case RdnaOp::V_NOT_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::BitNot, IrType::U32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_CVT_F32_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::ConvertI32ToF32, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_CVT_F32_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::ConvertU32ToF32, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_CVT_I32_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::ConvertF32ToI32, IrType::I32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_CVT_U32_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::ConvertF32ToU32, IrType::U32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_SQRT_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Sqrt, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_RSQ_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Rsqrt, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_RCP_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Rcp, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_SIN_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Sin, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_COS_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Cos, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_FLOOR_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Floor, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_CEIL_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Ceil, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_FRACT_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Fract, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_EXP_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Exp2, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_LOG_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Log2, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_TRUNC_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::Trunc, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_RNDNE_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::RoundEven, IrType::F32, {s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }

        // Vector VOP2
        case RdnaOp::V_ADD_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Add, IrType::F32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_SUB_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Sub, IrType::F32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_SUBREV_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Sub, IrType::F32, {s1, s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_MUL_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Mul, IrType::F32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_MIN_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Min, IrType::F32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_MAX_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Max, IrType::F32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_FMAC_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue acc = ResolveOperand(inst.dst, ir);
            IrInst& emitted = ir.Emit(IrOp::Fma, IrType::F32, {s0, s1, acc});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_CNDMASK_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue cond = ir.vcc;
            if (cond.id == 0 && !cond.isImmediate) {
                cond = IrValue::ImmBool(true);
            } else if (cond.type != IrType::Bool) {
                cond = ir.Emit(IrOp::CompareNeq, IrType::Bool, {cond, IrValue::ImmU32(0)}).result;
            }
            IrInst& emitted = ir.Emit(IrOp::Select, IrType::F32, {cond, s1, s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_ADD_NC_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Add, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_SUB_NC_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Sub, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_SUBREV_NC_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Sub, IrType::U32, {s1, s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_ADD_CO_CI_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;

            // Carry-in from VCC condition flag
            IrValue cinBool = ir.vcc;
            if (cinBool.type != IrType::Bool) {
                cinBool = ir.Emit(IrOp::CompareNeq, IrType::Bool, {cinBool, IrValue::ImmU32(0)}).result;
            }
            IrValue cinU32 = ir.Emit(IrOp::Select, IrType::U32, {cinBool, IrValue::ImmU32(1), IrValue::ImmU32(0)}).result;

            // s0 + s1
            IrValue tempSum = ir.Emit(IrOp::Add, IrType::U32, {s0, s1}).result;
            // Carry from s0 + s1 (overflows if tempSum < s0)
            IrValue carry1 = ir.Emit(IrOp::CompareLt, IrType::Bool, {tempSum, s0}).result;

            // tempSum + cin
            IrValue finalSum = ir.Emit(IrOp::Add, IrType::U32, {tempSum, cinU32}).result;
            // Carry from adding carry-in (overflows if finalSum < tempSum)
            IrValue carry2 = ir.Emit(IrOp::CompareLt, IrType::Bool, {finalSum, tempSum}).result;

            // Combined carry-out into VCC: carry1 || carry2
            IrValue carryOut = ir.Emit(IrOp::BitOr, IrType::Bool, {carry1, carry2}).result;
            ir.vcc = carryOut;

            StoreDestination(inst.dst, finalSum, ir);
            break;
        }
        case RdnaOp::V_SUB_CO_CI_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;

            // Borrow-in from VCC condition flag
            IrValue binBool = ir.vcc;
            if (binBool.type != IrType::Bool) {
                binBool = ir.Emit(IrOp::CompareNeq, IrType::Bool, {binBool, IrValue::ImmU32(0)}).result;
            }
            IrValue binU32 = ir.Emit(IrOp::Select, IrType::U32, {binBool, IrValue::ImmU32(1), IrValue::ImmU32(0)}).result;

            // Borrow from s0 - s1 (borrows if s0 < s1)
            IrValue borrow1 = ir.Emit(IrOp::CompareLt, IrType::Bool, {s0, s1}).result;
            IrValue tempDiff = ir.Emit(IrOp::Sub, IrType::U32, {s0, s1}).result;

            // Borrow from tempDiff - bin (borrows if tempDiff < bin)
            IrValue borrow2 = ir.Emit(IrOp::CompareLt, IrType::Bool, {tempDiff, binU32}).result;
            IrValue finalDiff = ir.Emit(IrOp::Sub, IrType::U32, {tempDiff, binU32}).result;

            // Combined borrow-out into VCC: borrow1 || borrow2
            IrValue borrowOut = ir.Emit(IrOp::BitOr, IrType::Bool, {borrow1, borrow2}).result;
            ir.vcc = borrowOut;

            StoreDestination(inst.dst, finalDiff, ir);
            break;
        }
        case RdnaOp::V_SUBREV_CO_CI_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;

            // Borrow-in from VCC condition flag (s1 - s0 - borrow)
            IrValue binBool = ir.vcc;
            if (binBool.type != IrType::Bool) {
                binBool = ir.Emit(IrOp::CompareNeq, IrType::Bool, {binBool, IrValue::ImmU32(0)}).result;
            }
            IrValue binU32 = ir.Emit(IrOp::Select, IrType::U32, {binBool, IrValue::ImmU32(1), IrValue::ImmU32(0)}).result;

            IrValue borrow1 = ir.Emit(IrOp::CompareLt, IrType::Bool, {s1, s0}).result;
            IrValue tempDiff = ir.Emit(IrOp::Sub, IrType::U32, {s1, s0}).result;

            IrValue borrow2 = ir.Emit(IrOp::CompareLt, IrType::Bool, {tempDiff, binU32}).result;
            IrValue finalDiff = ir.Emit(IrOp::Sub, IrType::U32, {tempDiff, binU32}).result;

            IrValue borrowOut = ir.Emit(IrOp::BitOr, IrType::Bool, {borrow1, borrow2}).result;
            ir.vcc = borrowOut;

            StoreDestination(inst.dst, finalDiff, ir);
            break;
        }
        case RdnaOp::V_MIN_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::Min, IrType::I32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_MAX_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::Max, IrType::I32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_MIN_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::Min, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_MAX_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::Max, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_LSHLREV_B32: {
            IrValue shift = ResolveOperand(inst.src[0], ir);
            IrValue val = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftLeft, IrType::U32, {val, shift});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_LSHRREV_B32: {
            IrValue shift = ResolveOperand(inst.src[0], ir);
            IrValue val = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftRightLogical, IrType::U32, {val, shift});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_ASHRREV_I32: {
            IrValue shift = ResolveOperand(inst.src[0], ir);
            IrValue val = ResolveOperand(inst.src[1], ir);
            val.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::ShiftRightArithmetic, IrType::I32, {val, shift});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_AND_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::BitAnd, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_OR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::BitOr, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_XOR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::BitXor, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_XNOR_B32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& xored = ir.Emit(IrOp::BitXor, IrType::U32, {s0, s1});
            IrInst& emitted = ir.Emit(IrOp::BitNot, IrType::U32, {xored.result});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }

        // Vector VOP3
        case RdnaOp::V_FMA_F32:
        case RdnaOp::V_MAD_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue s2 = ResolveOperand(inst.src[2], ir);
            IrInst& emitted = ir.Emit(IrOp::Fma, IrType::F32, {s0, s1, s2});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_MIN3_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue s2 = ResolveOperand(inst.src[2], ir);
            IrInst& m1 = ir.Emit(IrOp::Min, IrType::F32, {s0, s1});
            IrInst& m2 = ir.Emit(IrOp::Min, IrType::F32, {m1.result, s2});
            StoreDestination(inst.dst, m2.result, ir);
            break;
        }
        case RdnaOp::V_MAX3_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue s2 = ResolveOperand(inst.src[2], ir);
            IrInst& m1 = ir.Emit(IrOp::Max, IrType::F32, {s0, s1});
            IrInst& m2 = ir.Emit(IrOp::Max, IrType::F32, {m1.result, s2});
            StoreDestination(inst.dst, m2.result, ir);
            break;
        }
        case RdnaOp::V_MED3_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue s2 = ResolveOperand(inst.src[2], ir);
            IrInst& min01 = ir.Emit(IrOp::Min, IrType::F32, {s0, s1});
            IrInst& max01 = ir.Emit(IrOp::Max, IrType::F32, {s0, s1});
            IrInst& minMax = ir.Emit(IrOp::Min, IrType::F32, {max01.result, s2});
            IrInst& med = ir.Emit(IrOp::Max, IrType::F32, {min01.result, minMax.result});
            StoreDestination(inst.dst, med.result, ir);
            break;
        }

        // Vector VOPC - Floating Point (F32)
        case RdnaOp::V_CMP_LT_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            if (s0.isImmediate && s0.type == IrType::U32) s0 = IrValue::ImmF32(std::bit_cast<float>(s0.immU32));
            if (s1.isImmediate && s1.type == IrType::U32) s1 = IrValue::ImmF32(std::bit_cast<float>(s1.immU32));
            s0.type = IrType::F32;
            s1.type = IrType::F32;
            IrInst& emitted = ir.Emit(IrOp::CompareLt, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_EQ_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            if (s0.isImmediate && s0.type == IrType::U32) s0 = IrValue::ImmF32(std::bit_cast<float>(s0.immU32));
            if (s1.isImmediate && s1.type == IrType::U32) s1 = IrValue::ImmF32(std::bit_cast<float>(s1.immU32));
            s0.type = IrType::F32;
            s1.type = IrType::F32;
            IrInst& emitted = ir.Emit(IrOp::CompareEq, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_LE_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            if (s0.isImmediate && s0.type == IrType::U32) s0 = IrValue::ImmF32(std::bit_cast<float>(s0.immU32));
            if (s1.isImmediate && s1.type == IrType::U32) s1 = IrValue::ImmF32(std::bit_cast<float>(s1.immU32));
            s0.type = IrType::F32;
            s1.type = IrType::F32;
            IrInst& emitted = ir.Emit(IrOp::CompareLte, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_GT_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            if (s0.isImmediate && s0.type == IrType::U32) s0 = IrValue::ImmF32(std::bit_cast<float>(s0.immU32));
            if (s1.isImmediate && s1.type == IrType::U32) s1 = IrValue::ImmF32(std::bit_cast<float>(s1.immU32));
            s0.type = IrType::F32;
            s1.type = IrType::F32;
            IrInst& emitted = ir.Emit(IrOp::CompareGt, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_GE_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            if (s0.isImmediate && s0.type == IrType::U32) s0 = IrValue::ImmF32(std::bit_cast<float>(s0.immU32));
            if (s1.isImmediate && s1.type == IrType::U32) s1 = IrValue::ImmF32(std::bit_cast<float>(s1.immU32));
            s0.type = IrType::F32;
            s1.type = IrType::F32;
            IrInst& emitted = ir.Emit(IrOp::CompareGte, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_LG_F32:
        case RdnaOp::V_CMP_NEQ_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            if (s0.isImmediate && s0.type == IrType::U32) s0 = IrValue::ImmF32(std::bit_cast<float>(s0.immU32));
            if (s1.isImmediate && s1.type == IrType::U32) s1 = IrValue::ImmF32(std::bit_cast<float>(s1.immU32));
            s0.type = IrType::F32;
            s1.type = IrType::F32;
            IrInst& emitted = ir.Emit(IrOp::CompareNeq, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }

        // Vector VOPC - Integer (I32 / U32)
        case RdnaOp::V_CMP_LT_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareLt, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_LT_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareLt, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_EQ_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareEq, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_EQ_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareEq, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_LE_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareLte, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_LE_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareLte, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_GT_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareGt, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_GT_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareGt, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_GE_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareGte, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_GE_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareGte, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_NE_I32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::I32;
            s1.type = IrType::I32;
            IrInst& emitted = ir.Emit(IrOp::CompareNeq, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }
        case RdnaOp::V_CMP_NE_U32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            s0.type = IrType::U32;
            s1.type = IrType::U32;
            IrInst& emitted = ir.Emit(IrOp::CompareNeq, IrType::Bool, {s0, s1});
            ir.vcc = emitted.result;
            break;
        }

        // LDS DS Instructions
        case RdnaOp::DS_WRITE_B32:
        case RdnaOp::DS_WRITE_B64:
        case RdnaOp::DS_WRITE_SRC2_B64: {
            IrValue addr = ResolveOperand(inst.src[0], ir);
            IrValue data = ResolveOperand(inst.src[1], ir);
            ir.Emit(IrOp::SharedStore, IrType::Void, {addr, data});
            break;
        }
        case RdnaOp::DS_READ_B32:
        case RdnaOp::DS_READ_B64: {
            IrValue addr = ResolveOperand(inst.src[0], ir);
            IrInst& emitted = ir.Emit(IrOp::SharedLoad, IrType::U32, {addr});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::DS_ADD_U32:
        case RdnaOp::DS_ADD_SRC2_U64: {
            IrValue addr = ResolveOperand(inst.src[0], ir);
            IrValue data = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::SharedAtomicAdd, IrType::U32, {addr, data});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::DS_SUB_U32:
        case RdnaOp::DS_SUB_SRC2_U64: {
            IrValue addr = ResolveOperand(inst.src[0], ir);
            IrValue data = ResolveOperand(inst.src[1], ir);
            IrValue negData = ir.Emit(IrOp::Sub, IrType::U32, {IrValue::ImmU32(0), data}).result;
            IrInst& emitted = ir.Emit(IrOp::SharedAtomicAdd, IrType::U32, {addr, negData});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }

        // Scalar Memory SMEM Loads
        case RdnaOp::S_LOAD_DWORD:
        case RdnaOp::S_BUFFER_LOAD_DWORD: {
            IrValue sbase = ResolveOperand(inst.src[0], ir);
            if (inst.src[1].value != 0) {
                sbase = ir.Emit(IrOp::Add, IrType::U32, {sbase, IrValue::ImmU32(inst.src[1].value)}).result;
            }
            IrInst& loaded = ir.Emit(IrOp::BufferLoad, IrType::Vec4, {sbase, IrValue::ImmU32(0)});
            IrInst& extract = ir.Emit(IrOp::CompositeExtract, IrType::F32, {loaded.result, IrValue::ImmU32(0)});
            StoreDestination(inst.dst, extract.result, ir);
            break;
        }
        case RdnaOp::S_LOAD_DWORDX2:
        case RdnaOp::S_BUFFER_LOAD_DWORDX2:
        case RdnaOp::S_LOAD_DWORDX4:
        case RdnaOp::S_BUFFER_LOAD_DWORDX4:
        case RdnaOp::S_LOAD_DWORDX8:
        case RdnaOp::S_LOAD_DWORDX16: {
            IrValue sbase = ResolveOperand(inst.src[0], ir);
            if (inst.src[1].value != 0) {
                sbase = ir.Emit(IrOp::Add, IrType::U32, {sbase, IrValue::ImmU32(inst.src[1].value)}).result;
            }
            u32 count = (inst.op == RdnaOp::S_LOAD_DWORDX2 || inst.op == RdnaOp::S_BUFFER_LOAD_DWORDX2) ? 2u
                      : (inst.op == RdnaOp::S_LOAD_DWORDX4 || inst.op == RdnaOp::S_BUFFER_LOAD_DWORDX4) ? 4u
                      : (inst.op == RdnaOp::S_LOAD_DWORDX8) ? 8u : 16u;
            IrValue currentLoaded;
            for (u32 i = 0; i < count; ++i) {
                if ((i % 4) == 0) {
                    IrValue offsetVal = (i == 0) ? sbase : ir.Emit(IrOp::Add, IrType::U32, {sbase, IrValue::ImmU32(i * 4)}).result;
                    currentLoaded = ir.Emit(IrOp::BufferLoad, IrType::Vec4, {offsetVal, IrValue::ImmU32(0)}).result;
                }
                if (inst.dst.reg + i <= 105) {
                    IrInst& extract = ir.Emit(IrOp::CompositeExtract, IrType::F32, {currentLoaded, IrValue::ImmU32(i % 4)});
                    ir.sgprs[inst.dst.reg + i] = extract.result;
                }
            }
            break;
        }

        // Scalar Memory SMEM Stores
        case RdnaOp::S_STORE_DWORD:
        case RdnaOp::S_BUFFER_STORE_DWORD: {
            IrValue sbase = ResolveOperand(inst.src[0], ir);
            if (inst.src[1].value != 0) {
                sbase = ir.Emit(IrOp::Add, IrType::U32, {sbase, IrValue::ImmU32(inst.src[1].value)}).result;
            }
            IrValue sdata = ResolveOperand(inst.dst, ir);
            ir.Emit(IrOp::BufferStore, IrType::Void, {sbase, sdata});
            break;
        }
        case RdnaOp::S_STORE_DWORDX2: {
            IrValue sbase = ResolveOperand(inst.src[0], ir);
            if (inst.src[1].value != 0) {
                sbase = ir.Emit(IrOp::Add, IrType::U32, {sbase, IrValue::ImmU32(inst.src[1].value)}).result;
            }
            IrValue d0 = ResolveOperand(RdnaOperand{OperandKind::ScalarRegister, static_cast<u32>(inst.dst.reg + 0)}, ir);
            IrValue d1 = ResolveOperand(RdnaOperand{OperandKind::ScalarRegister, static_cast<u32>(inst.dst.reg + 1)}, ir);
            IrInst& vec = ir.Emit(IrOp::CompositeConstruct, IrType::Vec4, {d0, d1, IrValue::ImmF32(0.0f), IrValue::ImmF32(0.0f)});
            ir.Emit(IrOp::BufferStore, IrType::Void, {sbase, vec.result});
            break;
        }
        case RdnaOp::S_STORE_DWORDX4: {
            IrValue sbase = ResolveOperand(inst.src[0], ir);
            if (inst.src[1].value != 0) {
                sbase = ir.Emit(IrOp::Add, IrType::U32, {sbase, IrValue::ImmU32(inst.src[1].value)}).result;
            }
            IrValue c0 = ResolveOperand(RdnaOperand{OperandKind::ScalarRegister, static_cast<u32>(inst.dst.reg + 0)}, ir);
            IrValue c1 = ResolveOperand(RdnaOperand{OperandKind::ScalarRegister, static_cast<u32>(inst.dst.reg + 1)}, ir);
            IrValue c2 = ResolveOperand(RdnaOperand{OperandKind::ScalarRegister, static_cast<u32>(inst.dst.reg + 2)}, ir);
            IrValue c3 = ResolveOperand(RdnaOperand{OperandKind::ScalarRegister, static_cast<u32>(inst.dst.reg + 3)}, ir);
            IrInst& vec = ir.Emit(IrOp::CompositeConstruct, IrType::Vec4, {c0, c1, c2, c3});
            ir.Emit(IrOp::BufferStore, IrType::Void, {sbase, vec.result});
            break;
        }

        // Vector Buffer MUBUF & FLAT Multi-DWORD Loads
        case RdnaOp::BUFFER_LOAD_FORMAT_XY:
        case RdnaOp::BUFFER_LOAD_FORMAT_XYZ:
        case RdnaOp::BUFFER_LOAD_FORMAT_XYZW:
        case RdnaOp::BUFFER_LOAD_DWORDX4:
        case RdnaOp::GLOBAL_LOAD_DWORDX4:
        case RdnaOp::FLAT_LOAD_DWORDX4:
        case RdnaOp::TBUFFER_LOAD_FORMAT_XYZW:
        case RdnaOp::TBUFFER_LOAD_FORMAT_XYZ:
        case RdnaOp::TBUFFER_LOAD_FORMAT_XY: {
            IrValue vaddr = ComputeBufferVaddr(inst);
            IrValue srsrc = (inst.sourceCount > 1) ? ResolveOperand(inst.src[1], ir) : IrValue::ImmU32(0);
            IrInst& emitted = ir.Emit(IrOp::BufferLoad, IrType::Vec4, {vaddr, srsrc});
            u32 compCount = (inst.op == RdnaOp::BUFFER_LOAD_FORMAT_XY || inst.op == RdnaOp::TBUFFER_LOAD_FORMAT_XY) ? 2u
                          : (inst.op == RdnaOp::BUFFER_LOAD_FORMAT_XYZ || inst.op == RdnaOp::TBUFFER_LOAD_FORMAT_XYZ) ? 3u
                          : 4u;
            for (u32 i = 0; i < compCount; ++i) {
                if (inst.dst.reg + i < 256) {
                    IrInst& extract = ir.Emit(IrOp::CompositeExtract, IrType::F32, {emitted.result, IrValue::ImmU32(i)});
                    ir.vgprs[inst.dst.reg + i] = extract.result;
                }
            }
            break;
        }

        // Vector Buffer MUBUF, MTBUF & FLAT 4-DWORD Stores
        case RdnaOp::BUFFER_STORE_FORMAT_XYZW:
        case RdnaOp::BUFFER_STORE_DWORDX4:
        case RdnaOp::GLOBAL_STORE_DWORDX4:
        case RdnaOp::FLAT_STORE_DWORDX4:
        case RdnaOp::TBUFFER_STORE_FORMAT_XYZW: {
            IrValue c0 = ResolveOperand(RdnaOperand{OperandKind::VectorRegister, static_cast<u32>(inst.dst.reg + 0)}, ir);
            IrValue c1 = ResolveOperand(RdnaOperand{OperandKind::VectorRegister, static_cast<u32>(inst.dst.reg + 1)}, ir);
            IrValue c2 = ResolveOperand(RdnaOperand{OperandKind::VectorRegister, static_cast<u32>(inst.dst.reg + 2)}, ir);
            IrValue c3 = ResolveOperand(RdnaOperand{OperandKind::VectorRegister, static_cast<u32>(inst.dst.reg + 3)}, ir);
            IrInst& vec = ir.Emit(IrOp::CompositeConstruct, IrType::Vec4, {c0, c1, c2, c3});
            IrValue vaddr = ComputeBufferVaddr(inst);
            ir.Emit(IrOp::BufferStore, IrType::Void, {vaddr, vec.result});
            break;
        }

        // Vector Buffer MUBUF, MTBUF & FLAT 1-DWORD Loads
        case RdnaOp::BUFFER_LOAD_FORMAT_X:
        case RdnaOp::BUFFER_LOAD_DWORD:
        case RdnaOp::GLOBAL_LOAD_DWORD:
        case RdnaOp::FLAT_LOAD_DWORD:
        case RdnaOp::SCRATCH_LOAD_DWORD:
        case RdnaOp::TBUFFER_LOAD_FORMAT_X: {
            IrValue vaddr = ComputeBufferVaddr(inst);
            IrValue srsrc = (inst.sourceCount > 1) ? ResolveOperand(inst.src[1], ir) : IrValue::ImmU32(0);
            IrInst& emitted = ir.Emit(IrOp::BufferLoad, IrType::Vec4, {vaddr, srsrc});
            IrInst& extract = ir.Emit(IrOp::CompositeExtract, IrType::F32, {emitted.result, IrValue::ImmU32(0)});
            StoreDestination(inst.dst, extract.result, ir);
            break;
        }

        // Vector Buffer MUBUF, MTBUF & FLAT 1-DWORD Stores
        case RdnaOp::BUFFER_STORE_DWORD:
        case RdnaOp::GLOBAL_STORE_DWORD:
        case RdnaOp::FLAT_STORE_DWORD:
        case RdnaOp::SCRATCH_STORE_DWORD:
        case RdnaOp::TBUFFER_STORE_FORMAT_X: {
            IrValue vdata = ResolveOperand(inst.dst, ir);
            IrValue vaddr = ComputeBufferVaddr(inst);
            ir.Emit(IrOp::BufferStore, IrType::Void, {vaddr, vdata});
            break;
        }

        // Export EXP
        case RdnaOp::EXP: {
            IrValue v0 = ResolveOperand(inst.src[0], ir);
            IrValue v1 = ResolveOperand(inst.src[1], ir);
            IrValue v2 = ResolveOperand(inst.src[2], ir);
            IrValue v3 = ResolveOperand(inst.src[3], ir);
            IrInst& vec = ir.Emit(IrOp::CompositeConstruct, IrType::Vec4, {v0, v1, v2, v3});
            ir.Emit(IrOp::StoreOutput, IrType::Void, {vec.result});
            break;
        }

        // MIMG Texture Sampling & Storage
        case RdnaOp::IMAGE_STORE:
        case RdnaOp::IMAGE_STORE_MIP: {
            IrValue vdata = ResolveOperand(inst.dst, ir);
            IrValue uv = ResolveOperand(inst.src[0], ir);
            IrValue rsrc = ResolveOperand(inst.src[1], ir);
            ir.Emit(IrOp::ImageStore, IrType::Void, {uv, rsrc, vdata});
            break;
        }
        case RdnaOp::IMAGE_SAMPLE:
        case RdnaOp::IMAGE_SAMPLE_CL:
        case RdnaOp::IMAGE_SAMPLE_D:
        case RdnaOp::IMAGE_SAMPLE_L:
        case RdnaOp::IMAGE_SAMPLE_B:
        case RdnaOp::IMAGE_SAMPLE_LZ:
        case RdnaOp::IMAGE_SAMPLE_C:
        case RdnaOp::IMAGE_GATHER4:
        case RdnaOp::IMAGE_LOAD:
        case RdnaOp::IMAGE_LOAD_MIP: {
            IrValue u = ResolveOperand(inst.src[0], ir);
            IrValue v = ResolveOperand(RdnaOperand{OperandKind::VectorRegister, inst.src[0].reg + 1}, ir);
            IrValue rsrc = ResolveOperand(inst.src[1], ir);
            IrValue samp = ResolveOperand(inst.src[2], ir);
            IrOp op = (inst.op == RdnaOp::IMAGE_LOAD || inst.op == RdnaOp::IMAGE_LOAD_MIP)
                ? IrOp::ImageLoad : IrOp::ImageSample;
            IrInst& sampled = ir.Emit(op, IrType::Vec4, {u, v, rsrc, samp});
            for (u32 i = 0; i < 4; ++i) {
                if (inst.dst.reg + i < 256) {
                    IrInst& extract = ir.Emit(IrOp::CompositeExtract, IrType::F32, {sampled.result, IrValue::ImmU32(i)});
                    ir.vgprs[inst.dst.reg + i] = extract.result;
                }
            }
            break;
        }

        // VOP3P Packed Math
        case RdnaOp::V_PK_ADD_F16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Add, IrType::F32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_ADD_U16:
        case RdnaOp::V_PK_ADD_I16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Add, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_SUB_U16:
        case RdnaOp::V_PK_SUB_I16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Sub, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_MUL_F16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Mul, IrType::F32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_MUL_LO_U16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Mul, IrType::U32, {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_FMA_F16:
        case RdnaOp::V_PK_MAD_I16:
        case RdnaOp::V_PK_MAD_U16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrValue s2 = ResolveOperand(inst.src[2], ir);
            IrInst& emitted = ir.Emit(IrOp::Fma, IrType::F32, {s0, s1, s2});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_MIN_F16:
        case RdnaOp::V_PK_MIN_U16:
        case RdnaOp::V_PK_MIN_I16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Min, (inst.op == RdnaOp::V_PK_MIN_F16 ? IrType::F32 : IrType::U32), {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_MAX_F16:
        case RdnaOp::V_PK_MAX_U16:
        case RdnaOp::V_PK_MAX_I16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::Max, (inst.op == RdnaOp::V_PK_MAX_F16 ? IrType::F32 : IrType::U32), {s0, s1});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_LSHLREV_B16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftLeft, IrType::U32, {s1, s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_LSHRREV_B16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftRightLogical, IrType::U32, {s1, s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }
        case RdnaOp::V_PK_ASHRREV_I16: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            IrValue s1 = ResolveOperand(inst.src[1], ir);
            IrInst& emitted = ir.Emit(IrOp::ShiftRightArithmetic, IrType::U32, {s1, s0});
            StoreDestination(inst.dst, emitted.result, ir);
            break;
        }

        // VINTRP Parameter Interpolation
        case RdnaOp::V_INTERP_P1_F32:
        case RdnaOp::V_INTERP_P2_F32:
        case RdnaOp::V_INTERP_MOV_F32: {
            IrValue s0 = ResolveOperand(inst.src[0], ir);
            StoreDestination(inst.dst, s0, ir);
            break;
        }

        // SOPP Control Flow Branches
        case RdnaOp::S_BRANCH: {
            ir.Emit(IrOp::Branch, IrType::Void, {IrValue::ImmU32(inst.branchTarget)});
            break;
        }

        case RdnaOp::S_CBRANCH_SCC0:
        case RdnaOp::S_CBRANCH_SCC1:
        case RdnaOp::S_CBRANCH_VCCZ:
        case RdnaOp::S_CBRANCH_VCCNZ:
        case RdnaOp::S_CBRANCH_EXECZ:
        case RdnaOp::S_CBRANCH_EXECNZ: {
            IrValue cond = (inst.op == RdnaOp::S_CBRANCH_SCC0 || inst.op == RdnaOp::S_CBRANCH_SCC1)
                ? ir.scc
                : (inst.op == RdnaOp::S_CBRANCH_EXECZ || inst.op == RdnaOp::S_CBRANCH_EXECNZ)
                    ? ir.exec
                    : ir.vcc;
            if (cond.id == 0 && !cond.isImmediate) cond = IrValue::ImmBool(true);

            // Invert condition for Z (branch on zero / false) opcodes
            if (inst.op == RdnaOp::S_CBRANCH_SCC0 || inst.op == RdnaOp::S_CBRANCH_VCCZ || inst.op == RdnaOp::S_CBRANCH_EXECZ) {
                cond = ir.Emit(IrOp::CompareEq, IrType::Bool, {cond, IrValue::ImmBool(false)}).result;
            }

            u32 fallthroughPc = inst.pc + static_cast<u32>(inst.wordCount * 4);
            ir.Emit(IrOp::BranchConditional, IrType::Void, {cond, IrValue::ImmU32(inst.branchTarget), IrValue::ImmU32(fallthroughPc)});
            break;
        }

        default:
            break;
    }
}

std::vector<u32> ShaderRecompiler::EmitSpirvFromIr(const IrProgram& ir) {
    SpirvBuilder spv(Spv::Version1_5);
    spv.AddCapability(Spv::Capability::Shader);

    // Import GLSL extended instructions
    const u32 glslExt = spv.ImportExtInst("GLSL.std.450");

    spv.SetMemoryModel(Spv::AddressingModel::Logical, Spv::MemoryModel::GLSL450);

    const u32 typeVoid = spv.TypeVoid();
    const u32 typeFloat = spv.TypeFloat(32);
    const u32 typeVec4 = spv.TypeVector(typeFloat, 4);
    const u32 typeU32 = spv.TypeInt(32, false);
    const u32 typeI32 = spv.TypeInt(32, true);
    const u32 typeBool = spv.TypeBool();
    (void)typeI32;
    (void)typeBool;

    // Global memory resources: Workgroup LDS, StorageBuffer and Sampled Images
    bool hasShared = false;
    bool hasBuffer = false;
    bool hasImage = false;
    for (const auto& blk : ir.blocks) {
        for (const auto& inst : blk->instructions) {
            if (inst.op == IrOp::SharedLoad || inst.op == IrOp::SharedStore || inst.op == IrOp::SharedAtomicAdd) {
                hasShared = true;
            }
            if (inst.op == IrOp::BufferLoad || inst.op == IrOp::BufferStore) {
                hasBuffer = true;
            }
            if (inst.op == IrOp::ImageSample || inst.op == IrOp::ImageLoad || inst.op == IrOp::ImageStore) {
                hasImage = true;
            }
        }
    }

    // Interface variables
    u32 outPosVar = 0;
    u32 outColorVar = 0;
    u32 inVertexIndexVar = 0;
    u32 inGlobalIdVar = 0;
    std::vector<u32> interfaceIds;

    u32 ldsVar = 0;
    u32 workgroupUintPtrType = 0;
    if (hasShared) {
        const u32 const16384 = spv.ConstantU32(16384);
        const u32 ldsArrayType = spv.TypeArray(typeU32, const16384);
        const u32 ldsPtrType = spv.TypePointer(Spv::StorageClass::Workgroup, ldsArrayType);
        ldsVar = spv.DeclareVariable(ldsPtrType, Spv::StorageClass::Workgroup);
        workgroupUintPtrType = spv.TypePointer(Spv::StorageClass::Workgroup, typeU32);
        spv.SetName(ldsVar, "g_lds");
        interfaceIds.push_back(ldsVar);
    }

    u32 ssboVar = 0;
    u32 storageBufferVec4PtrType = 0;
    if (hasBuffer) {
        const u32 ssboArrayType = spv.TypeArray(typeVec4, spv.ConstantU32(65536));
        const u32 stride16 = 16;
        spv.Decorate(ssboArrayType, Spv::Decoration::ArrayStride, std::span(&stride16, 1));

        const u32 ssboStructType = spv.TypeStruct(std::span(&ssboArrayType, 1));
        spv.Decorate(ssboStructType, Spv::Decoration::Block);
        const u32 offset0 = 0;
        spv.DecorateMember(ssboStructType, 0, Spv::Decoration::Offset, std::span(&offset0, 1));

        const u32 ssboPtrType = spv.TypePointer(Spv::StorageClass::StorageBuffer, ssboStructType);
        ssboVar = spv.DeclareVariable(ssboPtrType, Spv::StorageClass::StorageBuffer);
        storageBufferVec4PtrType = spv.TypePointer(Spv::StorageClass::StorageBuffer, typeVec4);
        spv.DecorateBinding(ssboVar, 0, 0);
        spv.SetName(ssboVar, "g_ssbo");
        interfaceIds.push_back(ssboVar);
    }

    u32 sampledImageVar = 0;
    u32 typeSampledImage2D = 0;
    if (hasImage) {
        const u32 typeImage2D = spv.TypeImage(typeFloat, 1, 0, 0, 0, 1, 0);
        typeSampledImage2D = spv.TypeSampledImage(typeImage2D);
        const u32 sampledImagePtrType = spv.TypePointer(Spv::StorageClass::UniformConstant, typeSampledImage2D);
        sampledImageVar = spv.DeclareVariable(sampledImagePtrType, Spv::StorageClass::UniformConstant);
        spv.DecorateBinding(sampledImageVar, 0, 1);
        spv.SetName(sampledImageVar, "g_textureSampler");
        interfaceIds.push_back(sampledImageVar);
    }

    const u32 entryPointId = spv.AllocId();

    if (ir.stage == ShaderStage::Vertex) {
        // Output: gl_Position (BuiltIn Position)
        const u32 outPtrType = spv.TypePointer(Spv::StorageClass::Output, typeVec4);
        outPosVar = spv.DeclareVariable(outPtrType, Spv::StorageClass::Output);
        spv.DecorateBuiltIn(outPosVar, Spv::BuiltIn::Position);
        interfaceIds.push_back(outPosVar);

        // Input: gl_VertexIndex (BuiltIn VertexIndex)
        const u32 inPtrType = spv.TypePointer(Spv::StorageClass::Input, typeU32);
        inVertexIndexVar = spv.DeclareVariable(inPtrType, Spv::StorageClass::Input);
        spv.DecorateBuiltIn(inVertexIndexVar, Spv::BuiltIn::VertexIndex);
        interfaceIds.push_back(inVertexIndexVar);

        spv.AddEntryPoint(Spv::ExecutionModel::Vertex, entryPointId, "main", interfaceIds);
    } else if (ir.stage == ShaderStage::Fragment) {
        // Output: outColor at Location 0
        const u32 outPtrType = spv.TypePointer(Spv::StorageClass::Output, typeVec4);
        outColorVar = spv.DeclareVariable(outPtrType, Spv::StorageClass::Output);
        spv.DecorateLocation(outColorVar, 0);
        interfaceIds.push_back(outColorVar);

        spv.AddEntryPoint(Spv::ExecutionModel::Fragment, entryPointId, "main", interfaceIds);
        spv.AddExecutionMode(entryPointId, Spv::ExecutionMode::OriginUpperLeft);
    } else if (ir.stage == ShaderStage::Compute) {
        const u32 typeVec3U32 = spv.TypeVector(typeU32, 3);
        const u32 inPtrType = spv.TypePointer(Spv::StorageClass::Input, typeVec3U32);
        inGlobalIdVar = spv.DeclareVariable(inPtrType, Spv::StorageClass::Input);
        spv.DecorateBuiltIn(inGlobalIdVar, Spv::BuiltIn::GlobalInvocationId);
        interfaceIds.push_back(inGlobalIdVar);

        spv.AddEntryPoint(Spv::ExecutionModel::GLCompute, entryPointId, "main", interfaceIds);
        const u32 localSizes[3] = {ir.workgroupSize[0], ir.workgroupSize[1], ir.workgroupSize[2]};
        spv.AddExecutionMode(entryPointId, Spv::ExecutionMode::LocalSize, localSizes);
    }

    // Function definition
    const u32 funcTypeId = spv.TypeFunction(typeVoid, {});
    spv.SetName(entryPointId, "main");

    // Map from IR value id to SPIR-V id
    std::unordered_map<u32, u32> valMap;
    std::unordered_map<u32, IrType> valTypeMap;

    auto GetSpvId = [&](const IrValue& val) -> u32 {
        if (val.isImmediate) {
            switch (val.type) {
                case IrType::U32: return spv.ConstantU32(val.immU32);
                case IrType::I32: return spv.ConstantU32(static_cast<u32>(val.immI32)); // Unified as typeU32
                case IrType::F32: return spv.ConstantF32(val.immF32);
                case IrType::Bool: return spv.ConstantBool(val.immBool);
                default: return spv.ConstantU32(0);
            }
        }
        auto it = valMap.find(val.id);
        if (it != valMap.end()) {
            return it->second;
        }
        if (val.type == IrType::F32) return spv.ConstantF32(0.0f);
        if (val.type == IrType::Bool) return spv.ConstantBool(false);
        return spv.ConstantU32(0);
    };

    auto GetSpvU32 = [&](const IrValue& val) -> u32 {
        if (val.isImmediate) {
            if (val.type == IrType::F32) {
                u32 u;
                std::memcpy(&u, &val.immF32, sizeof(u));
                return spv.ConstantU32(u);
            }
            return spv.ConstantU32(val.immU32);
        }
        u32 spvId = GetSpvId(val);
        IrType actualType = (valTypeMap.count(val.id) ? valTypeMap[val.id] : val.type);
        if (actualType == IrType::F32) {
            return spv.EmitUnary(Spv::OpBitcast, typeU32, spvId);
        }
        if (actualType == IrType::Bool) {
            return spv.EmitTernary(Spv::OpSelect, typeU32, spvId, spv.ConstantU32(1), spv.ConstantU32(0));
        }
        return spvId;
    };

    auto GetSpvF32 = [&](const IrValue& val) -> u32 {
        if (val.isImmediate) {
            if (val.type == IrType::F32) return spv.ConstantF32(val.immF32);
            float f;
            std::memcpy(&f, &val.immU32, sizeof(f));
            return spv.ConstantF32(f);
        }
        u32 spvId = GetSpvId(val);
        IrType actualType = (valTypeMap.count(val.id) ? valTypeMap[val.id] : val.type);
        if (actualType == IrType::U32 || actualType == IrType::I32) {
            return spv.EmitUnary(Spv::OpBitcast, typeFloat, spvId);
        }
        if (actualType == IrType::Bool) {
            return spv.EmitTernary(Spv::OpSelect, typeFloat, spvId, spv.ConstantF32(1.0f), spv.ConstantF32(0.0f));
        }
        return spvId;
    };

    // Begin Function
    const u32 funcId = spv.BeginFunction(typeVoid, funcTypeId, entryPointId);
    (void)funcId;

    std::unordered_map<u32, u32> blockLabelMap;
    for (const auto& block : ir.blocks) {
        blockLabelMap[block->id] = spv.AllocId();
    }

    for (size_t bIdx = 0; bIdx < ir.blocks.size(); ++bIdx) {
        const auto& block = ir.blocks[bIdx];
        u32 blockLabel = blockLabelMap[block->id];
        spv.BeginBlock(blockLabel);

        bool blockTerminated = false;
        for (const auto& inst : block->instructions) {
            if (blockTerminated) break;
            switch (inst.op) {
                case IrOp::Add: {
                    if (inst.resultType == IrType::F32) {
                        u32 a = GetSpvF32(inst.args[0]);
                        u32 b = GetSpvF32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpFAdd, typeFloat, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::F32;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpIAdd, typeU32, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::Sub: {
                    if (inst.resultType == IrType::F32) {
                        u32 a = GetSpvF32(inst.args[0]);
                        u32 b = GetSpvF32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpFSub, typeFloat, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::F32;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpISub, typeU32, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::Mul: {
                    if (inst.resultType == IrType::F32) {
                        u32 a = GetSpvF32(inst.args[0]);
                        u32 b = GetSpvF32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpFMul, typeFloat, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::F32;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpIMul, typeU32, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::Fma: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 b = GetSpvF32(inst.args[1]);
                    u32 c = GetSpvF32(inst.args[2]);
                    u32 mul = spv.EmitBinary(Spv::OpFMul, typeFloat, a, b);
                    u32 res = spv.EmitBinary(Spv::OpFAdd, typeFloat, mul, c);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Min: {
                    if (inst.resultType == IrType::F32) {
                        u32 a = GetSpvF32(inst.args[0]);
                        u32 b = GetSpvF32(inst.args[1]);
                        u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::FMin, std::array<u32, 2>{a, b});
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::F32;
                    } else if (inst.resultType == IrType::I32) {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitExtInst(typeU32, glslExt, Spv::GlslStd450::SMin, std::array<u32, 2>{a, b});
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::I32;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitExtInst(typeU32, glslExt, Spv::GlslStd450::UMin, std::array<u32, 2>{a, b});
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::Max: {
                    if (inst.resultType == IrType::F32) {
                        u32 a = GetSpvF32(inst.args[0]);
                        u32 b = GetSpvF32(inst.args[1]);
                        u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::FMax, std::array<u32, 2>{a, b});
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::F32;
                    } else if (inst.resultType == IrType::I32) {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitExtInst(typeU32, glslExt, Spv::GlslStd450::SMax, std::array<u32, 2>{a, b});
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::I32;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitExtInst(typeU32, glslExt, Spv::GlslStd450::UMax, std::array<u32, 2>{a, b});
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::Sqrt: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Sqrt, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Rsqrt: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::InverseSqrt, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Rcp: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 one = spv.ConstantF32(1.0f);
                    u32 res = spv.EmitBinary(Spv::OpFDiv, typeFloat, one, a);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Sin: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Sin, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Cos: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Cos, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Floor: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Floor, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Ceil: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Ceil, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Fract: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Fract, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Abs: {
                    if (inst.resultType == IrType::F32) {
                        u32 a = GetSpvF32(inst.args[0]);
                        u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::FAbs, std::span(&a, 1));
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::F32;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 res = spv.EmitExtInst(typeU32, glslExt, Spv::GlslStd450::SAbs, std::span(&a, 1));
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::I32;
                    }
                    break;
                }
                case IrOp::Exp2: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Exp2, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Log2: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Log2, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Trunc: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::Trunc, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::RoundEven: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitExtInst(typeFloat, glslExt, Spv::GlslStd450::RoundEven, std::span(&a, 1));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::Select: {
                    u32 cond = GetSpvId(inst.args[0]);
                    u32 resType = (inst.resultType == IrType::F32) ? typeFloat : ((inst.resultType == IrType::Bool) ? typeBool : typeU32);
                    u32 trueVal = (inst.resultType == IrType::F32) ? GetSpvF32(inst.args[1]) : ((inst.resultType == IrType::Bool) ? GetSpvId(inst.args[1]) : GetSpvU32(inst.args[1]));
                    u32 falseVal = (inst.resultType == IrType::F32) ? GetSpvF32(inst.args[2]) : ((inst.resultType == IrType::Bool) ? GetSpvId(inst.args[2]) : GetSpvU32(inst.args[2]));
                    u32 res = spv.EmitTernary(Spv::OpSelect, resType, cond, trueVal, falseVal);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = inst.resultType;
                    break;
                }
                case IrOp::CompareEq: {
                    bool isFloatCmp = (inst.args[0].type == IrType::F32);
                    u32 res = isFloatCmp
                        ? spv.EmitBinary(Spv::OpFOrdEqual, typeBool, GetSpvF32(inst.args[0]), GetSpvF32(inst.args[1]))
                        : spv.EmitBinary(Spv::OpIEqual, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1]));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Bool;
                    break;
                }
                case IrOp::CompareNeq: {
                    bool isFloatCmp = (inst.args[0].type == IrType::F32);
                    u32 res = isFloatCmp
                        ? spv.EmitBinary(Spv::OpFUnordNotEqual, typeBool, GetSpvF32(inst.args[0]), GetSpvF32(inst.args[1]))
                        : spv.EmitBinary(Spv::OpINotEqual, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1]));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Bool;
                    break;
                }
                case IrOp::CompareLt: {
                    bool isFloatCmp = (inst.args[0].type == IrType::F32);
                    bool isUnsigned = (inst.args[0].type == IrType::U32);
                    u32 res = isFloatCmp
                        ? spv.EmitBinary(Spv::OpFOrdLessThan, typeBool, GetSpvF32(inst.args[0]), GetSpvF32(inst.args[1]))
                        : (isUnsigned
                            ? spv.EmitBinary(Spv::OpULessThan, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1]))
                            : spv.EmitBinary(Spv::OpSLessThan, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1])));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Bool;
                    break;
                }
                case IrOp::CompareLte: {
                    bool isFloatCmp = (inst.args[0].type == IrType::F32);
                    bool isUnsigned = (inst.args[0].type == IrType::U32);
                    u32 res = isFloatCmp
                        ? spv.EmitBinary(Spv::OpFOrdLessThanEqual, typeBool, GetSpvF32(inst.args[0]), GetSpvF32(inst.args[1]))
                        : (isUnsigned
                            ? spv.EmitBinary(Spv::OpULessThanEqual, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1]))
                            : spv.EmitBinary(Spv::OpSLessThanEqual, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1])));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Bool;
                    break;
                }
                case IrOp::CompareGt: {
                    bool isFloatCmp = (inst.args[0].type == IrType::F32);
                    bool isUnsigned = (inst.args[0].type == IrType::U32);
                    u32 res = isFloatCmp
                        ? spv.EmitBinary(Spv::OpFOrdGreaterThan, typeBool, GetSpvF32(inst.args[0]), GetSpvF32(inst.args[1]))
                        : (isUnsigned
                            ? spv.EmitBinary(Spv::OpUGreaterThan, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1]))
                            : spv.EmitBinary(Spv::OpSGreaterThan, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1])));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Bool;
                    break;
                }
                case IrOp::CompareGte: {
                    bool isFloatCmp = (inst.args[0].type == IrType::F32);
                    bool isUnsigned = (inst.args[0].type == IrType::U32);
                    u32 res = isFloatCmp
                        ? spv.EmitBinary(Spv::OpFOrdGreaterThanEqual, typeBool, GetSpvF32(inst.args[0]), GetSpvF32(inst.args[1]))
                        : (isUnsigned
                            ? spv.EmitBinary(Spv::OpUGreaterThanEqual, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1]))
                            : spv.EmitBinary(Spv::OpSGreaterThanEqual, typeBool, GetSpvU32(inst.args[0]), GetSpvU32(inst.args[1])));
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Bool;
                    break;
                }
                case IrOp::ShiftLeft: {
                    u32 a = GetSpvU32(inst.args[0]);
                    u32 b = GetSpvU32(inst.args[1]);
                    u32 res = spv.EmitBinary(Spv::OpShiftLeftLogical, typeU32, a, b);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::ShiftRightLogical: {
                    u32 a = GetSpvU32(inst.args[0]);
                    u32 b = GetSpvU32(inst.args[1]);
                    u32 res = spv.EmitBinary(Spv::OpShiftRightLogical, typeU32, a, b);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::ShiftRightArithmetic: {
                    u32 a = GetSpvU32(inst.args[0]);
                    u32 b = GetSpvU32(inst.args[1]);
                    u32 res = spv.EmitBinary(Spv::OpShiftRightArithmetic, typeU32, a, b);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::BitAnd: {
                    if (inst.resultType == IrType::Bool) {
                        u32 a = GetSpvId(inst.args[0]);
                        u32 b = GetSpvId(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpLogicalAnd, typeBool, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::Bool;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpBitwiseAnd, typeU32, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::BitOr: {
                    if (inst.resultType == IrType::Bool) {
                        u32 a = GetSpvId(inst.args[0]);
                        u32 b = GetSpvId(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpLogicalOr, typeBool, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::Bool;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 b = GetSpvU32(inst.args[1]);
                        u32 res = spv.EmitBinary(Spv::OpBitwiseOr, typeU32, a, b);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::BitXor: {
                    u32 a = GetSpvU32(inst.args[0]);
                    u32 b = GetSpvU32(inst.args[1]);
                    u32 res = spv.EmitBinary(Spv::OpBitwiseXor, typeU32, a, b);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::BitNot: {
                    if (inst.resultType == IrType::Bool) {
                        u32 a = GetSpvId(inst.args[0]);
                        u32 res = spv.EmitUnary(Spv::OpLogicalNot, typeBool, a);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::Bool;
                    } else {
                        u32 a = GetSpvU32(inst.args[0]);
                        u32 res = spv.EmitUnary(Spv::OpNot, typeU32, a);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::ConvertI32ToF32: {
                    u32 a = GetSpvU32(inst.args[0]);
                    u32 res = spv.EmitUnary(Spv::OpConvertSToF, typeFloat, a);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::ConvertU32ToF32: {
                    u32 a = GetSpvU32(inst.args[0]);
                    u32 res = spv.EmitUnary(Spv::OpConvertUToF, typeFloat, a);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::ConvertF32ToI32: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitUnary(Spv::OpConvertFToS, typeU32, a);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::ConvertF32ToU32: {
                    u32 a = GetSpvF32(inst.args[0]);
                    u32 res = spv.EmitUnary(Spv::OpConvertFToU, typeU32, a);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::SharedLoad: {
                    u32 byteAddr = GetSpvU32(inst.args[0]);
                    u32 two = spv.ConstantU32(2);
                    u32 dwordIdx = spv.EmitBinary(Spv::OpShiftRightLogical, typeU32, byteAddr, two);
                    u32 elemPtr = spv.EmitAccessChain(workgroupUintPtrType, ldsVar, std::span(&dwordIdx, 1));
                    u32 res = spv.EmitLoad(typeU32, elemPtr);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::SharedStore: {
                    u32 byteAddr = GetSpvU32(inst.args[0]);
                    u32 val = GetSpvU32(inst.args[1]);
                    u32 two = spv.ConstantU32(2);
                    u32 dwordIdx = spv.EmitBinary(Spv::OpShiftRightLogical, typeU32, byteAddr, two);
                    u32 elemPtr = spv.EmitAccessChain(workgroupUintPtrType, ldsVar, std::span(&dwordIdx, 1));
                    spv.EmitStore(elemPtr, val);
                    break;
                }
                case IrOp::SharedAtomicAdd: {
                    u32 byteAddr = GetSpvU32(inst.args[0]);
                    u32 val = GetSpvU32(inst.args[1]);
                    u32 two = spv.ConstantU32(2);
                    u32 dwordIdx = spv.EmitBinary(Spv::OpShiftRightLogical, typeU32, byteAddr, two);
                    u32 elemPtr = spv.EmitAccessChain(workgroupUintPtrType, ldsVar, std::span(&dwordIdx, 1));
                    u32 scopeId = spv.ConstantU32(2 /* Workgroup */);
                    u32 semId = spv.ConstantU32(0x100 /* WorkgroupMemory */);
                    u32 res = spv.EmitAtomicIAdd(typeU32, elemPtr, scopeId, semId, val);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::BufferLoad: {
                    u32 byteAddr = GetSpvU32(inst.args[0]);
                    u32 four = spv.ConstantU32(4);
                    u32 vecIdx = spv.EmitBinary(Spv::OpShiftRightLogical, typeU32, byteAddr, four);
                    u32 zero = spv.ConstantU32(0);
                    std::array<u32, 2> indices = { zero, vecIdx };
                    u32 elemPtr = spv.EmitAccessChain(storageBufferVec4PtrType, ssboVar, indices);
                    u32 res = spv.EmitLoad(typeVec4, elemPtr);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Vec4;
                    break;
                }
                case IrOp::BufferStore: {
                    u32 byteAddr = GetSpvU32(inst.args[0]);
                    u32 val = 0;
                    if (inst.args[1].type == IrType::Vec4) {
                        val = GetSpvId(inst.args[1]);
                    } else {
                        u32 fval = GetSpvF32(inst.args[1]);
                        val = spv.EmitCompositeConstruct(typeVec4, std::array<u32, 4>{fval, fval, fval, fval});
                    }
                    u32 four = spv.ConstantU32(4);
                    u32 vecIdx = spv.EmitBinary(Spv::OpShiftRightLogical, typeU32, byteAddr, four);
                    u32 zero = spv.ConstantU32(0);
                    std::array<u32, 2> indices = { zero, vecIdx };
                    u32 elemPtr = spv.EmitAccessChain(storageBufferVec4PtrType, ssboVar, indices);
                    spv.EmitStore(elemPtr, val);
                    break;
                }
                case IrOp::CompositeConstruct: {
                    std::vector<u32> constituents;
                    for (const auto& arg : inst.args) {
                        constituents.push_back(GetSpvF32(arg));
                    }
                    u32 res = spv.EmitCompositeConstruct(typeVec4, constituents);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::Vec4;
                    break;
                }
                case IrOp::CompositeExtract: {
                    u32 composite = GetSpvId(inst.args[0]);
                    u32 idx = inst.args[1].immU32;
                    u32 res = spv.EmitCompositeExtract(typeFloat, composite, idx);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::F32;
                    break;
                }
                case IrOp::LoadVertexIndex: {
                    u32 res = (inVertexIndexVar != 0)
                        ? spv.EmitLoad(typeU32, inVertexIndexVar)
                        : spv.ConstantU32(0);
                    valMap[inst.result.id] = res;
                    valTypeMap[inst.result.id] = IrType::U32;
                    break;
                }
                case IrOp::LoadGlobalInvocationId: {
                    if (inGlobalIdVar != 0) {
                        const u32 typeVec3U32 = spv.TypeVector(typeU32, 3);
                        u32 vec = spv.EmitLoad(typeVec3U32, inGlobalIdVar);
                        u32 comp = inst.args.empty() ? 0u : inst.args[0].immU32;
                        u32 res = spv.EmitCompositeExtract(typeU32, vec, comp);
                        valMap[inst.result.id] = res;
                        valTypeMap[inst.result.id] = IrType::U32;
                    } else {
                        valMap[inst.result.id] = spv.ConstantU32(0);
                        valTypeMap[inst.result.id] = IrType::U32;
                    }
                    break;
                }
                case IrOp::StoreOutput: {
                    u32 targetVar = (ir.stage == ShaderStage::Vertex) ? outPosVar : outColorVar;
                    if (targetVar != 0) {
                        u32 val = GetSpvId(inst.args[0]);
                        spv.EmitStore(targetVar, val);
                    }
                    break;
                }
                case IrOp::Barrier: {
                    spv.EmitControlBarrier(2 /* Workgroup */, 2 /* Workgroup */, 0x100 /* AcquireRelease | WorkgroupMemory */);
                    break;
                }
                case IrOp::Return: {
                    spv.EmitReturn();
                    blockTerminated = true;
                    break;
                }
                case IrOp::ImageSample:
                case IrOp::ImageLoad: {
                    if (sampledImageVar != 0) {
                        u32 sampledImg = spv.EmitLoad(typeSampledImage2D, sampledImageVar);
                        const u32 typeVec2 = spv.TypeVector(typeFloat, 2);
                        u32 coord = 0;
                        if (inst.args.empty()) {
                            coord = spv.ConstantComposite(typeVec2, std::array<u32, 2>{spv.ConstantF32(0.0f), spv.ConstantF32(0.0f)});
                        } else if (inst.args[0].type == IrType::Vec2) {
                            coord = GetSpvId(inst.args[0]);
                        } else {
                            u32 u = GetSpvF32(inst.args[0]);
                            u32 v = (inst.args.size() > 1) ? GetSpvF32(inst.args[1]) : u;
                            coord = spv.EmitCompositeConstruct(typeVec2, std::array<u32, 2>{u, v});
                        }
                        u32 sampledRes = spv.EmitImageSampleImplicitLod(typeVec4, sampledImg, coord);
                        valMap[inst.result.id] = sampledRes;
                        valTypeMap[inst.result.id] = IrType::Vec4;
                    } else {
                        std::array<u32, 4> elements = {
                            spv.ConstantF32(1.0f), spv.ConstantF32(1.0f),
                            spv.ConstantF32(1.0f), spv.ConstantF32(1.0f)
                        };
                        u32 resId = spv.ConstantComposite(typeVec4, elements);
                        valMap[inst.result.id] = resId;
                        valTypeMap[inst.result.id] = IrType::Vec4;
                    }
                    break;
                }
                case IrOp::ImageStore: {
                    break;
                }
                case IrOp::Branch: {
                    u32 targetLabel = 0;
                    if (inst.targetBlockId != 0 && blockLabelMap.count(inst.targetBlockId)) {
                        targetLabel = blockLabelMap[inst.targetBlockId];
                    }
                    if (targetLabel != 0) {
                        spv.EmitBranch(targetLabel);
                    } else {
                        spv.EmitReturn();
                    }
                    blockTerminated = true;
                    break;
                }
                case IrOp::BranchConditional: {
                    u32 condId = 0;
                    if (!inst.args.empty()) {
                        condId = GetSpvId(inst.args[0]);
                        IrType condType = valTypeMap.count(inst.args[0].id) ? valTypeMap[inst.args[0].id] : inst.args[0].type;
                        if (condType != IrType::Bool) {
                            condId = spv.EmitBinary(Spv::OpINotEqual, typeBool, condId, spv.ConstantU32(0));
                        }
                    } else {
                        condId = spv.ConstantBool(true);
                    }

                    u32 trueLabel = (inst.targetBlockId != 0 && blockLabelMap.count(inst.targetBlockId))
                        ? blockLabelMap[inst.targetBlockId] : 0;
                    u32 falseLabel = (inst.falseBlockId != 0 && blockLabelMap.count(inst.falseBlockId))
                        ? blockLabelMap[inst.falseBlockId] : 0;

                    if (trueLabel != 0 && falseLabel != 0) {
                        spv.EmitBranchConditional(condId, trueLabel, falseLabel);
                    } else if (trueLabel != 0) {
                        spv.EmitBranch(trueLabel);
                    } else {
                        spv.EmitReturn();
                    }
                    blockTerminated = true;
                    break;
                }
                default:
                    break;
            }
        }

        if (!blockTerminated) {
            if (bIdx + 1 < ir.blocks.size()) {
                u32 nextLabel = blockLabelMap[ir.blocks[bIdx + 1]->id];
                spv.EmitBranch(nextLabel);
            } else {
                if (ir.stage == ShaderStage::Fragment && outColorVar != 0) {
                    const u32 c0 = spv.ConstantF32(1.0f);
                    const u32 c1 = spv.ConstantF32(0.0f);
                    const u32 c2 = spv.ConstantF32(1.0f);
                    const u32 c3 = spv.ConstantF32(1.0f);
                    const u32 defaultColor = spv.ConstantComposite(typeVec4, std::array<u32, 4>{c0, c1, c2, c3});
                    spv.EmitStore(outColorVar, defaultColor);
                }
                spv.EmitReturn();
            }
        }
    }

    spv.EndFunction();
    return spv.Assemble();
}

RecompileResult ShaderRecompiler::Recompile(const ShaderBinary& binary) {
    RecompileResult result;
    result.shaderHash = ShaderCache::ComputeHash(binary.code, binary.stage);

    // 1. Check Cache
    if (m_cache->Lookup(result.shaderHash, result.spirv)) {
        result.success = true;
        return result;
    }

    // 2. Try Fast IR Translation Path first
    bool fastSuccess = false;
    try {
        std::vector<RdnaInstruction> instructions = m_decoder.DecodeProgram(binary.code);
        result.decodedInstructions = instructions.size();

        IrProgram ir;
        ir.stage = binary.stage;
        ir.workgroupSize[0] = binary.workgroupSizeX;
        ir.workgroupSize[1] = binary.workgroupSizeY;
        ir.workgroupSize[2] = binary.workgroupSizeZ;

        std::unordered_set<u32> targetPcs;
        for (const auto& inst : instructions) {
            if (inst.isBranch && inst.branchTarget != 0) {
                targetPcs.insert(inst.branchTarget);
            }
        }

        std::unordered_map<u32, IrBlock*> pcToBlock;
        IrBlock* entryBlock = ir.CreateBlock("entry");
        ir.SetCurrentBlock(entryBlock);
        if (!instructions.empty()) {
            pcToBlock[instructions[0].pc] = entryBlock;
        }

        for (size_t i = 0; i < instructions.size(); ++i) {
            const auto& inst = instructions[i];

            if (targetPcs.count(inst.pc) && ir.currentBlock != nullptr) {
                if (!ir.currentBlock->instructions.empty()) {
                    bool endsWithTerminator = false;
                    IrOp lastOp = ir.currentBlock->instructions.back().op;
                    if (lastOp == IrOp::Branch || lastOp == IrOp::BranchConditional || lastOp == IrOp::Return || lastOp == IrOp::Discard) {
                        endsWithTerminator = true;
                    }
                    IrBlock* nextBlock = ir.CreateBlock("bb_" + std::to_string(inst.pc));
                    pcToBlock[inst.pc] = nextBlock;
                    if (!endsWithTerminator) {
                        IrInst& fallthrough = ir.Emit(IrOp::Branch, IrType::Void, {IrValue::ImmU32(inst.pc)});
                        fallthrough.targetBlockId = nextBlock->id;
                    }
                    ir.SetCurrentBlock(nextBlock);
                } else {
                    pcToBlock[inst.pc] = ir.currentBlock;
                }
            }

            TranslateInstruction(inst, ir);

            if (inst.isBranch && (i + 1 < instructions.size())) {
                u32 nextPc = instructions[i + 1].pc;
                IrBlock* nextBlock = ir.CreateBlock("bb_" + std::to_string(nextPc));
                pcToBlock[nextPc] = nextBlock;
                ir.SetCurrentBlock(nextBlock);
            }
        }

        // Resolve targetBlockId and falseBlockId for all Branch and BranchConditional instructions
        for (size_t b = 0; b < ir.blocks.size(); ++b) {
            auto& blk = ir.blocks[b];
            for (auto& inst : blk->instructions) {
                if (inst.op == IrOp::Branch) {
                    if (inst.targetBlockId == 0 && !inst.args.empty()) {
                        auto it = pcToBlock.find(inst.args[0].immU32);
                        if (it != pcToBlock.end()) {
                            inst.targetBlockId = it->second->id;
                        }
                    }
                } else if (inst.op == IrOp::BranchConditional) {
                    if (inst.targetBlockId == 0 && inst.args.size() > 1) {
                        auto itTrue = pcToBlock.find(inst.args[1].immU32);
                        if (itTrue != pcToBlock.end()) {
                            inst.targetBlockId = itTrue->second->id;
                        }
                    }
                    if (inst.falseBlockId == 0) {
                        if (inst.args.size() > 2) {
                            auto itFalse = pcToBlock.find(inst.args[2].immU32);
                            if (itFalse != pcToBlock.end()) {
                                inst.falseBlockId = itFalse->second->id;
                            }
                        }
                        if (inst.falseBlockId == 0 && b + 1 < ir.blocks.size()) {
                            inst.falseBlockId = ir.blocks[b + 1]->id;
                        }
                    }
                }
            }
        }

        result.spirv = EmitSpirvFromIr(ir);
        if (!result.spirv.empty() && result.spirv[0] == Spv::MagicNumber) {
            fastSuccess = true;
            result.success = true;
            m_cache->Store(result.shaderHash, binary.stage, result.spirv);
            return result;
        }
    } catch (const std::exception& e) {
        result.errorMessage = e.what();
    }

    // 3. Fallback: Invoke Native RDNA 2 Full Recompiler Pipeline
    try {
        ::ShaderRecompiler::RecompileRequest req{};
        ::ShaderRecompiler::ShaderStage stage = ::ShaderRecompiler::ShaderStage::Compute;
        if (binary.stage == ShaderStage::Vertex) stage = ::ShaderRecompiler::ShaderStage::Vertex;
        else if (binary.stage == ShaderStage::Fragment) stage = ::ShaderRecompiler::ShaderStage::Fragment;
        else if (binary.stage == ShaderStage::Compute) stage = ::ShaderRecompiler::ShaderStage::Compute;

        req.shader.stage = stage;
        req.shader.codeAddress = binary.guestAddress ? binary.guestAddress : 0x100000ull;
        req.shader.code = std::span<const uint32_t>(binary.code.data(), binary.code.size());
        req.context.waveSize = 32;
        req.context.userDataBaseRegister = 0;

        if (stage == ::ShaderRecompiler::ShaderStage::Compute) {
            req.context.compute = ::ShaderRecompiler::ShaderComputeStageInfo{
                {binary.workgroupSizeX ? binary.workgroupSizeX : 64u,
                 binary.workgroupSizeY ? binary.workgroupSizeY : 1u,
                 binary.workgroupSizeZ ? binary.workgroupSizeZ : 1u},
                0u, {false, false, false}, false, 1u
            };
        } else if (stage == ::ShaderRecompiler::ShaderStage::Fragment) {
            req.context.pixel = ::ShaderRecompiler::ShaderPixelStageInfo{};
            req.context.pixel->wave32 = true;
            req.context.pixel->targetOutputMode[0] = 9u;
        } else if (stage == ::ShaderRecompiler::ShaderStage::Vertex) {
            req.context.vertex = ::ShaderRecompiler::ShaderVertexStageInfo{};
        }

        req.target.vulkanVersion = 0x00401000u;
        req.target.spirvVersion = 0x00010300u;
        req.target.subgroupSize = 32u;
        req.layout.pushConstantSizeBytes = 128;
        req.useCache = false;

        auto rdnaRes = ::ShaderRecompiler::Recompile(req);
        if (!rdnaRes.spirv.empty()) {
            result.spirv.assign(rdnaRes.spirv.begin(), rdnaRes.spirv.end());
            result.success = true;
            result.errorMessage.clear();
            m_cache->Store(result.shaderHash, binary.stage, result.spirv);
            return result;
        }
    } catch (const std::exception& ex) {
        if (result.errorMessage.empty()) {
            result.errorMessage = ex.what();
        }
    }

    return result;
}

} // namespace Sailor::Shader
