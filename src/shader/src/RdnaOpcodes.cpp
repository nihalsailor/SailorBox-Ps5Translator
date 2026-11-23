#include "shader/RdnaOpcodes.hpp"
#include "shader/ShaderTypes.hpp"
#include <format>
#include <sstream>

namespace Sailor::Shader {

const char* RdnaOpToString(RdnaOp op) {
    switch (op) {
        case RdnaOp::Invalid: return "invalid";
        case RdnaOp::S_MOV_B32: return "s_mov_b32";
        case RdnaOp::S_MOV_B64: return "s_mov_b64";
        case RdnaOp::S_CMOV_B32: return "s_cmov_b32";
        case RdnaOp::S_CMOV_B64: return "s_cmov_b64";
        case RdnaOp::S_NOT_B32: return "s_not_b32";
        case RdnaOp::S_NOT_B64: return "s_not_b64";
        case RdnaOp::S_WQM_B32: return "s_wqm_b32";
        case RdnaOp::S_WQM_B64: return "s_wqm_b64";
        case RdnaOp::S_BREV_B32: return "s_brev_b32";
        case RdnaOp::S_BREV_B64: return "s_brev_b64";
        case RdnaOp::S_BCNT0_I32_B32: return "s_bcnt0_i32_b32";
        case RdnaOp::S_BCNT1_I32_B32: return "s_bcnt1_i32_b32";
        case RdnaOp::S_FF0_I32_B32: return "s_ff0_i32_b32";
        case RdnaOp::S_FF1_I32_B32: return "s_ff1_i32_b32";
        case RdnaOp::S_FLBIT_I32_B32: return "s_flbit_i32_b32";
        case RdnaOp::S_SEXT_I32_I8: return "s_sext_i32_i8";
        case RdnaOp::S_SEXT_I32_I16: return "s_sext_i32_i16";
        case RdnaOp::S_BITSET0_B32: return "s_bitset0_b32";
        case RdnaOp::S_BITSET1_B32: return "s_bitset1_b32";
        case RdnaOp::S_GETPC_B64: return "s_getpc_b64";
        case RdnaOp::S_SETPC_B64: return "s_setpc_b64";
        case RdnaOp::S_SWAPPC_B64: return "s_swappc_b64";
        case RdnaOp::S_RFE_B64: return "s_rfe_b64";
        case RdnaOp::S_AND_SAVEEXEC_B64: return "s_and_saveexec_b64";
        case RdnaOp::S_OR_SAVEEXEC_B64: return "s_or_saveexec_b64";
        case RdnaOp::S_XOR_SAVEEXEC_B64: return "s_xor_saveexec_b64";
        case RdnaOp::S_ANDN2_SAVEEXEC_B64: return "s_andn2_saveexec_b64";
        case RdnaOp::S_ORN2_SAVEEXEC_B64: return "s_orn2_saveexec_b64";
        case RdnaOp::S_NAND_SAVEEXEC_B64: return "s_nand_saveexec_b64";
        case RdnaOp::S_NOR_SAVEEXEC_B64: return "s_nor_saveexec_b64";
        case RdnaOp::S_XNOR_SAVEEXEC_B64: return "s_xnor_saveexec_b64";
        case RdnaOp::S_ABS_I32: return "s_abs_i32";
        case RdnaOp::S_AND_SAVEEXEC_B32: return "s_and_saveexec_b32";
        case RdnaOp::S_OR_SAVEEXEC_B32: return "s_or_saveexec_b32";
        case RdnaOp::S_XOR_SAVEEXEC_B32: return "s_xor_saveexec_b32";

        case RdnaOp::S_ADD_U32: return "s_add_u32";
        case RdnaOp::S_SUB_U32: return "s_sub_u32";
        case RdnaOp::S_ADD_I32: return "s_add_i32";
        case RdnaOp::S_SUB_I32: return "s_sub_i32";
        case RdnaOp::S_ADDC_U32: return "s_addc_u32";
        case RdnaOp::S_SUBB_U32: return "s_subb_u32";
        case RdnaOp::S_MIN_I32: return "s_min_i32";
        case RdnaOp::S_MIN_U32: return "s_min_u32";
        case RdnaOp::S_MAX_I32: return "s_max_i32";
        case RdnaOp::S_MAX_U32: return "s_max_u32";
        case RdnaOp::S_CSELECT_B32: return "s_cselect_b32";
        case RdnaOp::S_CSELECT_B64: return "s_cselect_b64";
        case RdnaOp::S_AND_B32: return "s_and_b32";
        case RdnaOp::S_AND_B64: return "s_and_b64";
        case RdnaOp::S_OR_B32: return "s_or_b32";
        case RdnaOp::S_OR_B64: return "s_or_b64";
        case RdnaOp::S_XOR_B32: return "s_xor_b32";
        case RdnaOp::S_XOR_B64: return "s_xor_b64";
        case RdnaOp::S_ANDN2_B32: return "s_andn2_b32";
        case RdnaOp::S_ANDN2_B64: return "s_andn2_b64";
        case RdnaOp::S_ORN2_B32: return "s_orn2_b32";
        case RdnaOp::S_ORN2_B64: return "s_orn2_b64";
        case RdnaOp::S_NAND_B32: return "s_nand_b32";
        case RdnaOp::S_NOR_B32: return "s_nor_b32";
        case RdnaOp::S_XNOR_B32: return "s_xnor_b32";
        case RdnaOp::S_LSHL_B32: return "s_lshl_b32";
        case RdnaOp::S_LSHL_B64: return "s_lshl_b64";
        case RdnaOp::S_LSHR_B32: return "s_lshr_b32";
        case RdnaOp::S_ASHR_I32: return "s_ashr_i32";
        case RdnaOp::S_MUL_I32: return "s_mul_i32";
        case RdnaOp::S_BFE_U32: return "s_bfe_u32";
        case RdnaOp::S_BFE_I32: return "s_bfe_i32";

        case RdnaOp::S_MOVK_I32: return "s_movk_i32";
        case RdnaOp::S_ADDK_I32: return "s_addk_i32";
        case RdnaOp::S_MULK_I32: return "s_mulk_i32";

        case RdnaOp::S_CMP_EQ_I32: return "s_cmp_eq_i32";
        case RdnaOp::S_CMP_LG_I32: return "s_cmp_lg_i32";
        case RdnaOp::S_CMP_GT_I32: return "s_cmp_gt_i32";
        case RdnaOp::S_CMP_GE_I32: return "s_cmp_ge_i32";
        case RdnaOp::S_CMP_LT_I32: return "s_cmp_lt_i32";
        case RdnaOp::S_CMP_LE_I32: return "s_cmp_le_i32";
        case RdnaOp::S_CMP_EQ_U32: return "s_cmp_eq_u32";
        case RdnaOp::S_CMP_LG_U32: return "s_cmp_lg_u32";
        case RdnaOp::S_CMP_GT_U32: return "s_cmp_gt_u32";
        case RdnaOp::S_CMP_GE_U32: return "s_cmp_ge_u32";
        case RdnaOp::S_CMP_LT_U32: return "s_cmp_lt_u32";
        case RdnaOp::S_CMP_LE_U32: return "s_cmp_le_u32";

        case RdnaOp::S_NOP: return "s_nop";
        case RdnaOp::S_ENDPGM: return "s_endpgm";
        case RdnaOp::S_BRANCH: return "s_branch";
        case RdnaOp::S_WAKEUP: return "s_wakeup";
        case RdnaOp::S_CBRANCH_SCC0: return "s_cbranch_scc0";
        case RdnaOp::S_CBRANCH_SCC1: return "s_cbranch_scc1";
        case RdnaOp::S_CBRANCH_VCCZ: return "s_cbranch_vccz";
        case RdnaOp::S_CBRANCH_VCCNZ: return "s_cbranch_vccnz";
        case RdnaOp::S_CBRANCH_EXECZ: return "s_cbranch_execz";
        case RdnaOp::S_CBRANCH_EXECNZ: return "s_cbranch_execnz";
        case RdnaOp::S_BARRIER: return "s_barrier";
        case RdnaOp::S_WAITCNT: return "s_waitcnt";
        case RdnaOp::S_SETHALT: return "s_sethalt";
        case RdnaOp::S_SLEEP: return "s_sleep";
        case RdnaOp::S_SETPRIO: return "s_setprio";
        case RdnaOp::S_SENDMSG: return "s_sendmsg";
        case RdnaOp::S_CODE_END: return "s_code_end";

        case RdnaOp::V_NOP: return "v_nop";
        case RdnaOp::V_MOV_B32: return "v_mov_b32";
        case RdnaOp::V_READFIRSTLANE_B32: return "v_readfirstlane_b32";
        case RdnaOp::V_CVT_F32_I32: return "v_cvt_f32_i32";
        case RdnaOp::V_CVT_F32_U32: return "v_cvt_f32_u32";
        case RdnaOp::V_CVT_U32_F32: return "v_cvt_u32_f32";
        case RdnaOp::V_CVT_I32_F32: return "v_cvt_i32_f32";
        case RdnaOp::V_CVT_F16_F32: return "v_cvt_f16_f32";
        case RdnaOp::V_CVT_F32_F16: return "v_cvt_f32_f16";
        case RdnaOp::V_FRACT_F32: return "v_fract_f32";
        case RdnaOp::V_TRUNC_F32: return "v_trunc_f32";
        case RdnaOp::V_CEIL_F32: return "v_ceil_f32";
        case RdnaOp::V_RNDNE_F32: return "v_rndne_f32";
        case RdnaOp::V_FLOOR_F32: return "v_floor_f32";
        case RdnaOp::V_EXP_F32: return "v_exp_f32";
        case RdnaOp::V_LOG_F32: return "v_log_f32";
        case RdnaOp::V_RCP_F32: return "v_rcp_f32";
        case RdnaOp::V_RSQ_F32: return "v_rsq_f32";
        case RdnaOp::V_SQRT_F32: return "v_sqrt_f32";
        case RdnaOp::V_SIN_F32: return "v_sin_f32";
        case RdnaOp::V_COS_F32: return "v_cos_f32";
        case RdnaOp::V_NOT_B32: return "v_not_b32";
        case RdnaOp::V_BFREV_B32: return "v_bfrev_b32";

        case RdnaOp::V_CNDMASK_B32: return "v_cndmask_b32";
        case RdnaOp::V_ADD_F32: return "v_add_f32";
        case RdnaOp::V_SUB_F32: return "v_sub_f32";
        case RdnaOp::V_SUBREV_F32: return "v_subrev_f32";
        case RdnaOp::V_MUL_F32: return "v_mul_f32";
        case RdnaOp::V_MIN_F32: return "v_min_f32";
        case RdnaOp::V_MAX_F32: return "v_max_f32";
        case RdnaOp::V_MIN_I32: return "v_min_i32";
        case RdnaOp::V_MAX_I32: return "v_max_i32";
        case RdnaOp::V_MIN_U32: return "v_min_u32";
        case RdnaOp::V_MAX_U32: return "v_max_u32";
        case RdnaOp::V_LSHRREV_B32: return "v_lshrrev_b32";
        case RdnaOp::V_ASHRREV_I32: return "v_ashrrev_i32";
        case RdnaOp::V_LSHLREV_B32: return "v_lshlrev_b32";
        case RdnaOp::V_AND_B32: return "v_and_b32";
        case RdnaOp::V_OR_B32: return "v_or_b32";
        case RdnaOp::V_XOR_B32: return "v_xor_b32";
        case RdnaOp::V_XNOR_B32: return "v_xnor_b32";
        case RdnaOp::V_ADD_NC_U32: return "v_add_nc_u32";
        case RdnaOp::V_SUB_NC_U32: return "v_sub_nc_u32";
        case RdnaOp::V_SUBREV_NC_U32: return "v_subrev_nc_u32";
        case RdnaOp::V_ADD_CO_CI_U32: return "v_add_co_ci_u32";
        case RdnaOp::V_SUB_CO_CI_U32: return "v_sub_co_ci_u32";
        case RdnaOp::V_SUBREV_CO_CI_U32: return "v_subrev_co_ci_u32";
        case RdnaOp::V_FMAC_F32: return "v_fmac_f32";

        case RdnaOp::V_MAD_F32: return "v_mad_f32";
        case RdnaOp::V_FMA_F32: return "v_fma_f32";
        case RdnaOp::V_MIN3_F32: return "v_min3_f32";
        case RdnaOp::V_MAX3_F32: return "v_max3_f32";
        case RdnaOp::V_MED3_F32: return "v_med3_f32";

        case RdnaOp::V_CMP_LT_F32: return "v_cmp_lt_f32";
        case RdnaOp::V_CMP_EQ_F32: return "v_cmp_eq_f32";
        case RdnaOp::V_CMP_LE_F32: return "v_cmp_le_f32";
        case RdnaOp::V_CMP_GT_F32: return "v_cmp_gt_f32";
        case RdnaOp::V_CMP_GE_F32: return "v_cmp_ge_f32";
        case RdnaOp::V_CMP_LG_F32: return "v_cmp_lg_f32";
        case RdnaOp::V_CMP_NEQ_F32: return "v_cmp_neq_f32";
        case RdnaOp::V_CMP_LT_I32: return "v_cmp_lt_i32";
        case RdnaOp::V_CMP_EQ_I32: return "v_cmp_eq_i32";
        case RdnaOp::V_CMP_LE_I32: return "v_cmp_le_i32";
        case RdnaOp::V_CMP_GT_I32: return "v_cmp_gt_i32";
        case RdnaOp::V_CMP_NE_I32: return "v_cmp_ne_i32";
        case RdnaOp::V_CMP_GE_I32: return "v_cmp_ge_i32";
        case RdnaOp::V_CMP_LT_U32: return "v_cmp_lt_u32";
        case RdnaOp::V_CMP_EQ_U32: return "v_cmp_eq_u32";
        case RdnaOp::V_CMP_LE_U32: return "v_cmp_le_u32";
        case RdnaOp::V_CMP_GT_U32: return "v_cmp_gt_u32";
        case RdnaOp::V_CMP_NE_U32: return "v_cmp_ne_u32";
        case RdnaOp::V_CMP_GE_U32: return "v_cmp_ge_u32";

        case RdnaOp::S_LOAD_DWORD: return "s_load_dword";
        case RdnaOp::S_LOAD_DWORDX2: return "s_load_dwordx2";
        case RdnaOp::S_LOAD_DWORDX4: return "s_load_dwordx4";
        case RdnaOp::S_LOAD_DWORDX8: return "s_load_dwordx8";
        case RdnaOp::S_LOAD_DWORDX16: return "s_load_dwordx16";
        case RdnaOp::S_BUFFER_LOAD_DWORD: return "s_buffer_load_dword";
        case RdnaOp::S_BUFFER_LOAD_DWORDX2: return "s_buffer_load_dwordx2";
        case RdnaOp::S_BUFFER_LOAD_DWORDX4: return "s_buffer_load_dwordx4";
        case RdnaOp::S_STORE_DWORD: return "s_store_dword";
        case RdnaOp::S_STORE_DWORDX2: return "s_store_dwordx2";
        case RdnaOp::S_STORE_DWORDX4: return "s_store_dwordx4";
        case RdnaOp::S_BUFFER_STORE_DWORD: return "s_buffer_store_dword";

        case RdnaOp::S_ATOMIC_ADD: return "s_atomic_add";
        case RdnaOp::S_ATOMIC_SUB: return "s_atomic_sub";
        case RdnaOp::S_BUFFER_ATOMIC_ADD: return "s_buffer_atomic_add";
        case RdnaOp::S_BUFFER_ATOMIC_SUB: return "s_buffer_atomic_sub";

        case RdnaOp::BUFFER_LOAD_FORMAT_X: return "buffer_load_format_x";
        case RdnaOp::BUFFER_LOAD_FORMAT_XY: return "buffer_load_format_xy";
        case RdnaOp::BUFFER_LOAD_FORMAT_XYZ: return "buffer_load_format_xyz";
        case RdnaOp::BUFFER_LOAD_FORMAT_XYZW: return "buffer_load_format_xyzw";
        case RdnaOp::BUFFER_STORE_FORMAT_XYZW: return "buffer_store_format_xyzw";
        case RdnaOp::BUFFER_LOAD_DWORD: return "buffer_load_dword";
        case RdnaOp::BUFFER_LOAD_DWORDX4: return "buffer_load_dwordx4";
        case RdnaOp::BUFFER_STORE_DWORD: return "buffer_store_dword";
        case RdnaOp::BUFFER_STORE_DWORDX4: return "buffer_store_dwordx4";

        case RdnaOp::IMAGE_SAMPLE: return "image_sample";
        case RdnaOp::IMAGE_SAMPLE_L: return "image_sample_l";
        case RdnaOp::IMAGE_LOAD: return "image_load";
        case RdnaOp::IMAGE_STORE: return "image_store";

        case RdnaOp::FLAT_LOAD_DWORD: return "flat_load_dword";
        case RdnaOp::FLAT_STORE_DWORD: return "flat_store_dword";
        case RdnaOp::GLOBAL_LOAD_DWORD: return "global_load_dword";
        case RdnaOp::GLOBAL_LOAD_DWORDX4: return "global_load_dwordx4";
        case RdnaOp::GLOBAL_STORE_DWORD: return "global_store_dword";
        case RdnaOp::GLOBAL_STORE_DWORDX4: return "global_store_dwordx4";
        case RdnaOp::SCRATCH_LOAD_DWORD: return "scratch_load_dword";
        case RdnaOp::SCRATCH_STORE_DWORD: return "scratch_store_dword";

        case RdnaOp::DS_ADD_U32: return "ds_add_u32";
        case RdnaOp::DS_SUB_U32: return "ds_sub_u32";
        case RdnaOp::DS_WRITE_B32: return "ds_write_b32";
        case RdnaOp::DS_READ_B32: return "ds_read_b32";
        case RdnaOp::DS_WRITE_B64: return "ds_write_b64";
        case RdnaOp::DS_READ_B64: return "ds_read_b64";
        case RdnaOp::DS_ADD_SRC2_U64: return "ds_add_src2_u64";
        case RdnaOp::DS_SUB_SRC2_U64: return "ds_sub_src2_u64";
        case RdnaOp::DS_AND_SRC2_B64: return "ds_and_src2_b64";
        case RdnaOp::DS_OR_SRC2_B64: return "ds_or_src2_b64";
        case RdnaOp::DS_XOR_SRC2_B64: return "ds_xor_src2_b64";
        case RdnaOp::DS_WRITE_SRC2_B64: return "ds_write_src2_b64";

        case RdnaOp::EXP: return "exp";
        default: return "unknown";
    }
}

std::string RdnaOperand::ToString() const {
    switch (kind) {
        case OperandKind::None: return "none";
        case OperandKind::ScalarRegister: return std::format("s{}", reg);
        case OperandKind::VectorRegister: return std::format("v{}", reg);
        case OperandKind::VccLo: return "vcc_lo";
        case OperandKind::VccHi: return "vcc_hi";
        case OperandKind::ExecLo: return "exec_lo";
        case OperandKind::ExecHi: return "exec_hi";
        case OperandKind::Scc: return "scc";
        case OperandKind::M0: return "m0";
        case OperandKind::Null: return "null";
        case OperandKind::LiteralConstant: return std::format("0x{:x}", value);
        case OperandKind::InlineIntConstant: return std::format("{}", signedVal);
        case OperandKind::InlineFloatConstant: return std::format("{:.4f}f", std::bit_cast<float>(value));
        case OperandKind::SrcSharedBase: return "src_shared_base";
        case OperandKind::SrcSharedLimit: return "src_shared_limit";
        case OperandKind::SrcPrivateBase: return "src_private_base";
        case OperandKind::SrcPrivateLimit: return "src_private_limit";
        default: return "unknown";
    }
}

std::string RdnaInstruction::Disassemble() const {
    std::string s = std::format("{:08x}: {:<20}", pc, opcodeName);
    if (dst.kind != OperandKind::None) {
        s += " " + dst.ToString();
    }
    for (u8 i = 0; i < sourceCount; ++i) {
        s += (i == 0 && dst.kind == OperandKind::None ? " " : ", ") + src[i].ToString();
    }
    if (isBranch) {
        s += std::format(" [target: 0x{:08x}]", branchTarget);
    }
    return s;
}

} // namespace Sailor::Shader
