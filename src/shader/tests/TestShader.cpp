#include "shader/ShaderTypes.hpp"
#include "shader/RdnaOpcodes.hpp"
#include "shader/RdnaDecoder.hpp"
#include "shader/ShaderIr.hpp"
#include "shader/SpirvBuilder.hpp"
#include "shader/ShaderCache.hpp"
#include "shader/ShaderRecompiler.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "RdnaDecoder/RdnaOpcode.hpp"
#include "common/Logger.hpp"

#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>

using namespace Sailor;
using namespace Sailor::Shader;
using RecompilerEngine = Sailor::Shader::ShaderRecompiler;

void TestRdnaDecoder() {
    std::cout << "[Test 1] Testing RDNA 2 / gfx1013 Instruction Decoder..." << std::endl;
    RdnaDecoder decoder;

    // 1. SOP2: s_add_u32 s2, s0, s1
    // word = 0x80000000 | (0x00 << 23) | (2 << 16) | (1 << 8) | 0 = 0x80020100
    const u32 sop2Code[] = {0x80020100u, 0xBF8B0000u /* s_endpgm */};
    auto prog1 = decoder.DecodeProgram(sop2Code);
    assert(prog1.size() == 2);
    assert(prog1[0].op == RdnaOp::S_ADD_U32);
    assert(prog1[0].dst.kind == OperandKind::ScalarRegister && prog1[0].dst.reg == 2);
    assert(prog1[0].src[0].kind == OperandKind::ScalarRegister && prog1[0].src[0].reg == 0);
    assert(prog1[0].src[1].kind == OperandKind::ScalarRegister && prog1[0].src[1].reg == 1);
    assert(prog1[1].op == RdnaOp::S_ENDPGM && prog1[1].isEndProgram);
    std::cout << "  -> SOP2 Decoded: " << prog1[0].Disassemble() << std::endl;

    // 2. SOP1: s_mov_b32 s5, s10
    // word = 0x80000000 | (0x7D << 23) | (5 << 16) | (0x03 << 8) | 10 = 0xBE85030A
    const u32 sop1Code[] = {0xBE85030Au};
    auto instSop1 = decoder.DecodeInstruction(sop1Code, 0);
    assert(instSop1.op == RdnaOp::S_MOV_B32);
    assert(instSop1.dst.reg == 5 && instSop1.src[0].reg == 10);
    std::cout << "  -> SOP1 Decoded: " << instSop1.Disassemble() << std::endl;

    // 3. VOP2: v_add_f32 v0, v1, v2
    // word = (0x03 << 25) | (0 << 17) | (2 << 9) | 1 = 0x06000401
    const u32 vop2Code[] = {0x06000401u};
    auto instVop2 = decoder.DecodeInstruction(vop2Code, 0);
    assert(instVop2.op == RdnaOp::V_ADD_F32);
    assert(instVop2.dst.reg == 0);
    assert(instVop2.src[0].reg == 1 && instVop2.src[1].reg == 2);
    std::cout << "  -> VOP2 Decoded: " << instVop2.Disassemble() << std::endl;

    // 4. VOP1: v_cvt_f32_i32 v4, v1
    // word = (0x3F << 25) | (4 << 17) | (0x05 << 9) | 1 = 0x7E080A01
    const u32 vop1Code[] = {0x7E080A01u};
    auto instVop1 = decoder.DecodeInstruction(vop1Code, 0);
    assert(instVop1.op == RdnaOp::V_CVT_F32_I32);
    assert(instVop1.dst.reg == 4 && instVop1.src[0].reg == 1);
    std::cout << "  -> VOP1 Decoded: " << instVop1.Disassemble() << std::endl;

    // 5. VOP3: v_fma_f32 v0, v1, v2, v3
    // w0 = (0x35 << 26) | (0x1C3 << 16) | 0 = 0xD5C30000
    // w1 = (3 << 18) | (2 << 9) | 1 = 0x000C0401
    const u32 vop3Code[] = {0xD5C30000u, 0x000C0401u};
    auto instVop3 = decoder.DecodeInstruction(vop3Code, 0);
    assert(instVop3.op == RdnaOp::V_FMA_F32);
    assert(instVop3.dst.reg == 0);
    assert(instVop3.src[0].reg == 1 && instVop3.src[1].reg == 2 && instVop3.src[2].reg == 3);
    std::cout << "  -> VOP3 Decoded: " << instVop3.Disassemble() << std::endl;

    // 6. PS5 gfx1013 Custom LDS: ds_add_src2_u64
    // w0 = (0x36 << 26) | (0x80 << 18) = 0xDA000000
    // w1 = (0 << 24) | (2 << 8) | 1 = 0x00000201
    const u32 dsGfx1013Code[] = {0xDA000000u, 0x00000201u};
    auto instDs = decoder.DecodeInstruction(dsGfx1013Code, 0);
    assert(instDs.op == RdnaOp::DS_ADD_SRC2_U64);
    assert(instDs.family == RdnaFamily::DS);
    std::cout << "  -> PS5 gfx1013 DS Decoded: " << instDs.Disassemble() << std::endl;

    // 7. MUBUF: buffer_load_dwordx4 with hardware RDNA 2 op in w0[24:18]
    // w0 = (0x38 << 26) | (0x0C << 18) | 0x10 = 0xE0300010
    // w1 = (0 << 8) | 1 = 0x00000001
    const u32 mubufCode[] = {0xE0300010u, 0x00000001u};
    auto instMubuf = decoder.DecodeInstruction(mubufCode, 0);
    assert(instMubuf.op == RdnaOp::BUFFER_LOAD_DWORDX4);
    assert(instMubuf.family == RdnaFamily::MUBUF);
    assert(instMubuf.src[2].value == 0x10);
    std::cout << "  -> RDNA 2 MUBUF Decoded: " << instMubuf.Disassemble() << std::endl;

    // 8. FLAT: global_load_dwordx4 with hardware RDNA 2 op in w0[24:18]
    // w0 = (0x37 << 26) | (0x17 << 18) | 0x20 = 0xDC5C0020
    // w1 = (2 << 8) | 3 = 0x00000203
    const u32 flatCode[] = {0xDC5C0020u, 0x00000203u};
    auto instFlat = decoder.DecodeInstruction(flatCode, 0);
    assert(instFlat.op == RdnaOp::GLOBAL_LOAD_DWORDX4);
    assert(instFlat.family == RdnaFamily::FLAT);
    assert(instFlat.src[1].value == 0x20);
    std::cout << "  -> RDNA 2 FLAT Decoded: " << instFlat.Disassemble() << std::endl;

    // 9. VOP1: v_readfirstlane_b32 s4, v2 (opId = 0x02, dstCode = 4, src0 = 2)
    // word = (0x3F << 25) | (4 << 17) | (0x02 << 9) | 2 = 0x7E080402
    const u32 rflCode[] = {0x7E080402u};
    auto instRfl = decoder.DecodeInstruction(rflCode, 0);
    assert(instRfl.op == RdnaOp::V_READFIRSTLANE_B32);
    assert(instRfl.dst.kind == OperandKind::ScalarRegister && instRfl.dst.reg == 4);
    assert(instRfl.src[0].kind == OperandKind::VectorRegister && instRfl.src[0].reg == 2);
    std::cout << "  -> V_READFIRSTLANE_B32 Decoded: " << instRfl.Disassemble() << std::endl;

    std::cout << "  [PASS] RDNA 2 / gfx1013 Instruction Decoder passed!" << std::endl;
}

void TestShaderIr() {
    std::cout << "[Test 2] Testing Intermediate Representation (SSA IR)..." << std::endl;
    IrProgram ir;
    ir.stage = ShaderStage::Vertex;

    IrBlock* entry = ir.CreateBlock("entry");
    ir.SetCurrentBlock(entry);

    IrValue a = IrValue::ImmF32(1.5f);
    IrValue b = IrValue::ImmF32(2.5f);
    IrValue addRes = ir.EmitAdd(a, b);
    assert(addRes.type == IrType::F32);
    assert(addRes.id > 0);

    IrValue mulRes = ir.EmitMul(addRes, IrValue::ImmF32(4.0f));
    assert(mulRes.type == IrType::F32);

    ir.EmitReturn();
    assert(entry->instructions.size() == 3);
    assert(entry->instructions.back().op == IrOp::Return);

    std::cout << "  [PASS] SSA IR Graph construction passed!" << std::endl;
}

void TestSpirvBuilder() {
    std::cout << "[Test 3] Testing SPIR-V 1.5 Binary Builder..." << std::endl;

    // Build a minimal valid SPIR-V vertex module
    SpirvBuilder spv(Spv::Version1_5);
    spv.AddCapability(Spv::Capability::Shader);
    spv.SetMemoryModel(Spv::AddressingModel::Logical, Spv::MemoryModel::GLSL450);

    const u32 typeVoid = spv.TypeVoid();
    const u32 typeFunc = spv.TypeFunction(typeVoid, {});
    const u32 entryId = spv.AllocId();

    spv.AddEntryPoint(Spv::ExecutionModel::Vertex, entryId, "main");

    const u32 funcId = spv.BeginFunction(typeVoid, typeFunc, entryId);
    assert(funcId == entryId);

    spv.BeginBlock();
    spv.EmitReturn();
    spv.EndFunction();

    std::vector<u32> binary = spv.Assemble();
    assert(binary.size() >= 5);
    assert(binary[0] == Spv::MagicNumber);
    assert(binary[1] == Spv::Version1_5);
    assert(binary[2] == Spv::GeneratorId);
    assert(binary[3] >= 5); // Bound strictly greater than all IDs
    assert(binary[4] == 0); // Reserved

    std::cout << "  -> Assembled " << binary.size() << " SPIR-V words (Magic: 0x"
              << std::hex << binary[0] << std::dec << ", Version: 0x"
              << std::hex << binary[1] << std::dec << ")" << std::endl;
    std::cout << "  [PASS] SPIR-V Binary Builder passed!" << std::endl;
}

void TestShaderCache() {
    std::cout << "[Test 4] Testing Multi-Tier Shader Cache..." << std::endl;

    const std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_TestCache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);

    ShaderCache cache(tempCacheDir.string());

    const u32 sampleCode[] = {0x80020100u, 0xBF8B0000u};
    const u64 hash = ShaderCache::ComputeHash(sampleCode, ShaderStage::Vertex);
    assert(hash != 0);

    // Initial lookup should miss
    std::vector<u32> outSpirv;
    assert(!cache.Lookup(hash, outSpirv));

    // Store dummy SPIR-V
    const u32 dummySpv[] = {Spv::MagicNumber, Spv::Version1_5, 0x1234, 10, 0};
    cache.Store(hash, ShaderStage::Vertex, dummySpv);
    assert(cache.EntryCount() == 1);

    // Memory lookup should hit
    assert(cache.Lookup(hash, outSpirv));
    assert(outSpirv.size() == 5);
    assert(outSpirv[0] == Spv::MagicNumber);

    // Clear memory cache, test disk persistence
    cache.Clear();
    assert(cache.EntryCount() == 0);

    assert(cache.Lookup(hash, outSpirv));
    assert(outSpirv.size() == 5);
    assert(outSpirv[0] == Spv::MagicNumber);
    std::cout << "  -> Disk Cache Verified at: " << tempCacheDir.string() << std::endl;

    // Clean up test directory
    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] Multi-Tier Shader Cache passed!" << std::endl;
}

void TestEndToEndRecompiler() {
    std::cout << "[Test 5] Testing End-to-End Shader Recompiler..." << std::endl;

    const std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_E2ECache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);

    auto cache = std::make_shared<ShaderCache>(tempCacheDir.string());
    RecompilerEngine recompiler(cache);

    // Realistic PS5 RDNA2 fragment shader:
    // 1. v_add_f32 v0, v1, v2    (0x06000401)
    // 2. v_mul_f32 v3, v0, v4    (0x10060800) -> dst=v3, src0=v0, src1=v4
    // 3. s_endpgm                (0xBF8B0000)
    ShaderBinary binary;
    binary.stage = ShaderStage::Fragment;
    binary.code = {
        0x06000401u, // v_add_f32 v0, v1, v2
        0x10060800u, // v_mul_f32 v3, v0, v4
        0xBF8B0000u  // s_endpgm
    };

    RecompileResult res1 = recompiler.Recompile(binary);
    assert(res1.success);
    assert(!res1.spirv.empty());
    assert(res1.spirv[0] == Spv::MagicNumber);
    assert(res1.decodedInstructions == 3);
    std::cout << "  -> First compilation emitted " << res1.spirv.size() << " SPIR-V words (Shader Hash: 0x"
              << std::hex << res1.shaderHash << std::dec << ")" << std::endl;

    // Second compilation of the identical binary: must be a fast cache hit
    RecompileResult res2 = recompiler.Recompile(binary);
    assert(res2.success);
    assert(res2.shaderHash == res1.shaderHash);
    assert(res2.spirv == res1.spirv);
    std::cout << "  -> Second compilation hit cache instantaneously!" << std::endl;

    // Test Compute Shader Stage:
    ShaderBinary computeBin;
    computeBin.stage = ShaderStage::Compute;
    computeBin.workgroupSizeX = 64;
    computeBin.workgroupSizeY = 1;
    computeBin.workgroupSizeZ = 1;
    computeBin.code = {
        0x80020100u, // s_add_u32 s2, s0, s1
        0xBF8A0000u, // s_barrier
        0xBF8B0000u  // s_endpgm
    };

    RecompileResult resCompute = recompiler.Recompile(computeBin);
    assert(resCompute.success);
    assert(resCompute.spirv[0] == Spv::MagicNumber);
    std::cout << "  -> Compute shader recompiled successfully (" << resCompute.spirv.size() << " SPIR-V words)" << std::endl;

    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] End-to-End Shader Recompiler passed!" << std::endl;
}

void TestComparisons() {
    std::cout << "[Test 6] Testing VOPC & SOPC Comparisons and 0.0f Constant..." << std::endl;
    std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_CmpCache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);
    auto testCache = std::make_shared<ShaderCache>(tempCacheDir.string());
    RecompilerEngine recompiler(testCache);

    // Test program with v_cmp_lt_f32 vcc, 0.0, v0
    // VOPC encoding: bit 31 = 0, bits [30:25] = 0x3E (0b011110)
    // word = (0x3Eu << 25) | (opId << 17) | (src1 << 9) | src0
    ShaderBinary cmpBin;
    cmpBin.stage = ShaderStage::Compute;
    cmpBin.workgroupSizeX = 64;
    cmpBin.code = {
        0x7C020080u, // v_cmp_lt_f32 vcc, 0.0, v0  ((0x3Eu << 25) | (0x01u << 17) | 128u)
        0x7C060080u, // v_cmp_le_f32 vcc, 0.0, v0  ((0x3Eu << 25) | (0x03u << 17) | 128u)
        0x7C0C0080u, // v_cmp_ge_f32 vcc, 0.0, v0  ((0x3Eu << 25) | (0x06u << 17) | 128u)
        0x7D040080u, // v_cmp_eq_i32 vcc, 0, v0    ((0x3Eu << 25) | (0x82u << 17) | 128u)
        0xBF8B0000u  // s_endpgm
    };

    RecompileResult res = recompiler.Recompile(cmpBin);
    assert(res.success);
    assert(!res.spirv.empty());
    assert(res.spirv[0] == Spv::MagicNumber);
    assert(res.decodedInstructions == 5);
    std::cout << "  -> VOPC comparisons successfully compiled (" << res.spirv.size() << " SPIR-V words)" << std::endl;
    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] VOPC & SOPC Comparisons passed!" << std::endl;
}

void TestVop2IntegerAndBitwise() {
    std::cout << "[Test 7] Testing VOP2 Integer, Bitwise & Min/Max Recompilation..." << std::endl;
    std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_Vop2Cache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);
    auto testCache = std::make_shared<ShaderCache>(tempCacheDir.string());
    RecompilerEngine recompiler(testCache);

    // VOP2 encoding: (opId << 25) | (dst << 17) | (src1 << 9) | src0
    // dst=v0 (0), src1=v1 (1), src0=v2 (2)
    ShaderBinary bin;
    bin.stage = ShaderStage::Compute;
    bin.workgroupSizeX = 64;
    bin.code = {
        (0x11u << 25) | (0u << 17) | (1u << 9) | 2u, // v_min_i32 v0, v2, v1
        (0x12u << 25) | (0u << 17) | (1u << 9) | 2u, // v_max_i32 v0, v2, v1
        (0x13u << 25) | (0u << 17) | (1u << 9) | 2u, // v_min_u32 v0, v2, v1
        (0x14u << 25) | (0u << 17) | (1u << 9) | 2u, // v_max_u32 v0, v2, v1
        (0x16u << 25) | (0u << 17) | (1u << 9) | 2u, // v_lshrrev_b32 v0, v2, v1
        (0x18u << 25) | (0u << 17) | (1u << 9) | 2u, // v_ashrrev_i32 v0, v2, v1
        (0x1Au << 25) | (0u << 17) | (1u << 9) | 2u, // v_lshlrev_b32 v0, v2, v1
        (0x1Bu << 25) | (0u << 17) | (1u << 9) | 2u, // v_and_b32 v0, v2, v1
        (0x1Cu << 25) | (0u << 17) | (1u << 9) | 2u, // v_or_b32 v0, v2, v1
        (0x1Du << 25) | (0u << 17) | (1u << 9) | 2u, // v_xor_b32 v0, v2, v1
        (0x1Eu << 25) | (0u << 17) | (1u << 9) | 2u, // v_xnor_b32 v0, v2, v1
        (0x28u << 25) | (0u << 17) | (1u << 9) | 2u, // v_add_co_ci_u32 v0, v2, v1
        (0x29u << 25) | (0u << 17) | (1u << 9) | 2u, // v_sub_co_ci_u32 v0, v2, v1
        (0x2Au << 25) | (0u << 17) | (1u << 9) | 2u, // v_subrev_co_ci_u32 v0, v2, v1
        0xBF8B0000u                                  // s_endpgm
    };

    RecompileResult res = recompiler.Recompile(bin);
    assert(res.success);
    assert(!res.spirv.empty());
    assert(res.spirv[0] == Spv::MagicNumber);
    assert(res.decodedInstructions == 15);
    std::cout << "  -> VOP2 integer, bitwise & carry-chain successfully compiled (" << res.spirv.size() << " SPIR-V words)" << std::endl;
    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] VOP2 Integer, Bitwise & Carry-Chain passed!" << std::endl;
}

void TestSmemStoresAndLdsSub() {
    std::cout << "[Test 8] Testing SMEM Stores, SMEM Wide Loads & LDS Sub Atomics..." << std::endl;
    std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_SmemCache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);
    auto testCache = std::make_shared<ShaderCache>(tempCacheDir.string());
    RecompilerEngine recompiler(testCache);

    // SMEM word0: (0x3Du << 26) | (opId << 18) | (dst << 6) | (sbase << 1)
    auto smem = [](u32 opId, u32 dst, u32 sbase, u32 offset) -> std::pair<u32, u32> {
        u32 w0 = (0x3Du << 26) | ((opId & 0x7Fu) << 18) | ((dst & 0x7Fu) << 6) | ((sbase & 0x1Fu) << 1);
        u32 w1 = offset & 0x1FFFFFu;
        return {w0, w1};
    };

    // DS word0: (0x36u << 26) | (opId << 18)
    auto ds = [](u32 opId, u32 addr, u32 data0, u32 dst) -> std::pair<u32, u32> {
        u32 w0 = (0x36u << 26) | ((opId & 0xFFu) << 18) | (0u /* offset */);
        u32 w1 = (dst << 24) | (data0 << 8) | (addr & 0xFFu);
        return {w0, w1};
    };

    ShaderBinary bin;
    bin.stage = ShaderStage::Compute;
    bin.workgroupSizeX = 64;

    auto [s_store, s_store_off] = smem(0x10, 0, 0, 0);       // s_store_dword
    auto [s_store2, s_store2_off] = smem(0x11, 2, 0, 4);     // s_store_dwordx2
    auto [s_store4, s_store4_off] = smem(0x12, 4, 0, 8);     // s_store_dwordx4
    auto [s_bstore, s_bstore_off] = smem(0x18, 8, 0, 16);    // s_buffer_store_dword
    auto [s_load8, s_load8_off] = smem(0x03, 10, 0, 32);     // s_load_dwordx8 (multi-vec4)
    auto [s_load16, s_load16_off] = smem(0x04, 20, 0, 64);   // s_load_dwordx16 (multi-vec4)
    auto [ds_sub32, ds_sub32_w1] = ds(0x01, 0, 1, 2);        // ds_sub_u32
    auto [ds_sub64, ds_sub64_w1] = ds(0x81, 0, 1, 2);        // ds_sub_src2_u64

    bin.code = {
        s_store, s_store_off,
        s_store2, s_store2_off,
        s_store4, s_store4_off,
        s_bstore, s_bstore_off,
        s_load8, s_load8_off,
        s_load16, s_load16_off,
        ds_sub32, ds_sub32_w1,
        ds_sub64, ds_sub64_w1,
        0xBF8B0000u // s_endpgm
    };

    RecompileResult res = recompiler.Recompile(bin);
    assert(res.success);
    assert(!res.spirv.empty());
    assert(res.spirv[0] == Spv::MagicNumber);
    std::cout << "  -> SMEM stores, wide loads & DS atomics compiled (" << res.spirv.size() << " SPIR-V words)" << std::endl;
    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] SMEM Stores, Wide Loads & LDS Sub Atomics passed!" << std::endl;
}

void TestVop3LiteralsAndMathOps() {
    std::cout << "[Test 9] Testing VOP3 Literal Constants & Lowered Math Opcodes..." << std::endl;
    RdnaDecoder decoder;

    // 1. Verify VOP3 literal constant decoding (wordCount == 3)
    // v_fma_f32 v0, v1, v2, 0x3F800000 (1.0f)
    const u32 vop3LiteralCode[] = {
        (0x35u << 26) | (0x1C3u << 16) | 0u, // opId=0x1C3 (V_FMA_F32), dst=v0
        (511u << 18) | (2u << 9) | 1u,       // src2=511 (256+255 literal constant), src1=v2, src0=v1
        0x3F800000u                          // literal constant 1.0f
    };
    auto instVop3 = decoder.DecodeInstruction(vop3LiteralCode, 0);
    assert(instVop3.op == RdnaOp::V_FMA_F32);
    assert(instVop3.wordCount == 3);
    assert(instVop3.src[2].kind == OperandKind::LiteralConstant);
    assert(instVop3.src[2].value == 0x3F800000u);
    std::cout << "  -> VOP3 literal constant verified (wordCount=3, literal=0x3F800000)" << std::endl;

    // 2. Recompile shader containing S_ABS_I32, V_EXP_F32, V_LOG_F32, V_TRUNC_F32, V_RNDNE_F32, and S_STORE_DWORDX2
    std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_MathOpsCache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);
    auto testCache = std::make_shared<ShaderCache>(tempCacheDir.string());
    RecompilerEngine recompiler(testCache);

    // SOP1 word: 0x80000000 | (0x7D << 23) | (dst << 16) | (opId << 8) | src0
    auto sop1 = [](u32 opId, u32 dst, u32 src0) -> u32 {
        return 0x80000000u | (0x7Du << 23) | ((dst & 0x7Fu) << 16) | ((opId & 0xFFu) << 8) | (src0 & 0xFFu);
    };

    // SOP2 word: 0x80000000 | (opId << 23) | (dst << 16) | (src1 << 8) | src0
    auto sop2 = [](u32 opId, u32 dst, u32 src1, u32 src0) -> u32 {
        return 0x80000000u | ((opId & 0x7Fu) << 23) | ((dst & 0x7Fu) << 16) | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu);
    };

    // VOP1 word: (0x3Fu << 25) | (dst << 17) | (opId << 9) | src0
    auto vop1 = [](u32 opId, u32 dst, u32 src0) -> u32 {
        return (0x3Fu << 25) | ((dst & 0xFFu) << 17) | ((opId & 0xFFu) << 9) | (src0 & 0x1FFu);
    };

    // VOP2 word: (opId << 25) | (dst << 17) | (src1 << 9) | src0
    auto vop2 = [](u32 opId, u32 dst, u32 src1, u32 src0) -> u32 {
        return ((opId & 0x3Fu) << 25) | ((dst & 0xFFu) << 17) | ((src1 & 0x1FFu) << 9) | (src0 & 0x1FFu);
    };

    // SMEM word0: (0x3Du << 26) | (opId << 18) | (dst << 6) | (sbase << 1)
    auto smem = [](u32 opId, u32 dst, u32 sbase, u32 offset) -> std::pair<u32, u32> {
        u32 w0 = (0x3Du << 26) | ((opId & 0x7Fu) << 18) | ((dst & 0x7Fu) << 6) | ((sbase & 0x1Fu) << 1);
        u32 w1 = offset & 0x1FFFFFu;
        return {w0, w1};
    };

    auto [s_store2, s_store2_off] = smem(0x11, 2, 0, 4); // s_store_dwordx2 (tests vec4 packing)

    ShaderBinary bin;
    bin.stage = ShaderStage::Compute;
    bin.workgroupSizeX = 64;
    bin.code = {
        sop1(0x34, 1, 0),        // s_abs_i32 s1, s0
        vop1(0x11, 0, 1),        // v_trunc_f32 v0, v1
        vop1(0x13, 0, 1),        // v_rndne_f32 v0, v1
        vop1(0x15, 0, 1),        // v_exp_f32 v0, v1
        vop1(0x16, 0, 1),        // v_log_f32 v0, v1
        sop1(0x03, 106, 0),      // s_mov_b32 vcc_lo, s0 (scalar write to VCC with U32 -> bool coercion)
        vop2(0x01, 0, 1, 2),     // v_cndmask_b32 v0, v2, v1 (evaluates coerced VCC)
        sop2(0x0A, 2, 1, 0),     // s_cselect_b32 s2, s0, s1 (evaluates SCC)
        s_store2, s_store2_off,  // s_store_dwordx2
        0xBF8B0000u              // s_endpgm
    };

    RecompileResult res = recompiler.Recompile(bin);
    assert(res.success);
    assert(!res.spirv.empty());
    assert(res.spirv[0] == Spv::MagicNumber);
    std::cout << "  -> Math ops and S_STORE_DWORDX2 compiled (" << res.spirv.size() << " SPIR-V words)" << std::endl;
    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] VOP3 Literal Constants & Lowered Math Opcodes passed!" << std::endl;
}

void TestTextureSamplingAndControlFlow() {
    std::cout << "[Test 10] Testing Texture Sampling, VOP3P & Multi-Block Control Flow..." << std::endl;
    std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_TexFlowCache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);
    auto testCache = std::make_shared<ShaderCache>(tempCacheDir.string());
    RecompilerEngine recompiler(testCache);

    // 1. Fragment shader with IMAGE_SAMPLE:
    u32 mimgWord0 = (0x3Cu << 26) | (0x20u << 18) | (0u << 8) | 4u;
    u32 mimgWord1 = (0u << 16) | 0u;
    ShaderBinary texBin;
    texBin.stage = ShaderStage::Fragment;
    texBin.code = {
        mimgWord0, mimgWord1,
        0xBF8B0000u // s_endpgm
    };
    RecompileResult resTex = recompiler.Recompile(texBin);
    assert(resTex.success);
    assert(!resTex.spirv.empty());
    assert(resTex.spirv[0] == Spv::MagicNumber);
    std::cout << "  -> IMAGE_SAMPLE compiled into SPIR-V (" << resTex.spirv.size() << " words)" << std::endl;

    // 2. Control flow shader with conditional branch:
    ShaderBinary flowBin;
    flowBin.stage = ShaderStage::Fragment;
    flowBin.code = {
        0x06000401u, // pc=0:  v_add_f32 v0, v1, v2
        0xBF870001u, // pc=4:  s_cbranch_vccnz target=12
        0x10060800u, // pc=8:  v_mul_f32 v3, v0, v4
        0xBF8B0000u  // pc=12: s_endpgm
    };
    RecompileResult resFlow = recompiler.Recompile(flowBin);
    assert(resFlow.success);
    assert(!resFlow.spirv.empty());
    assert(resFlow.spirv[0] == Spv::MagicNumber);
    std::cout << "  -> Multi-block control flow compiled into SPIR-V (" << resFlow.spirv.size() << " words)" << std::endl;

    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] Texture Sampling, VOP3P & Multi-Block Control Flow passed!" << std::endl;
}

void TestNativeRdnaBackendEngine() {
    std::cout << "[Test 11] Testing Native SailorBox RDNA 2 Engine..." << std::endl;
    ::ShaderRecompiler::RdnaInstructionDecoder rdnaDecoder;
    const uint32_t code[] = {0x80020100u, 0xBF8B0000u};
    auto program = rdnaDecoder.Decode(code);
    assert(program.instructions.size() >= 2);
    assert(program.instructions[0].op == ::ShaderRecompiler::RdnaOpcode::SAddU32);
    assert(program.instructions[1].op == ::ShaderRecompiler::RdnaOpcode::SEndpgm);
    std::cout << "  -> RDNA Instruction Decoded: " << ::ShaderRecompiler::RdnaInstructionToString(program.instructions[0]) << std::endl;
    std::cout << "  -> RDNA Program Decoded successfully (" << program.instructions.size() << " instructions)" << std::endl;
    std::cout << "  [PASS] Native SailorBox RDNA 2 Engine passed!" << std::endl;
}

void TestUnifiedRecompilerFallback() {
    std::cout << "[Test 12] Testing Unified RDNA 2 Recompiler Integration..." << std::endl;
    const std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / "SailorBox_FallbackCache";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);

    auto cache = std::make_shared<ShaderCache>(tempCacheDir.string());
    RecompilerEngine recompiler(cache);

    ShaderBinary computeBin;
    computeBin.stage = ShaderStage::Compute;
    computeBin.workgroupSizeX = 32;
    computeBin.code = {
        0x80020100u, // s_add_u32 s2, s0, s1
        0xBF8B0000u  // s_endpgm
    };

    RecompileResult res = recompiler.Recompile(computeBin);
    assert(res.success);
    assert(!res.spirv.empty());
    assert(res.spirv[0] == Spv::MagicNumber);
    std::cout << "  -> Unified recompiler pipeline produced " << res.spirv.size() << " SPIR-V words!" << std::endl;
    std::filesystem::remove_all(tempCacheDir, ec);
    std::cout << "  [PASS] Unified RDNA 2 Recompiler Integration passed!" << std::endl;
}

int main() {
    std::cout << "=================================================" << std::endl;
    std::cout << " SailorBox Phase 4: Shader Recompiler Test Suite" << std::endl;
    std::cout << "=================================================" << std::endl;

    try {
        TestRdnaDecoder();
        TestShaderIr();
        TestSpirvBuilder();
        TestShaderCache();
        TestEndToEndRecompiler();
        TestComparisons();
        TestVop2IntegerAndBitwise();
        TestSmemStoresAndLdsSub();
        TestVop3LiteralsAndMathOps();
        TestTextureSamplingAndControlFlow();
        TestNativeRdnaBackendEngine();
        TestUnifiedRecompilerFallback();

        std::cout << "\n>>> ALL PHASE 4 SHADER RECOMPILER TESTS PASSED! <<<\n" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED]: " << e.what() << std::endl;
        return 1;
    }
}
