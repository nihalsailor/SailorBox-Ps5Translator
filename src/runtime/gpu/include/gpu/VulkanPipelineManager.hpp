#pragma once

#include "common/Types.hpp"
#include <unordered_map>
#include <mutex>
#include <vector>

namespace Sailor {

struct GraphicsPipelineStateKey {
    void* vsModule{nullptr};
    void* psModule{nullptr};
    u32 primitiveTopology{3}; // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3
    u32 cullMode{2};          // VK_CULL_MODE_BACK_BIT = 2
    u32 frontFace{0};         // VK_FRONT_FACE_COUNTER_CLOCKWISE = 0
    bool depthTestEnable{true};
    bool depthWriteEnable{true};
    u32 depthCompareOp{1};    // VK_COMPARE_OP_LESS = 1
    bool blendEnable{false};
    u32 colorWriteMask{0xF};

    bool operator==(const GraphicsPipelineStateKey& other) const = default;
};

struct GraphicsPipelineHash {
    size_t operator()(const GraphicsPipelineStateKey& k) const noexcept {
        size_t h = reinterpret_cast<size_t>(k.vsModule) ^ (reinterpret_cast<size_t>(k.psModule) << 1);
        h ^= (static_cast<size_t>(k.primitiveTopology) << 8);
        h ^= (static_cast<size_t>(k.cullMode) << 16);
        h ^= (static_cast<size_t>(k.depthTestEnable) << 24);
        return h;
    }
};

class VulkanPipelineManager {
public:
    static VulkanPipelineManager& Instance();

    void Initialize();
    void Shutdown();

    void* GetOrCreateGraphicsPipeline(const GraphicsPipelineStateKey& key);
    void* GetOrCreateComputePipeline(void* csModule);

    void* GetPipelineLayout() const { return m_pipelineLayout; }
    void* GetDescriptorSetLayout() const { return m_descriptorSetLayout; }
    void* AllocateDescriptorSet();

    void BindStorageBuffer(void* descriptorSet, u32 binding, void* vkBuffer, u64 offset, u64 range);
    void BindSampledImage(void* descriptorSet, u32 binding, void* vkImageView, void* vkSampler);

private:
    VulkanPipelineManager() = default;

    bool CreateDescriptorLayouts();
    bool CreatePipelineLayout();

    std::mutex m_mutex;
    void* m_descriptorPool{nullptr};
    void* m_descriptorSetLayout{nullptr};
    void* m_pipelineLayout{nullptr};
    void* m_renderPass{nullptr};

    std::unordered_map<GraphicsPipelineStateKey, void*, GraphicsPipelineHash> m_graphicsPipelines;
    std::unordered_map<void*, void*> m_computePipelines;
};

} // namespace Sailor
