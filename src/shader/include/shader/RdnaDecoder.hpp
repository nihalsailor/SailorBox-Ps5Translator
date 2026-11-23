#pragma once

#include "ShaderTypes.hpp"
#include "RdnaOpcodes.hpp"
#include <span>
#include <vector>

namespace Sailor::Shader {

class RdnaDecoder {
public:
    RdnaDecoder() = default;

    // Decodes an entire RDNA 2 / gfx1013 shader program until s_endpgm or end of buffer
    [[nodiscard]] std::vector<RdnaInstruction> DecodeProgram(std::span<const u32> code) const;

    // Decodes a single instruction at wordIndex
    [[nodiscard]] RdnaInstruction DecodeInstruction(std::span<const u32> code, u32 wordIndex) const;

    // Determines the encoding family from the primary 32-bit instruction word
    [[nodiscard]] static RdnaFamily GetInstructionFamily(u32 firstWord);

    // Operand decoders
    [[nodiscard]] static RdnaOperand DecodeScalarSource(u32 code, u32 pc);
    [[nodiscard]] static RdnaOperand DecodeScalarDestination(u32 code, u32 pc);
    [[nodiscard]] static RdnaOperand DecodeVectorSource(u32 code, u32 pc);
    [[nodiscard]] static RdnaOperand DecodeVectorDestination(u32 code);

private:
    RdnaInstruction DecodeSop1(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeSop2(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeSopk(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeSopc(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeSopp(std::span<const u32> code, u32 wordIndex) const;

    RdnaInstruction DecodeVop1(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeVop2(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeVop3(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeVop3p(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeVopc(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeVintrp(std::span<const u32> code, u32 wordIndex) const;

    RdnaInstruction DecodeSmem(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeMubuf(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeMtbuf(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeFlat(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeDs(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeExp(std::span<const u32> code, u32 wordIndex) const;
    RdnaInstruction DecodeMimg(std::span<const u32> code, u32 wordIndex) const;
};

} // namespace Sailor::Shader
