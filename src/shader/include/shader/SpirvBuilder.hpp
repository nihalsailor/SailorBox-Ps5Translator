#pragma once

#include "SpirvOpcodes.hpp"
#include <vector>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Sailor::Shader {

class SpirvBuilder {
public:
    explicit SpirvBuilder(u32 version = Spv::Version1_5);

    // Allocates a fresh SPIR-V ID
    u32 AllocId();

    // Section 1: Capabilities
    void AddCapability(Spv::Capability cap);

    // Section 2: Extensions
    void AddExtension(std::string_view extName);

    // Section 3: Extended Instruction Set Import (e.g. GLSL.std.450)
    u32 ImportExtInst(std::string_view name);

    // Section 4: Memory Model
    void SetMemoryModel(Spv::AddressingModel addr, Spv::MemoryModel mem);

    // Section 5: Entry Points
    void AddEntryPoint(Spv::ExecutionModel model, u32 entryId, std::string_view name, std::span<const u32> interfaces = {});

    // Section 6: Execution Modes
    void AddExecutionMode(u32 entryId, Spv::ExecutionMode mode, std::span<const u32> literals = {});

    // Section 7: Debug Info
    void SetName(u32 id, std::string_view name);

    // Section 8: Annotations / Decorations
    void Decorate(u32 targetId, Spv::Decoration decoration, std::span<const u32> operands = {});
    void DecorateMember(u32 structId, u32 member, Spv::Decoration decoration, std::span<const u32> operands = {});
    void DecorateBuiltIn(u32 targetId, Spv::BuiltIn builtin);
    void DecorateLocation(u32 targetId, u32 location);
    void DecorateBinding(u32 targetId, u32 set, u32 binding);

    // Section 9: Types & Constants (cached)
    u32 TypeVoid();
    u32 TypeBool();
    u32 TypeInt(u32 width, bool isSigned);
    u32 TypeFloat(u32 width);
    u32 TypeVector(u32 componentTypeId, u32 count);
    u32 TypeArray(u32 elementTypeId, u32 lengthConstantId);
    u32 TypeRuntimeArray(u32 elementTypeId);
    u32 TypeStruct(std::span<const u32> memberTypeIds);
    u32 TypePointer(Spv::StorageClass storageClass, u32 typeId);
    u32 TypeFunction(u32 returnTypeId, std::span<const u32> paramTypes = {});
    u32 TypeImage(u32 sampledTypeId, u32 dim = 1, u32 depth = 0, u32 arrayed = 0, u32 ms = 0, u32 sampled = 1, u32 format = 0);
    u32 TypeSampler();
    u32 TypeSampledImage(u32 imageTypeId);

    u32 ConstantU32(u32 val);
    u32 ConstantI32(i32 val);
    u32 ConstantF32(f32 val);
    u32 ConstantBool(bool val);
    u32 ConstantComposite(u32 compositeTypeId, std::span<const u32> constituents);

    // Section 9: Variables
    u32 DeclareVariable(u32 ptrTypeId, Spv::StorageClass storageClass, u32 initializerId = 0);

    // Section 10: Functions & Blocks
    u32 BeginFunction(u32 returnTypeId, u32 funcTypeId, u32 funcId = 0);
    void EndFunction();
    u32 BeginBlock(u32 labelId = 0);

    // Function body instructions
    u32 EmitLoad(u32 resultTypeId, u32 pointerId);
    void EmitStore(u32 pointerId, u32 objectId);
    u32 EmitAccessChain(u32 resultPtrTypeId, u32 basePtrId, std::span<const u32> indexes);
    u32 EmitCompositeConstruct(u32 resultTypeId, std::span<const u32> constituents);
    u32 EmitCompositeExtract(u32 resultTypeId, u32 compositeId, u32 index);
    u32 EmitBinary(Spv::Op op, u32 resultTypeId, u32 a, u32 b);
    u32 EmitUnary(Spv::Op op, u32 resultTypeId, u32 a);
    u32 EmitTernary(Spv::Op op, u32 resultTypeId, u32 a, u32 b, u32 c);
    u32 EmitExtInst(u32 resultTypeId, u32 setImportId, u32 instruction, std::span<const u32> operands);
    u32 EmitAtomicIAdd(u32 resultTypeId, u32 ptrId, u32 scopeId, u32 semId, u32 valId, u32 resultId = 0);
    u32 EmitSampledImage(u32 resultTypeId, u32 imageId, u32 samplerId);
    u32 EmitImageSampleImplicitLod(u32 resultTypeId, u32 sampledImageId, u32 coordinateId);
    u32 EmitImageFetch(u32 resultTypeId, u32 imageId, u32 coordinateId, u32 lodId = 0);
    void EmitImageWrite(u32 imageId, u32 coordinateId, u32 texelId);
    void EmitBranch(u32 targetLabelId);
    void EmitBranchConditional(u32 condId, u32 trueLabelId, u32 falseLabelId);
    void EmitReturn();
    void EmitReturnValue(u32 valId);
    void EmitKill();
    void EmitControlBarrier(u32 executionScope, u32 memoryScope, u32 semantics);

    // Final assembly into ready-to-run binary SPIR-V module
    [[nodiscard]] std::vector<u32> Assemble() const;

private:
    void AppendInst(std::vector<u32>& section, Spv::Op op, std::span<const u32> operands);
    void AppendString(std::vector<u32>& section, std::string_view str);

    u32 m_version = Spv::Version1_5;
    u32 m_nextId = 1;

    // SPIR-V Logical Sections
    std::unordered_set<u32> m_capabilitiesSet;
    std::vector<u32> m_capabilities;
    std::vector<u32> m_extensions;
    std::vector<u32> m_extInstImports;
    std::vector<u32> m_memoryModel;
    std::vector<u32> m_entryPoints;
    std::vector<u32> m_executionModes;
    std::vector<u32> m_debug;
    std::vector<u32> m_annotations;
    std::vector<u32> m_typesAndGlobals;
    std::vector<u32> m_functionInstructions;

    // Type caches
    u32 m_typeVoid = 0;
    u32 m_typeBool = 0;
    u32 m_typeU32 = 0;
    u32 m_typeI32 = 0;
    u32 m_typeF32 = 0;
    std::unordered_map<u64, u32> m_vecTypes; // (componentTypeId << 32) | count
    std::unordered_map<u64, u32> m_ptrTypes; // (storageClass << 32) | typeId
    std::unordered_map<u32, u32> m_u32Constants;
    std::unordered_map<i32, u32> m_i32Constants;
    std::unordered_map<u32, u32> m_f32Constants;
    u32 m_constTrue = 0;
    u32 m_constFalse = 0;
};

} // namespace Sailor::Shader
