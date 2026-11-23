#include "shader/SpirvBuilder.hpp"
#include <cstring>
#include <bit>

namespace Sailor::Shader {

SpirvBuilder::SpirvBuilder(u32 version)
    : m_version(version) {
}

u32 SpirvBuilder::AllocId() {
    return m_nextId++;
}

void SpirvBuilder::AppendInst(std::vector<u32>& section, Spv::Op op, std::span<const u32> operands) {
    const u32 wordCount = static_cast<u32>(1 + operands.size());
    const u32 head = (wordCount << 16) | (static_cast<u32>(op) & 0xFFFFu);
    section.push_back(head);
    section.insert(section.end(), operands.begin(), operands.end());
}

void SpirvBuilder::AppendString(std::vector<u32>& section, std::string_view str) {
    const size_t len = str.size();
    const size_t wordCount = (len + 4) / 4;
    size_t start = section.size();
    section.resize(start + wordCount, 0);
    std::memcpy(&section[start], str.data(), len);
}

void SpirvBuilder::AddCapability(Spv::Capability cap) {
    if (m_capabilitiesSet.insert(static_cast<u32>(cap)).second) {
        const u32 operand = static_cast<u32>(cap);
        AppendInst(m_capabilities, Spv::OpCapability, std::span(&operand, 1));
    }
}

void SpirvBuilder::AddExtension(std::string_view extName) {
    std::vector<u32> operands;
    AppendString(operands, extName);
    AppendInst(m_extensions, Spv::OpExtension, operands);
}

u32 SpirvBuilder::ImportExtInst(std::string_view name) {
    const u32 id = AllocId();
    std::vector<u32> operands;
    operands.push_back(id);
    AppendString(operands, name);
    AppendInst(m_extInstImports, Spv::OpExtInstImport, operands);
    return id;
}

void SpirvBuilder::SetMemoryModel(Spv::AddressingModel addr, Spv::MemoryModel mem) {
    m_memoryModel.clear();
    const u32 operands[2] = {static_cast<u32>(addr), static_cast<u32>(mem)};
    AppendInst(m_memoryModel, Spv::OpMemoryModel, operands);
}

void SpirvBuilder::AddEntryPoint(Spv::ExecutionModel model, u32 entryId, std::string_view name, std::span<const u32> interfaces) {
    std::vector<u32> operands;
    operands.push_back(static_cast<u32>(model));
    operands.push_back(entryId);
    AppendString(operands, name);
    operands.insert(operands.end(), interfaces.begin(), interfaces.end());
    AppendInst(m_entryPoints, Spv::OpEntryPoint, operands);
}

void SpirvBuilder::AddExecutionMode(u32 entryId, Spv::ExecutionMode mode, std::span<const u32> literals) {
    std::vector<u32> operands;
    operands.push_back(entryId);
    operands.push_back(static_cast<u32>(mode));
    operands.insert(operands.end(), literals.begin(), literals.end());
    AppendInst(m_executionModes, Spv::OpExecutionMode, operands);
}

void SpirvBuilder::SetName(u32 id, std::string_view name) {
    std::vector<u32> operands;
    operands.push_back(id);
    AppendString(operands, name);
    AppendInst(m_debug, Spv::OpName, operands);
}

void SpirvBuilder::Decorate(u32 targetId, Spv::Decoration decoration, std::span<const u32> operands) {
    std::vector<u32> ops;
    ops.push_back(targetId);
    ops.push_back(static_cast<u32>(decoration));
    ops.insert(ops.end(), operands.begin(), operands.end());
    AppendInst(m_annotations, Spv::OpDecorate, ops);
}

void SpirvBuilder::DecorateMember(u32 structId, u32 member, Spv::Decoration decoration, std::span<const u32> operands) {
    std::vector<u32> ops;
    ops.push_back(structId);
    ops.push_back(member);
    ops.push_back(static_cast<u32>(decoration));
    ops.insert(ops.end(), operands.begin(), operands.end());
    AppendInst(m_annotations, Spv::OpMemberDecorate, ops);
}

void SpirvBuilder::DecorateBuiltIn(u32 targetId, Spv::BuiltIn builtin) {
    const u32 operand = static_cast<u32>(builtin);
    Decorate(targetId, Spv::Decoration::BuiltIn, std::span(&operand, 1));
}

void SpirvBuilder::DecorateLocation(u32 targetId, u32 location) {
    Decorate(targetId, Spv::Decoration::Location, std::span(&location, 1));
}

void SpirvBuilder::DecorateBinding(u32 targetId, u32 set, u32 binding) {
    Decorate(targetId, Spv::Decoration::DescriptorSet, std::span(&set, 1));
    Decorate(targetId, Spv::Decoration::Binding, std::span(&binding, 1));
}

u32 SpirvBuilder::TypeVoid() {
    if (!m_typeVoid) {
        m_typeVoid = AllocId();
        const u32 op = m_typeVoid;
        AppendInst(m_typesAndGlobals, Spv::OpTypeVoid, std::span(&op, 1));
    }
    return m_typeVoid;
}

u32 SpirvBuilder::TypeBool() {
    if (!m_typeBool) {
        m_typeBool = AllocId();
        const u32 op = m_typeBool;
        AppendInst(m_typesAndGlobals, Spv::OpTypeBool, std::span(&op, 1));
    }
    return m_typeBool;
}

u32 SpirvBuilder::TypeInt(u32 width, bool isSigned) {
    if (width == 32) {
        if (isSigned) {
            if (!m_typeI32) {
                m_typeI32 = AllocId();
                const u32 ops[3] = {m_typeI32, 32, 1};
                AppendInst(m_typesAndGlobals, Spv::OpTypeInt, ops);
            }
            return m_typeI32;
        } else {
            if (!m_typeU32) {
                m_typeU32 = AllocId();
                const u32 ops[3] = {m_typeU32, 32, 0};
                AppendInst(m_typesAndGlobals, Spv::OpTypeInt, ops);
            }
            return m_typeU32;
        }
    }
    const u32 id = AllocId();
    const u32 ops[3] = {id, width, isSigned ? 1u : 0u};
    AppendInst(m_typesAndGlobals, Spv::OpTypeInt, ops);
    return id;
}

u32 SpirvBuilder::TypeFloat(u32 width) {
    if (width == 32) {
        if (!m_typeF32) {
            m_typeF32 = AllocId();
            const u32 ops[2] = {m_typeF32, 32};
            AppendInst(m_typesAndGlobals, Spv::OpTypeFloat, ops);
        }
        return m_typeF32;
    }
    const u32 id = AllocId();
    const u32 ops[2] = {id, width};
    AppendInst(m_typesAndGlobals, Spv::OpTypeFloat, ops);
    return id;
}

u32 SpirvBuilder::TypeVector(u32 componentTypeId, u32 count) {
    const u64 key = (static_cast<u64>(componentTypeId) << 32) | count;
    auto it = m_vecTypes.find(key);
    if (it != m_vecTypes.end()) {
        return it->second;
    }
    const u32 id = AllocId();
    const u32 ops[3] = {id, componentTypeId, count};
    AppendInst(m_typesAndGlobals, Spv::OpTypeVector, ops);
    m_vecTypes[key] = id;
    return id;
}

u32 SpirvBuilder::TypeArray(u32 elementTypeId, u32 lengthConstantId) {
    const u32 id = AllocId();
    const u32 ops[3] = {id, elementTypeId, lengthConstantId};
    AppendInst(m_typesAndGlobals, Spv::OpTypeArray, ops);
    return id;
}

u32 SpirvBuilder::TypeRuntimeArray(u32 elementTypeId) {
    const u32 id = AllocId();
    const u32 ops[2] = {id, elementTypeId};
    AppendInst(m_typesAndGlobals, Spv::OpTypeRuntimeArray, ops);
    return id;
}

u32 SpirvBuilder::TypeStruct(std::span<const u32> memberTypeIds) {
    const u32 id = AllocId();
    std::vector<u32> ops;
    ops.push_back(id);
    ops.insert(ops.end(), memberTypeIds.begin(), memberTypeIds.end());
    AppendInst(m_typesAndGlobals, Spv::OpTypeStruct, ops);
    return id;
}

u32 SpirvBuilder::TypeImage(u32 sampledTypeId, u32 dim, u32 depth, u32 arrayed, u32 ms, u32 sampled, u32 format) {
    const u32 id = AllocId();
    const u32 ops[8] = {id, sampledTypeId, dim, depth, arrayed, ms, sampled, format};
    AppendInst(m_typesAndGlobals, Spv::OpTypeImage, ops);
    return id;
}

u32 SpirvBuilder::TypeSampler() {
    const u32 id = AllocId();
    AppendInst(m_typesAndGlobals, Spv::OpTypeSampler, std::span(&id, 1));
    return id;
}

u32 SpirvBuilder::TypeSampledImage(u32 imageTypeId) {
    const u32 id = AllocId();
    const u32 ops[2] = {id, imageTypeId};
    AppendInst(m_typesAndGlobals, Spv::OpTypeSampledImage, ops);
    return id;
}

u32 SpirvBuilder::TypePointer(Spv::StorageClass storageClass, u32 typeId) {
    const u64 key = (static_cast<u64>(storageClass) << 32) | typeId;
    auto it = m_ptrTypes.find(key);
    if (it != m_ptrTypes.end()) {
        return it->second;
    }
    const u32 id = AllocId();
    const u32 ops[3] = {id, static_cast<u32>(storageClass), typeId};
    AppendInst(m_typesAndGlobals, Spv::OpTypePointer, ops);
    m_ptrTypes[key] = id;
    return id;
}

u32 SpirvBuilder::TypeFunction(u32 returnTypeId, std::span<const u32> paramTypes) {
    const u32 id = AllocId();
    std::vector<u32> ops;
    ops.push_back(id);
    ops.push_back(returnTypeId);
    ops.insert(ops.end(), paramTypes.begin(), paramTypes.end());
    AppendInst(m_typesAndGlobals, Spv::OpTypeFunction, ops);
    return id;
}

u32 SpirvBuilder::ConstantU32(u32 val) {
    auto it = m_u32Constants.find(val);
    if (it != m_u32Constants.end()) {
        return it->second;
    }
    const u32 typeId = TypeInt(32, false);
    const u32 id = AllocId();
    const u32 ops[3] = {typeId, id, val};
    AppendInst(m_typesAndGlobals, Spv::OpConstant, ops);
    m_u32Constants[val] = id;
    return id;
}

u32 SpirvBuilder::ConstantI32(i32 val) {
    auto it = m_i32Constants.find(val);
    if (it != m_i32Constants.end()) {
        return it->second;
    }
    const u32 typeId = TypeInt(32, true);
    const u32 id = AllocId();
    const u32 ops[3] = {typeId, id, static_cast<u32>(val)};
    AppendInst(m_typesAndGlobals, Spv::OpConstant, ops);
    m_i32Constants[val] = id;
    return id;
}

u32 SpirvBuilder::ConstantF32(f32 val) {
    const u32 raw = std::bit_cast<u32>(val);
    auto it = m_f32Constants.find(raw);
    if (it != m_f32Constants.end()) {
        return it->second;
    }
    const u32 typeId = TypeFloat(32);
    const u32 id = AllocId();
    const u32 ops[3] = {typeId, id, raw};
    AppendInst(m_typesAndGlobals, Spv::OpConstant, ops);
    m_f32Constants[raw] = id;
    return id;
}

u32 SpirvBuilder::ConstantBool(bool val) {
    if (val) {
        if (!m_constTrue) {
            m_constTrue = AllocId();
            const u32 ops[2] = {TypeBool(), m_constTrue};
            AppendInst(m_typesAndGlobals, Spv::OpConstantTrue, ops);
        }
        return m_constTrue;
    } else {
        if (!m_constFalse) {
            m_constFalse = AllocId();
            const u32 ops[2] = {TypeBool(), m_constFalse};
            AppendInst(m_typesAndGlobals, Spv::OpConstantFalse, ops);
        }
        return m_constFalse;
    }
}

u32 SpirvBuilder::ConstantComposite(u32 compositeTypeId, std::span<const u32> constituents) {
    const u32 id = AllocId();
    std::vector<u32> ops;
    ops.push_back(compositeTypeId);
    ops.push_back(id);
    ops.insert(ops.end(), constituents.begin(), constituents.end());
    AppendInst(m_typesAndGlobals, Spv::OpConstantComposite, ops);
    return id;
}

u32 SpirvBuilder::DeclareVariable(u32 ptrTypeId, Spv::StorageClass storageClass, u32 initializerId) {
    const u32 id = AllocId();
    std::vector<u32> ops;
    ops.push_back(ptrTypeId);
    ops.push_back(id);
    ops.push_back(static_cast<u32>(storageClass));
    if (initializerId != 0) {
        ops.push_back(initializerId);
    }
    AppendInst(m_typesAndGlobals, Spv::OpVariable, ops);
    return id;
}

u32 SpirvBuilder::BeginFunction(u32 returnTypeId, u32 funcTypeId, u32 funcId) {
    const u32 id = (funcId != 0) ? funcId : AllocId();
    const u32 ops[4] = {returnTypeId, id, 0u /* FunctionControlNone */, funcTypeId};
    AppendInst(m_functionInstructions, Spv::OpFunction, ops);
    return id;
}

void SpirvBuilder::EndFunction() {
    AppendInst(m_functionInstructions, Spv::OpFunctionEnd, {});
}

u32 SpirvBuilder::BeginBlock(u32 labelId) {
    const u32 id = (labelId != 0) ? labelId : AllocId();
    AppendInst(m_functionInstructions, Spv::OpLabel, std::span(&id, 1));
    return id;
}

u32 SpirvBuilder::EmitLoad(u32 resultTypeId, u32 pointerId) {
    const u32 id = AllocId();
    const u32 ops[3] = {resultTypeId, id, pointerId};
    AppendInst(m_functionInstructions, Spv::OpLoad, ops);
    return id;
}

void SpirvBuilder::EmitStore(u32 pointerId, u32 objectId) {
    const u32 ops[2] = {pointerId, objectId};
    AppendInst(m_functionInstructions, Spv::OpStore, ops);
}

u32 SpirvBuilder::EmitAccessChain(u32 resultPtrTypeId, u32 basePtrId, std::span<const u32> indexes) {
    const u32 id = AllocId();
    std::vector<u32> ops;
    ops.push_back(resultPtrTypeId);
    ops.push_back(id);
    ops.push_back(basePtrId);
    ops.insert(ops.end(), indexes.begin(), indexes.end());
    AppendInst(m_functionInstructions, Spv::OpAccessChain, ops);
    return id;
}

u32 SpirvBuilder::EmitCompositeConstruct(u32 resultTypeId, std::span<const u32> constituents) {
    const u32 id = AllocId();
    std::vector<u32> ops;
    ops.push_back(resultTypeId);
    ops.push_back(id);
    ops.insert(ops.end(), constituents.begin(), constituents.end());
    AppendInst(m_functionInstructions, Spv::OpCompositeConstruct, ops);
    return id;
}

u32 SpirvBuilder::EmitCompositeExtract(u32 resultTypeId, u32 compositeId, u32 index) {
    const u32 id = AllocId();
    const u32 ops[4] = {resultTypeId, id, compositeId, index};
    AppendInst(m_functionInstructions, Spv::OpCompositeExtract, ops);
    return id;
}

u32 SpirvBuilder::EmitUnary(Spv::Op op, u32 resultTypeId, u32 a) {
    const u32 id = AllocId();
    const u32 ops[3] = {resultTypeId, id, a};
    AppendInst(m_functionInstructions, op, ops);
    return id;
}

u32 SpirvBuilder::EmitBinary(Spv::Op op, u32 resultTypeId, u32 a, u32 b) {
    const u32 id = AllocId();
    const u32 ops[4] = {resultTypeId, id, a, b};
    AppendInst(m_functionInstructions, op, ops);
    return id;
}

u32 SpirvBuilder::EmitTernary(Spv::Op op, u32 resultTypeId, u32 a, u32 b, u32 c) {
    const u32 id = AllocId();
    const u32 ops[5] = {resultTypeId, id, a, b, c};
    AppendInst(m_functionInstructions, op, ops);
    return id;
}

u32 SpirvBuilder::EmitExtInst(u32 resultTypeId, u32 setImportId, u32 instruction, std::span<const u32> operands) {
    const u32 id = AllocId();
    std::vector<u32> ops;
    ops.reserve(4 + operands.size());
    ops.push_back(resultTypeId);
    ops.push_back(id);
    ops.push_back(setImportId);
    ops.push_back(instruction);
    ops.insert(ops.end(), operands.begin(), operands.end());
    AppendInst(m_functionInstructions, Spv::OpExtInst, ops);
    return id;
}

u32 SpirvBuilder::EmitAtomicIAdd(u32 resultTypeId, u32 ptrId, u32 scopeId, u32 semId, u32 valId, u32 resultId) {
    const u32 id = (resultId != 0) ? resultId : AllocId();
    const u32 ops[6] = {resultTypeId, id, ptrId, scopeId, semId, valId};
    AppendInst(m_functionInstructions, Spv::OpAtomicIAdd, ops);
    return id;
}

u32 SpirvBuilder::EmitSampledImage(u32 resultTypeId, u32 imageId, u32 samplerId) {
    const u32 id = AllocId();
    const u32 ops[4] = {resultTypeId, id, imageId, samplerId};
    AppendInst(m_functionInstructions, Spv::OpSampledImage, ops);
    return id;
}

u32 SpirvBuilder::EmitImageSampleImplicitLod(u32 resultTypeId, u32 sampledImageId, u32 coordinateId) {
    const u32 id = AllocId();
    const u32 ops[4] = {resultTypeId, id, sampledImageId, coordinateId};
    AppendInst(m_functionInstructions, Spv::OpImageSampleImplicitLod, ops);
    return id;
}

u32 SpirvBuilder::EmitImageFetch(u32 resultTypeId, u32 imageId, u32 coordinateId, u32 lodId) {
    const u32 id = AllocId();
    if (lodId != 0) {
        const u32 ops[6] = {resultTypeId, id, imageId, coordinateId, 2 /* Lod */, lodId};
        AppendInst(m_functionInstructions, Spv::OpImageFetch, ops);
    } else {
        const u32 ops[4] = {resultTypeId, id, imageId, coordinateId};
        AppendInst(m_functionInstructions, Spv::OpImageFetch, ops);
    }
    return id;
}

void SpirvBuilder::EmitImageWrite(u32 imageId, u32 coordinateId, u32 texelId) {
    const u32 ops[3] = {imageId, coordinateId, texelId};
    AppendInst(m_functionInstructions, Spv::OpImageWrite, ops);
}

void SpirvBuilder::EmitBranch(u32 targetLabelId) {
    AppendInst(m_functionInstructions, Spv::OpBranch, std::span(&targetLabelId, 1));
}

void SpirvBuilder::EmitBranchConditional(u32 condId, u32 trueLabelId, u32 falseLabelId) {
    const u32 ops[3] = {condId, trueLabelId, falseLabelId};
    AppendInst(m_functionInstructions, Spv::OpBranchConditional, ops);
}

void SpirvBuilder::EmitReturn() {
    AppendInst(m_functionInstructions, Spv::OpReturn, {});
}

void SpirvBuilder::EmitReturnValue(u32 valId) {
    AppendInst(m_functionInstructions, Spv::OpReturnValue, std::span(&valId, 1));
}

void SpirvBuilder::EmitKill() {
    AppendInst(m_functionInstructions, Spv::OpKill, {});
}

void SpirvBuilder::EmitControlBarrier(u32 executionScope, u32 memoryScope, u32 semantics) {
    const u32 scopeId = ConstantU32(executionScope);
    const u32 memScopeId = ConstantU32(memoryScope);
    const u32 semId = ConstantU32(semantics);
    const u32 ops[3] = {scopeId, memScopeId, semId};
    AppendInst(m_functionInstructions, Spv::OpControlBarrier, ops);
}

std::vector<u32> SpirvBuilder::Assemble() const {
    std::vector<u32> binary;
    // 5-word header
    binary.reserve(5 +
        m_capabilities.size() +
        m_extensions.size() +
        m_extInstImports.size() +
        m_memoryModel.size() +
        m_entryPoints.size() +
        m_executionModes.size() +
        m_debug.size() +
        m_annotations.size() +
        m_typesAndGlobals.size() +
        m_functionInstructions.size());

    binary.push_back(Spv::MagicNumber);
    binary.push_back(m_version);
    binary.push_back(Spv::GeneratorId);
    binary.push_back(m_nextId); // Bound
    binary.push_back(0u);        // Reserved

    // Append sections in strict SPIR-V specification order
    binary.insert(binary.end(), m_capabilities.begin(), m_capabilities.end());
    binary.insert(binary.end(), m_extensions.begin(), m_extensions.end());
    binary.insert(binary.end(), m_extInstImports.begin(), m_extInstImports.end());
    binary.insert(binary.end(), m_memoryModel.begin(), m_memoryModel.end());
    binary.insert(binary.end(), m_entryPoints.begin(), m_entryPoints.end());
    binary.insert(binary.end(), m_executionModes.begin(), m_executionModes.end());
    binary.insert(binary.end(), m_debug.begin(), m_debug.end());
    binary.insert(binary.end(), m_annotations.begin(), m_annotations.end());
    binary.insert(binary.end(), m_typesAndGlobals.begin(), m_typesAndGlobals.end());
    binary.insert(binary.end(), m_functionInstructions.begin(), m_functionInstructions.end());

    return binary;
}

} // namespace Sailor::Shader
