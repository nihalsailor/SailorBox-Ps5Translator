#pragma once

#include "ShaderTypes.hpp"
#include "RdnaDecoder.hpp"
#include "ShaderIr.hpp"
#include "SpirvBuilder.hpp"
#include "ShaderCache.hpp"
#include <vector>
#include <memory>

namespace Sailor::Shader {

struct RecompileResult {
    bool success = false;
    std::vector<u32> spirv;
    u64 shaderHash = 0;
    std::string errorMessage;
    size_t decodedInstructions = 0;
};

class ShaderRecompiler {
public:
    explicit ShaderRecompiler(std::shared_ptr<ShaderCache> cache = nullptr);

    // Recompiles a shader binary into SPIR-V
    RecompileResult Recompile(const ShaderBinary& binary);

    // Accessors
    [[nodiscard]] const RdnaDecoder& GetDecoder() const { return m_decoder; }
    [[nodiscard]] std::shared_ptr<ShaderCache> GetCache() const { return m_cache; }

private:
    void TranslateInstruction(const RdnaInstruction& inst, IrProgram& ir);
    std::vector<u32> EmitSpirvFromIr(const IrProgram& ir);

    RdnaDecoder m_decoder;
    std::shared_ptr<ShaderCache> m_cache;
};

} // namespace Sailor::Shader
