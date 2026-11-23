#include "gpu/AgcDriver.hpp"
#include "gpu/VulkanContext.hpp"
#include "gpu/VulkanPipelineManager.hpp"
#include "gpu/TextureDetiler.hpp"
#include "shader/ShaderRecompiler.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sailor {

AgcDriver& AgcDriver::Instance() {
    static AgcDriver instance;
    return instance;
}

AgcDriver::AgcDriver() {
    m_recompiler = std::make_shared<Shader::ShaderRecompiler>();

    m_decoder.SetDrawCallback([this](const DrawCommand& cmd) {
        RecordDraw(cmd);
    });

    m_decoder.SetDispatchCallback([this](const DispatchCommand& cmd) {
        RecordDispatch(cmd);
    });

    m_computeDecoder.SetDispatchCallback([this](const DispatchCommand& cmd) {
        RecordComputeDispatch(cmd);
    });
}

u64 AgcDriver::GetShReg64(u32 regLo, u32 regHi) const {
    u32 lo = m_decoder.GetShReg(regLo);
    u32 hi = m_decoder.GetShReg(regHi);
    if (lo == 0 && hi == 0 && regLo >= 0x2C00) {
        lo = m_decoder.GetShReg(regLo - 0x2C00);
        hi = m_decoder.GetShReg(regHi - 0x2C00);
    }
    return (static_cast<u64>(hi) << 32) | lo;
}

u64 AgcDriver::GetComputeShReg64(u32 regLo, u32 regHi) const {
    u32 lo = m_computeDecoder.GetShReg(regLo);
    u32 hi = m_computeDecoder.GetShReg(regHi);
    if (lo == 0 && hi == 0 && regLo >= 0x2C00) {
        lo = m_computeDecoder.GetShReg(regLo - 0x2C00);
        hi = m_computeDecoder.GetShReg(regHi - 0x2C00);
    }
    return (static_cast<u64>(hi) << 32) | lo;
}

static bool TryReadShaderDwords(const u32* codePtr, size_t maxDwords, u32* outBuffer, size_t* outCount) {
#ifdef _WIN32
    __try {
        size_t count = 0;
        for (size_t i = 0; i < maxDwords; ++i) {
            u32 w = codePtr[i];
            outBuffer[count++] = w;
            if ((w & 0xFFFF0000) == 0xBF810000 || (w & 0xFFFF0000) == 0xBF8B0000) {
                break;
            }
        }
        *outCount = count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    size_t count = 0;
    for (size_t i = 0; i < maxDwords; ++i) {
        u32 w = codePtr[i];
        outBuffer[count++] = w;
        if ((w & 0xFFFF0000) == 0xBF810000 || (w & 0xFFFF0000) == 0xBF8B0000) {
            break;
        }
    }
    *outCount = count;
    return true;
#endif
}

void* AgcDriver::GetOrCreateShaderModule(u64 guestAddr, Shader::ShaderStage stage) {
    if (guestAddr == 0 || !m_recompiler) return nullptr;

    const u32* codePtr = reinterpret_cast<const u32*>(guestAddr);
    std::vector<u32> code(4096);
    size_t dwordCount = 0;

    if (!TryReadShaderDwords(codePtr, 4096, code.data(), &dwordCount) || dwordCount == 0) {
        SB_LOG_WARN("AgcDriver", "Could not safely read guest shader at 0x{:X}", guestAddr);
        return nullptr;
    }
    code.resize(dwordCount);

    Shader::ShaderBinary bin;
    bin.stage = stage;
    bin.code = std::move(code);
    bin.guestAddress = guestAddr;

    auto res = m_recompiler->Recompile(bin);
    if (!res.success || res.spirv.empty()) {
        SB_LOG_WARN("AgcDriver", "Failed to recompile shader at 0x{:X}: {}", guestAddr, res.errorMessage);
        return nullptr;
    }

    auto it = m_pipelineCache.find(res.shaderHash);
    if (it != m_pipelineCache.end() && it->second.shaderModule) {
        return it->second.shaderModule;
    }

    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev) return nullptr;

    auto vkCreateShaderModule = reinterpret_cast<int(*)(void*, const void*, const void*, void**)>(
        VulkanContext::Instance().GetProcAddr("vkCreateShaderModule"));
    if (!vkCreateShaderModule) return nullptr;

    struct VkShaderModuleCreateInfo {
        u32 sType{16}; // VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
        const void* pNext{nullptr};
        u32 flags{0};
        size_t codeSize{0};
        const u32* pCode{nullptr};
    };

    VkShaderModuleCreateInfo createInfo{};
    createInfo.codeSize = res.spirv.size() * sizeof(u32);
    createInfo.pCode = res.spirv.data();

    void* shaderMod = nullptr;
    int err = vkCreateShaderModule(dev, &createInfo, nullptr, &shaderMod);
    if (err == 0 && shaderMod) {
        m_pipelineCache[res.shaderHash].shaderModule = shaderMod;
        SB_LOG_INFO("AgcDriver", "Created VkShaderModule {:p} for shader at 0x{:X} (hash 0x{:X})",
            shaderMod, guestAddr, res.shaderHash);
        return shaderMod;
    }
    return nullptr;
}

void AgcDriver::RecordDraw(const DrawCommand& cmd) {
    u64 vsAddr = GetShReg64(0x2C48, 0x2C49);
    u64 psAddr = GetShReg64(0x2C08, 0x2C09);
    void* vsMod = vsAddr ? GetOrCreateShaderModule(vsAddr, Shader::ShaderStage::Vertex) : nullptr;
    void* psMod = psAddr ? GetOrCreateShaderModule(psAddr, Shader::ShaderStage::Fragment) : nullptr;

    if (vsMod && !m_activePipeline) {
        GraphicsPipelineStateKey key{};
        key.vsModule = vsMod;
        key.psModule = psMod;

        u32 suScMode = m_decoder.GetContextReg(0xA204);
        if (suScMode) {
            key.cullMode = (suScMode & 0x3);
            key.frontFace = (suScMode & 0x4) ? 1 : 0;
        }
        u32 dbDepth = m_decoder.GetContextReg(0xA080);
        if (dbDepth) {
            key.depthTestEnable = (dbDepth & 0x1) != 0;
            key.depthWriteEnable = (dbDepth & 0x2) != 0;
            key.depthCompareOp = (dbDepth >> 4) & 0x7;
        }
        void* pipe = VulkanPipelineManager::Instance().GetOrCreateGraphicsPipeline(key);
        if (pipe) {
            m_activePipeline = pipe;
        }
    }

    void* cb = VulkanContext::Instance().GetCommandBuffer();
    if (cb && m_activePipeline) {
        auto vkCmdDraw = reinterpret_cast<void(*)(void*, u32, u32, u32, u32)>(
            VulkanContext::Instance().GetProcAddr("vkCmdDraw"));
        auto vkCmdDrawIndexed = reinterpret_cast<void(*)(void*, u32, u32, u32, i32, u32)>(
            VulkanContext::Instance().GetProcAddr("vkCmdDrawIndexed"));
        auto vkCmdDrawIndexedIndirect = reinterpret_cast<void(*)(void*, void*, u64, u32, u32)>(
            VulkanContext::Instance().GetProcAddr("vkCmdDrawIndexedIndirect"));

        if (cmd.isIndirect) {
            if (vkCmdDrawIndexedIndirect && m_activeIndirectBuffer) {
                vkCmdDrawIndexedIndirect(cb, m_activeIndirectBuffer, cmd.indirectBufferAddress, 1, 20);
            }
        } else if (cmd.isIndexed) {
            if (vkCmdDrawIndexed) {
                vkCmdDrawIndexed(cb, cmd.indexCount, cmd.instanceCount, cmd.firstIndex, cmd.vertexOffset, cmd.firstInstance);
            }
        } else {
            if (vkCmdDraw) {
                vkCmdDraw(cb, cmd.vertexCount, cmd.instanceCount, cmd.firstIndex, cmd.firstInstance);
            }
        }
    }

    if (cmd.isIndexed) {
        SB_LOG_DEBUG("AgcDriver", "Executing Indexed Draw: {} indices from base 0x{:X}",
            cmd.indexCount, cmd.indexBufferAddress);
    } else {
        SB_LOG_DEBUG("AgcDriver", "Executing Auto Draw: {} vertices, {} instances",
            cmd.vertexCount, cmd.instanceCount);
    }
}

void AgcDriver::RecordDispatch(const DispatchCommand& cmd) {
    u64 csAddr = GetShReg64(0x2E00, 0x2E01);
    void* csMod = csAddr ? GetOrCreateShaderModule(csAddr, Shader::ShaderStage::Compute) : nullptr;
    if (csMod && !m_activePipeline) {
        void* pipe = VulkanPipelineManager::Instance().GetOrCreateComputePipeline(csMod);
        if (pipe) {
            m_activePipeline = pipe;
        }
    }

    void* cb = VulkanContext::Instance().GetCommandBuffer();
    if (cb && m_activePipeline) {
        auto vkCmdDispatch = reinterpret_cast<void(*)(void*, u32, u32, u32)>(
            VulkanContext::Instance().GetProcAddr("vkCmdDispatch"));
        auto vkCmdDispatchIndirect = reinterpret_cast<void(*)(void*, void*, u64)>(
            VulkanContext::Instance().GetProcAddr("vkCmdDispatchIndirect"));

        if (cmd.isIndirect) {
            if (vkCmdDispatchIndirect && m_activeIndirectBuffer) {
                vkCmdDispatchIndirect(cb, m_activeIndirectBuffer, cmd.indirectBufferAddress);
            }
        } else {
            if (vkCmdDispatch) {
                vkCmdDispatch(cb, cmd.threadsX, cmd.threadsY, cmd.threadsZ);
            }
        }
    }

    SB_LOG_DEBUG("AgcDriver", "Executing Compute Dispatch: ({}, {}, {}) groups",
        cmd.threadsX, cmd.threadsY, cmd.threadsZ);
}

void AgcDriver::RecordComputeDispatch(const DispatchCommand& cmd) {
    u64 csAddr = GetComputeShReg64(0x2E00, 0x2E01);
    void* csMod = csAddr ? GetOrCreateShaderModule(csAddr, Shader::ShaderStage::Compute) : nullptr;
    void* pipe = csMod ? VulkanPipelineManager::Instance().GetOrCreateComputePipeline(csMod) : nullptr;

    void* cb = VulkanContext::Instance().GetComputeCommandBuffer();
    if (cb && pipe) {
        auto vkCmdBindPipeline = reinterpret_cast<void(*)(void*, u32, void*)>(
            VulkanContext::Instance().GetProcAddr("vkCmdBindPipeline"));
        auto vkCmdDispatch = reinterpret_cast<void(*)(void*, u32, u32, u32)>(
            VulkanContext::Instance().GetProcAddr("vkCmdDispatch"));
        auto vkCmdDispatchIndirect = reinterpret_cast<void(*)(void*, void*, u64)>(
            VulkanContext::Instance().GetProcAddr("vkCmdDispatchIndirect"));

        if (vkCmdBindPipeline) {
            vkCmdBindPipeline(cb, 1 /* VK_PIPELINE_BIND_POINT_COMPUTE */, pipe);
        }

        if (cmd.isIndirect) {
            if (vkCmdDispatchIndirect && m_activeIndirectBuffer) {
                vkCmdDispatchIndirect(cb, m_activeIndirectBuffer, cmd.indirectBufferAddress);
            }
        } else {
            if (vkCmdDispatch) {
                vkCmdDispatch(cb, cmd.threadsX, cmd.threadsY, cmd.threadsZ);
            }
        }
    }

    SB_LOG_DEBUG("AgcDriver", "Executing Async Compute Dispatch: ({}, {}, {}) groups",
        cmd.threadsX, cmd.threadsY, cmd.threadsZ);
}

i32 AgcDriver::Initialize() {
    VulkanContext::Instance().Initialize();
    VulkanPipelineManager::Instance().Initialize();
    SB_LOG_INFO("AgcDriver", "SailorBox AGC Driver initialized with Vulkan 1.3 Pipeline Manager");
    return SCE_OK;
}

i32 AgcDriver::SubmitCommandBuffer(const u32* dcbStart, u32 dcbSizeDwords, u32 queueId) {
    if (!dcbStart || dcbSizeDwords == 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    std::lock_guard lock(m_submitMutex);
    SB_LOG_INFO("AgcDriver", "Submitting DCB queue={}: ptr={:p}, dwords={}",
        queueId, static_cast<const void*>(dcbStart), dcbSizeDwords);

    bool ok = m_decoder.DecodeBuffer(std::span<const u32>(dcbStart, dcbSizeDwords));
    if (!ok) {
        SB_LOG_ERROR("AgcDriver", "Error occurred during PM4 command buffer decoding");
        return SCE_KERNEL_ERROR_EINVAL;
    }

    m_submittedFrames.fetch_add(1, std::memory_order_relaxed);
    return SCE_OK;
}

i32 AgcDriver::SubmitAcb(const u32* acbStart, u32 acbSizeDwords, u32 queueId) {
    if (!acbStart || acbSizeDwords == 0) {
        return SCE_KERNEL_ERROR_EINVAL;
    }

    // Async Compute buffers submit independently from DCB draw commands
    std::lock_guard lock(m_computeMutex);
    SB_LOG_DEBUG("AgcDriver", "Submitting ACB queue={}: ptr={:p}, dwords={}",
        queueId, static_cast<const void*>(acbStart), acbSizeDwords);

    void* cb = VulkanContext::Instance().GetComputeCommandBuffer();
    auto vkBeginCommandBuffer = reinterpret_cast<int(*)(void*, const void*)>(
        VulkanContext::Instance().GetProcAddr("vkBeginCommandBuffer"));
    auto vkEndCommandBuffer = reinterpret_cast<int(*)(void*)>(
        VulkanContext::Instance().GetProcAddr("vkEndCommandBuffer"));

    struct VkCommandBufferBeginInfo {
        u32 sType{42};
        const void* pNext{nullptr};
        u32 flags{1}; // VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        const void* pInheritanceInfo{nullptr};
    };

    if (cb && vkBeginCommandBuffer) {
        VkCommandBufferBeginInfo beginInfo{};
        vkBeginCommandBuffer(cb, &beginInfo);
    }

    bool ok = m_computeDecoder.DecodeBuffer(std::span<const u32>(acbStart, acbSizeDwords));

    if (cb && vkEndCommandBuffer) {
        vkEndCommandBuffer(cb);
        VulkanContext::Instance().SubmitComputeCommandBuffer(cb, true);
    }

    return ok ? SCE_OK : SCE_KERNEL_ERROR_EINVAL;
}

} // namespace Sailor
