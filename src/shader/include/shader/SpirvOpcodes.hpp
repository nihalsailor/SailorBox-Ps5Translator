#pragma once

#include "common/Types.hpp"

namespace Sailor::Shader::Spv {

constexpr u32 MagicNumber = 0x07230203;
constexpr u32 Version1_0 = 0x00010000;
constexpr u32 Version1_3 = 0x00010300;
constexpr u32 Version1_5 = 0x00010500;
constexpr u32 GeneratorId = 0x00534201; // SailorBox generator ID

enum Op : u32 {
    OpNop = 0,
    OpSource = 3,
    OpName = 5,
    OpMemberName = 6,
    OpExtension = 10,
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstantTrue = 41,
    OpConstantFalse = 42,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpConstantNull = 46,
    OpFunction = 54,
    OpFunctionParameter = 55,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpSampledImage = 86,
    OpImageSampleImplicitLod = 87,
    OpImageSampleExplicitLod = 88,
    OpImageFetch = 95,
    OpImageWrite = 98,
    OpConvertFToU = 109,
    OpConvertFToS = 110,
    OpConvertSToF = 111,
    OpConvertUToF = 112,
    OpBitcast = 124,
    OpSNegate = 126,
    OpFNegate = 127,
    OpIAdd = 128,
    OpFAdd = 129,
    OpISub = 130,
    OpFSub = 131,
    OpIMul = 132,
    OpFMul = 133,
    OpUDiv = 134,
    OpSDiv = 135,
    OpFDiv = 136,
    OpFMod = 141,
    OpShiftRightLogical = 194,
    OpShiftRightArithmetic = 195,
    OpShiftLeftLogical = 196,
    OpBitwiseOr = 197,
    OpBitwiseXor = 198,
    OpBitwiseAnd = 199,
    OpNot = 200,
    OpLogicalEqual = 164,
    OpLogicalNotEqual = 165,
    OpLogicalOr = 166,
    OpLogicalAnd = 167,
    OpLogicalNot = 168,
    OpSelect = 169,
    OpIEqual = 170,
    OpINotEqual = 171,
    OpUGreaterThan = 172,
    OpSGreaterThan = 173,
    OpUGreaterThanEqual = 174,
    OpSGreaterThanEqual = 175,
    OpULessThan = 176,
    OpSLessThan = 177,
    OpULessThanEqual = 178,
    OpSLessThanEqual = 179,
    OpFOrdEqual = 180,
    OpFUnordNotEqual = 181,
    OpFOrdLessThan = 182,
    OpFOrdGreaterThan = 184,
    OpFOrdLessThanEqual = 186,
    OpFOrdGreaterThanEqual = 188,
    OpControlBarrier = 224,
    OpMemoryBarrier = 225,
    OpAtomicIAdd = 229,
    OpAtomicISub = 230,
    OpAtomicSMin = 231,
    OpAtomicUMin = 232,
    OpAtomicSMax = 233,
    OpAtomicUMax = 234,
    OpAtomicAnd = 235,
    OpAtomicOr = 236,
    OpAtomicXor = 237,
    OpLabel = 248,
    OpBranch = 249,
    OpBranchConditional = 250,
    OpKill = 252,
    OpReturn = 253,
    OpReturnValue = 254
};

namespace GlslStd450 {
    constexpr u32 Round = 1;
    constexpr u32 RoundEven = 2;
    constexpr u32 Trunc = 3;
    constexpr u32 FAbs = 4;
    constexpr u32 SAbs = 5;
    constexpr u32 Floor = 8;
    constexpr u32 Ceil = 9;
    constexpr u32 Fract = 10;
    constexpr u32 Sin = 13;
    constexpr u32 Cos = 14;
    constexpr u32 Exp2 = 29;
    constexpr u32 Log2 = 30;
    constexpr u32 Sqrt = 31;
    constexpr u32 InverseSqrt = 32;
    constexpr u32 FMin = 37;
    constexpr u32 UMin = 38;
    constexpr u32 SMin = 39;
    constexpr u32 FMax = 40;
    constexpr u32 UMax = 41;
    constexpr u32 SMax = 42;
}

enum class Capability : u32 {
    Matrix = 0,
    Shader = 1,
    Geometry = 2,
    Tessellation = 3,
    Addresses = 4,
    Linkage = 5,
    Kernel = 6,
    Vector16 = 7,
    Float16 = 9,
    Float64 = 10,
    Int64 = 11,
    Int16 = 22,
    ImageQuery = 34
};

enum class ExecutionModel : u32 {
    Vertex = 0,
    TessellationControl = 1,
    TessellationEvaluation = 2,
    Geometry = 3,
    Fragment = 4,
    GLCompute = 5
};

enum class ExecutionMode : u32 {
    OriginUpperLeft = 7,
    LocalSize = 17
};

enum class StorageClass : u32 {
    UniformConstant = 0,
    Input = 1,
    Uniform = 2,
    Output = 3,
    Workgroup = 4,
    Private = 6,
    Function = 7,
    StorageBuffer = 12
};

enum class Decoration : u32 {
    RelaxedPrecision = 0,
    Block = 2,
    BufferBlock = 3,
    RowMajor = 4,
    ColMajor = 5,
    ArrayStride = 6,
    MatrixStride = 7,
    BuiltIn = 11,
    Location = 30,
    Binding = 33,
    DescriptorSet = 34,
    Offset = 35
};

enum class BuiltIn : u32 {
    Position = 0,
    PointSize = 1,
    VertexIndex = 42,
    InstanceIndex = 43,
    FragCoord = 15,
    FrontFacing = 17,
    WorkgroupId = 24,
    LocalInvocationId = 27,
    GlobalInvocationId = 28
};

enum class AddressingModel : u32 {
    Logical = 0,
    Physical32 = 1,
    Physical64 = 2
};

enum class MemoryModel : u32 {
    Simple = 0,
    GLSL450 = 1,
    OpenCL = 2,
    Vulkan = 3
};

} // namespace Sailor::Shader::Spv
