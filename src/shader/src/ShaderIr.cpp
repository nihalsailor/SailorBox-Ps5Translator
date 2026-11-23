#include "shader/ShaderIr.hpp"
#include <format>
#include <cassert>

namespace Sailor::Shader {

IrBlock* IrProgram::CreateBlock(std::string_view label) {
    auto block = std::make_unique<IrBlock>();
    block->id = nextBlockId++;
    block->label = label.empty() ? std::format("block_{}", block->id) : std::string(label);
    IrBlock* ptr = block.get();
    blocks.push_back(std::move(block));
    return ptr;
}

void IrProgram::SetCurrentBlock(IrBlock* block) {
    currentBlock = block;
}

IrValue IrProgram::AllocateValue(IrType type) {
    IrValue val;
    val.id = nextValueId++;
    val.type = type;
    val.isImmediate = false;
    val.defInst = nullptr;
    return val;
}

IrInst& IrProgram::Emit(IrOp op, IrType resultType, std::vector<IrValue> args) {
    if (!currentBlock) {
        currentBlock = CreateBlock("entry");
    }

    IrInst inst;
    inst.op = op;
    inst.resultType = resultType;
    inst.args = std::move(args);

    if (resultType != IrType::Void) {
        inst.result = AllocateValue(resultType);
        inst.result.defInst = &inst;
    }

    currentBlock->instructions.push_back(inst);
    return currentBlock->instructions.back();
}

IrValue IrProgram::EmitAdd(IrValue a, IrValue b) {
    IrInst& inst = Emit(IrOp::Add, a.type, {a, b});
    return inst.result;
}

IrValue IrProgram::EmitSub(IrValue a, IrValue b) {
    IrInst& inst = Emit(IrOp::Sub, a.type, {a, b});
    return inst.result;
}

IrValue IrProgram::EmitMul(IrValue a, IrValue b) {
    IrInst& inst = Emit(IrOp::Mul, a.type, {a, b});
    return inst.result;
}

IrValue IrProgram::EmitFma(IrValue a, IrValue b, IrValue c) {
    IrInst& inst = Emit(IrOp::Fma, a.type, {a, b, c});
    return inst.result;
}

IrValue IrProgram::EmitMin(IrValue a, IrValue b) {
    IrInst& inst = Emit(IrOp::Min, a.type, {a, b});
    return inst.result;
}

IrValue IrProgram::EmitMax(IrValue a, IrValue b) {
    IrInst& inst = Emit(IrOp::Max, a.type, {a, b});
    return inst.result;
}

void IrProgram::EmitReturn() {
    Emit(IrOp::Return, IrType::Void, {});
}

} // namespace Sailor::Shader
