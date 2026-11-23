#pragma once

#include "ShaderTypes.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace Sailor::Shader {

enum class IrType : u8 {
    Void = 0,
    Bool,
    U32,
    I32,
    F32,
    Vec2,
    Vec3,
    Vec4
};

inline const char* IrTypeToString(IrType type) {
    switch (type) {
        case IrType::Void: return "void";
        case IrType::Bool: return "bool";
        case IrType::U32: return "u32";
        case IrType::I32: return "i32";
        case IrType::F32: return "f32";
        case IrType::Vec2: return "vec2";
        case IrType::Vec3: return "vec3";
        case IrType::Vec4: return "vec4";
        default: return "unknown";
    }
}

enum class IrOp : u16 {
    Nop = 0,
    // Constants
    Constant,

    // Scalar & Vector ALU
    Add,
    Sub,
    Mul,
    Div,
    Neg,
    Abs,
    Fma,
    Min,
    Max,
    Clamp,

    // Math functions
    Sqrt,
    Rsqrt,
    Rcp,
    Exp2,
    Log2,
    Sin,
    Cos,
    Floor,
    Ceil,
    Fract,
    Trunc,
    RoundEven,

    // Bitwise
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    ShiftLeft,
    ShiftRightLogical,
    ShiftRightArithmetic,

    // Comparison & Selection
    CompareEq,
    CompareNeq,
    CompareLt,
    CompareLte,
    CompareGt,
    CompareGte,
    Select, // (condition, true_val, false_val)

    // Type Conversions
    ConvertI32ToF32,
    ConvertU32ToF32,
    ConvertF32ToI32,
    ConvertF32ToU32,

    // Composite operations
    CompositeConstruct,
    CompositeExtract,

    // Builtin Inputs / Outputs
    LoadVertexIndex,
    LoadInstanceIndex,
    LoadGlobalInvocationId,
    LoadLocalInvocationId,
    LoadWorkgroupId,
    LoadInput,
    StoreOutput,

    // Memory (Buffer, LDS, Image)
    BufferLoad,
    BufferStore,
    SharedLoad,
    SharedStore,
    SharedAtomicAdd,
    ImageSample,
    ImageLoad,
    ImageStore,

    // Control flow
    Branch,
    BranchConditional,
    Barrier,
    Return,
    Discard
};

struct IrInst;

struct IrValue {
    u32 id = 0;
    IrType type = IrType::Void;
    bool isImmediate = false;
    union {
        u32 immU32;
        i32 immI32;
        f32 immF32;
        bool immBool;
    };
    IrInst* defInst = nullptr;

    static IrValue ImmU32(u32 v, u32 id = 0) {
        IrValue val;
        val.id = id;
        val.type = IrType::U32;
        val.isImmediate = true;
        val.immU32 = v;
        return val;
    }

    static IrValue ImmF32(f32 v, u32 id = 0) {
        IrValue val;
        val.id = id;
        val.type = IrType::F32;
        val.isImmediate = true;
        val.immF32 = v;
        return val;
    }

    static IrValue ImmBool(bool v, u32 id = 0) {
        IrValue val;
        val.id = id;
        val.type = IrType::Bool;
        val.isImmediate = true;
        val.immBool = v;
        return val;
    }
};

struct IrInst {
    IrOp op = IrOp::Nop;
    IrType resultType = IrType::Void;
    IrValue result{};
    std::vector<IrValue> args;
    u32 targetBlockId = 0;
    u32 falseBlockId = 0;
    u32 slot = 0; // binding or location slot
};

struct IrBlock {
    u32 id = 0;
    std::string label;
    std::vector<IrInst> instructions;
};

class IrProgram {
public:
    ShaderStage stage = ShaderStage::Vertex;
    u32 workgroupSize[3] = {1, 1, 1};
    std::vector<std::unique_ptr<IrBlock>> blocks;
    u32 nextValueId = 1;
    u32 nextBlockId = 1;

    // Active block
    IrBlock* currentBlock = nullptr;

    // Register mapping: SGPR (0..105), VGPR (0..255) to latest SSA values
    std::unordered_map<u32, IrValue> sgprs;
    std::unordered_map<u32, IrValue> vgprs;
    IrValue vcc = IrValue::ImmBool(true);
    IrValue exec = IrValue::ImmBool(true);
    IrValue scc = IrValue::ImmBool(false);

    IrBlock* CreateBlock(std::string_view label = "");
    void SetCurrentBlock(IrBlock* block);

    IrValue AllocateValue(IrType type);
    IrInst& Emit(IrOp op, IrType resultType, std::vector<IrValue> args = {});

    // High level helpers
    IrValue EmitAdd(IrValue a, IrValue b);
    IrValue EmitSub(IrValue a, IrValue b);
    IrValue EmitMul(IrValue a, IrValue b);
    IrValue EmitFma(IrValue a, IrValue b, IrValue c);
    IrValue EmitMin(IrValue a, IrValue b);
    IrValue EmitMax(IrValue a, IrValue b);
    void EmitReturn();
};

} // namespace Sailor::Shader
