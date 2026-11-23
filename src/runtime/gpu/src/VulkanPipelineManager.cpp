#include "gpu/VulkanPipelineManager.hpp"
#include "gpu/VulkanContext.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <array>
#include <cstring>

namespace Sailor {

VulkanPipelineManager& VulkanPipelineManager::Instance() {
    static VulkanPipelineManager instance;
    return instance;
}

void VulkanPipelineManager::Initialize() {
    std::lock_guard lock(m_mutex);
    CreateDescriptorLayouts();
    CreatePipelineLayout();
    SB_LOG_INFO("PipelineManager", "Vulkan Pipeline & Descriptor Manager initialized");
}

void VulkanPipelineManager::Shutdown() {
    std::lock_guard lock(m_mutex);
    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev) return;

    auto vkDestroyPipeline = reinterpret_cast<void(*)(void*, void*, const void*)>(
        VulkanContext::Instance().GetProcAddr("vkDestroyPipeline"));
    auto vkDestroyPipelineLayout = reinterpret_cast<void(*)(void*, void*, const void*)>(
        VulkanContext::Instance().GetProcAddr("vkDestroyPipelineLayout"));
    auto vkDestroyDescriptorPool = reinterpret_cast<void(*)(void*, void*, const void*)>(
        VulkanContext::Instance().GetProcAddr("vkDestroyDescriptorPool"));
    auto vkDestroyDescriptorSetLayout = reinterpret_cast<void(*)(void*, void*, const void*)>(
        VulkanContext::Instance().GetProcAddr("vkDestroyDescriptorSetLayout"));

    for (auto& [k, p] : m_graphicsPipelines) {
        if (vkDestroyPipeline && p) vkDestroyPipeline(dev, p, nullptr);
    }
    m_graphicsPipelines.clear();

    for (auto& [k, p] : m_computePipelines) {
        if (vkDestroyPipeline && p) vkDestroyPipeline(dev, p, nullptr);
    }
    m_computePipelines.clear();

    if (vkDestroyPipelineLayout && m_pipelineLayout) {
        vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
        m_pipelineLayout = nullptr;
    }
    if (vkDestroyDescriptorPool && m_descriptorPool) {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool = nullptr;
    }
    if (vkDestroyDescriptorSetLayout && m_descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = nullptr;
    }
}

bool VulkanPipelineManager::CreateDescriptorLayouts() {
    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev) return false;

    auto vkCreateDescriptorSetLayout = reinterpret_cast<int(*)(void*, const void*, const void*, void**)>(
        VulkanContext::Instance().GetProcAddr("vkCreateDescriptorSetLayout"));
    auto vkCreateDescriptorPool = reinterpret_cast<int(*)(void*, const void*, const void*, void**)>(
        VulkanContext::Instance().GetProcAddr("vkCreateDescriptorPool"));

    if (!vkCreateDescriptorSetLayout || !vkCreateDescriptorPool) return false;

    // Standard PS5 resource descriptor layout:
    // Binding 0: Storage Buffers (BDA / Uniform / V# buffers)
    // Binding 1: Sampled Images (Textures / T#)
    // Binding 2: Samplers (S#)
    struct VkDescriptorSetLayoutBinding {
        u32 binding;
        u32 descriptorType;
        u32 descriptorCount;
        u32 stageFlags;
        const void* pImmutableSamplers;
    };

    struct VkDescriptorSetLayoutCreateInfo {
        u32 sType{32}; // VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        const void* pNext{nullptr};
        u32 flags{0};
        u32 bindingCount{0};
        const VkDescriptorSetLayoutBinding* pBindings{nullptr};
    };

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0] = {0, 7 /* STORAGE_BUFFER */, 16, 0x7FFFFFFF /* ALL_STAGES */, nullptr};
    bindings[1] = {1, 2 /* SAMPLED_IMAGE */, 32, 0x7FFFFFFF, nullptr};
    bindings[2] = {2, 0 /* SAMPLER */, 16, 0x7FFFFFFF, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<u32>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    int err = vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_descriptorSetLayout);
    if (err != 0 || !m_descriptorSetLayout) {
        SB_LOG_ERROR("PipelineManager", "Failed to create VkDescriptorSetLayout (err={})", err);
        return false;
    }

    struct VkDescriptorPoolSize {
        u32 type;
        u32 descriptorCount;
    };

    struct VkDescriptorPoolCreateInfo {
        u32 sType{33}; // VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        const void* pNext{nullptr};
        u32 flags{1 /* FREE_DESCRIPTOR_SET_BIT */};
        u32 maxSets{1024};
        u32 poolSizeCount{3};
        const VkDescriptorPoolSize* pPoolSizes{nullptr};
    };

    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0] = {7 /* STORAGE_BUFFER */, 4096};
    poolSizes[1] = {2 /* SAMPLED_IMAGE */, 8192};
    poolSizes[2] = {0 /* SAMPLER */, 4096};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.maxSets = 1024;
    poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    err = vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_descriptorPool);
    if (err != 0 || !m_descriptorPool) {
        SB_LOG_ERROR("PipelineManager", "Failed to create VkDescriptorPool (err={})", err);
        return false;
    }

    return true;
}

bool VulkanPipelineManager::CreatePipelineLayout() {
    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev || !m_descriptorSetLayout) return false;

    auto vkCreatePipelineLayout = reinterpret_cast<int(*)(void*, const void*, const void*, void**)>(
        VulkanContext::Instance().GetProcAddr("vkCreatePipelineLayout"));
    if (!vkCreatePipelineLayout) return false;

    struct VkPushConstantRange {
        u32 stageFlags;
        u32 offset;
        u32 size;
    };

    struct VkPipelineLayoutCreateInfo {
        u32 sType{30}; // VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        const void* pNext{nullptr};
        u32 flags{0};
        u32 setLayoutCount{1};
        const void* const* pSetLayouts{nullptr};
        u32 pushConstantRangeCount{1};
        const VkPushConstantRange* pPushConstantRanges{nullptr};
    };

    VkPushConstantRange pushRange{0x7FFFFFFF /* ALL_STAGES */, 0, 128};

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    int err = vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &m_pipelineLayout);
    if (err != 0 || !m_pipelineLayout) {
        SB_LOG_ERROR("PipelineManager", "Failed to create VkPipelineLayout (err={})", err);
        return false;
    }
    return true;
}

void* VulkanPipelineManager::AllocateDescriptorSet() {
    std::lock_guard lock(m_mutex);
    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev || !m_descriptorPool || !m_descriptorSetLayout) return nullptr;

    auto vkAllocateDescriptorSets = reinterpret_cast<int(*)(void*, const void*, void**)>(
        VulkanContext::Instance().GetProcAddr("vkAllocateDescriptorSets"));
    if (!vkAllocateDescriptorSets) return nullptr;

    struct VkDescriptorSetAllocateInfo {
        u32 sType{34}; // VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        const void* pNext{nullptr};
        void* descriptorPool;
        u32 descriptorSetCount{1};
        const void* const* pSetLayouts{nullptr};
    };

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    void* set = nullptr;
    int err = vkAllocateDescriptorSets(dev, &allocInfo, &set);
    if (err == 0 && set) {
        return set;
    }
    return nullptr;
}

void VulkanPipelineManager::BindStorageBuffer(void* descriptorSet, u32 binding, void* vkBuffer, u64 offset, u64 range) {
    if (!descriptorSet || !vkBuffer) return;
    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev) return;

    auto vkUpdateDescriptorSets = reinterpret_cast<void(*)(void*, u32, const void*, u32, const void*)>(
        VulkanContext::Instance().GetProcAddr("vkUpdateDescriptorSets"));
    if (!vkUpdateDescriptorSets) return;

    struct VkDescriptorBufferInfo {
        void* buffer;
        u64 offset;
        u64 range;
    };

    struct VkWriteDescriptorSet {
        u32 sType{35}; // VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
        const void* pNext{nullptr};
        void* dstSet;
        u32 dstBinding;
        u32 dstArrayElement{0};
        u32 descriptorCount{1};
        u32 descriptorType{7 /* STORAGE_BUFFER */};
        const void* pImageInfo{nullptr};
        const VkDescriptorBufferInfo* pBufferInfo{nullptr};
        const void* pTexelBufferView{nullptr};
    };

    VkDescriptorBufferInfo bufInfo{vkBuffer, offset, range};
    VkWriteDescriptorSet write{};
    write.dstSet = descriptorSet;
    write.dstBinding = binding;
    write.pBufferInfo = &bufInfo;

    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
}

void VulkanPipelineManager::BindSampledImage(void* descriptorSet, u32 binding, void* vkImageView, void* vkSampler) {
    if (!descriptorSet || !vkImageView) return;
    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev) return;

    auto vkUpdateDescriptorSets = reinterpret_cast<void(*)(void*, u32, const void*, u32, const void*)>(
        VulkanContext::Instance().GetProcAddr("vkUpdateDescriptorSets"));
    if (!vkUpdateDescriptorSets) return;

    struct VkDescriptorImageInfo {
        void* sampler;
        void* imageView;
        u32 imageLayout{5 /* VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL */};
    };

    struct VkWriteDescriptorSet {
        u32 sType{35};
        const void* pNext{nullptr};
        void* dstSet;
        u32 dstBinding;
        u32 dstArrayElement{0};
        u32 descriptorCount{1};
        u32 descriptorType{2 /* SAMPLED_IMAGE */};
        const VkDescriptorImageInfo* pImageInfo{nullptr};
        const void* pBufferInfo{nullptr};
        const void* pTexelBufferView{nullptr};
    };

    VkDescriptorImageInfo imgInfo{vkSampler, vkImageView, 5};
    VkWriteDescriptorSet write{};
    write.dstSet = descriptorSet;
    write.dstBinding = binding;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
}

void* VulkanPipelineManager::GetOrCreateGraphicsPipeline(const GraphicsPipelineStateKey& key) {
    if (!key.vsModule) return nullptr;

    std::lock_guard lock(m_mutex);
    auto it = m_graphicsPipelines.find(key);
    if (it != m_graphicsPipelines.end()) {
        return it->second;
    }

    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev || !m_pipelineLayout) return nullptr;

    auto vkCreateGraphicsPipelines = reinterpret_cast<int(*)(void*, void*, u32, const void*, const void*, void**)>(
        VulkanContext::Instance().GetProcAddr("vkCreateGraphicsPipelines"));
    if (!vkCreateGraphicsPipelines) return nullptr;

    // Define Vulkan structures
    struct VkPipelineShaderStageCreateInfo {
        u32 sType{18}; // VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        const void* pNext{nullptr};
        u32 flags{0};
        u32 stage{1}; // 1 = VERTEX, 16 = FRAGMENT
        void* module{nullptr};
        const char* pName{"main"};
        const void* pSpecializationInfo{nullptr};
    };

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    VkPipelineShaderStageCreateInfo vsStage{};
    vsStage.stage = 1; // VERTEX
    vsStage.module = key.vsModule;
    stages.push_back(vsStage);

    if (key.psModule) {
        VkPipelineShaderStageCreateInfo psStage{};
        psStage.stage = 16; // FRAGMENT
        psStage.module = key.psModule;
        stages.push_back(psStage);
    }

    struct VkPipelineVertexInputStateCreateInfo {
        u32 sType{19};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 vertexBindingDescriptionCount{0};
        const void* pVertexBindingDescriptions{nullptr};
        u32 vertexAttributeDescriptionCount{0};
        const void* pVertexAttributeDescriptions{nullptr};
    } vertexInput{};

    struct VkPipelineInputAssemblyStateCreateInfo {
        u32 sType{20};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 topology{3}; // TRIANGLE_LIST
        u32 primitiveRestartEnable{0};
    } inputAssembly{};
    inputAssembly.topology = key.primitiveTopology;

    struct VkPipelineViewportStateCreateInfo {
        u32 sType{22};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 viewportCount{1};
        const void* pViewports{nullptr};
        u32 scissorCount{1};
        const void* pScissors{nullptr};
    } viewportState{};

    struct VkPipelineRasterizationStateCreateInfo {
        u32 sType{23};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 depthClampEnable{0};
        u32 rasterizerDiscardEnable{0};
        u32 polygonMode{0}; // FILL
        u32 cullMode{2};    // BACK
        u32 frontFace{0};   // CCW
        u32 depthBiasEnable{0};
        float depthBiasConstantFactor{0.0f};
        float depthBiasClamp{0.0f};
        float depthBiasSlopeFactor{0.0f};
        float lineWidth{1.0f};
    } rasterizer{};
    rasterizer.cullMode = key.cullMode;
    rasterizer.frontFace = key.frontFace;

    struct VkPipelineMultisampleStateCreateInfo {
        u32 sType{24};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 rasterizationSamples{1};
        u32 sampleShadingEnable{0};
        float minSampleShading{1.0f};
        const void* pSampleMask{nullptr};
        u32 alphaToCoverageEnable{0};
        u32 alphaToOneEnable{0};
    } multisampling{};

    struct VkPipelineDepthStencilStateCreateInfo {
        u32 sType{25};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 depthTestEnable{1};
        u32 depthWriteEnable{1};
        u32 depthCompareOp{1}; // LESS
        u32 depthBoundsTestEnable{0};
        u32 stencilTestEnable{0};
        u8 front[28]{0};
        u8 back[28]{0};
        float minDepthBounds{0.0f};
        float maxDepthBounds{1.0f};
    } depthStencil{};
    depthStencil.depthTestEnable = key.depthTestEnable ? 1 : 0;
    depthStencil.depthWriteEnable = key.depthWriteEnable ? 1 : 0;
    depthStencil.depthCompareOp = key.depthCompareOp;

    struct VkPipelineColorBlendAttachmentState {
        u32 blendEnable{0};
        u32 srcColorBlendFactor{1};
        u32 dstColorBlendFactor{0};
        u32 colorBlendOp{0};
        u32 srcAlphaBlendFactor{1};
        u32 dstAlphaBlendFactor{0};
        u32 alphaBlendOp{0};
        u32 colorWriteMask{0xF};
    } colorAttachment{};
    colorAttachment.blendEnable = key.blendEnable ? 1 : 0;
    colorAttachment.colorWriteMask = key.colorWriteMask;

    struct VkPipelineColorBlendStateCreateInfo {
        u32 sType{26};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 logicOpEnable{0};
        u32 logicOp{0};
        u32 attachmentCount{1};
        const VkPipelineColorBlendAttachmentState* pAttachments{nullptr};
        float blendConstants[4]{0.0f, 0.0f, 0.0f, 0.0f};
    } colorBlending{};
    colorBlending.pAttachments = &colorAttachment;

    struct VkPipelineDynamicStateCreateInfo {
        u32 sType{27};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 dynamicStateCount{2};
        const u32* pDynamicStates{nullptr};
    } dynamicState{};
    const u32 dynamicStates[] = {0 /* VIEWPORT */, 1 /* SCISSOR */};
    dynamicState.pDynamicStates = dynamicStates;

    // Vulkan 1.3 dynamic rendering (VkPipelineRenderingCreateInfo)
    struct VkPipelineRenderingCreateInfo {
        u32 sType{1000044002}; // VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO
        const void* pNext{nullptr};
        u32 viewMask{0};
        u32 colorAttachmentCount{1};
        const u32* pColorAttachmentFormats{nullptr};
        u32 depthAttachmentFormat{126}; // VK_FORMAT_D32_SFLOAT
        u32 stencilAttachmentFormat{0};
    } renderingInfo{};
    const u32 colorFormat = 44; // VK_FORMAT_B8G8R8A8_UNORM
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    struct VkGraphicsPipelineCreateInfo {
        u32 sType{28}; // VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
        const void* pNext{nullptr};
        u32 flags{0};
        u32 stageCount{0};
        const VkPipelineShaderStageCreateInfo* pStages{nullptr};
        const VkPipelineVertexInputStateCreateInfo* pVertexInputState{nullptr};
        const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState{nullptr};
        const void* pTessellationState{nullptr};
        const VkPipelineViewportStateCreateInfo* pViewportState{nullptr};
        const VkPipelineRasterizationStateCreateInfo* pRasterizationState{nullptr};
        const VkPipelineMultisampleStateCreateInfo* pMultisampleState{nullptr};
        const VkPipelineDepthStencilStateCreateInfo* pDepthStencilState{nullptr};
        const VkPipelineColorBlendStateCreateInfo* pColorBlendState{nullptr};
        const VkPipelineDynamicStateCreateInfo* pDynamicState{nullptr};
        void* layout{nullptr};
        void* renderPass{nullptr};
        u32 subpass{0};
        void* basePipelineHandle{nullptr};
        i32 basePipelineIndex{-1};
    } pipelineInfo{};

    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = static_cast<u32>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;

    void* pipeline = nullptr;
    int err = vkCreateGraphicsPipelines(dev, nullptr, 1, &pipelineInfo, nullptr, &pipeline);
    if (err == 0 && pipeline) {
        m_graphicsPipelines[key] = pipeline;
        SB_LOG_INFO("PipelineManager", "Created VkGraphicsPipeline {:p} (vs={:p}, ps={:p})",
            pipeline, key.vsModule, key.psModule);
        return pipeline;
    }

    SB_LOG_WARN("PipelineManager", "Failed to create VkGraphicsPipeline (err={})", err);
    return nullptr;
}

void* VulkanPipelineManager::GetOrCreateComputePipeline(void* csModule) {
    if (!csModule) return nullptr;

    std::lock_guard lock(m_mutex);
    auto it = m_computePipelines.find(csModule);
    if (it != m_computePipelines.end()) {
        return it->second;
    }

    void* dev = VulkanContext::Instance().GetDevice();
    if (!dev || !m_pipelineLayout) return nullptr;

    auto vkCreateComputePipelines = reinterpret_cast<int(*)(void*, void*, u32, const void*, const void*, void**)>(
        VulkanContext::Instance().GetProcAddr("vkCreateComputePipelines"));
    if (!vkCreateComputePipelines) return nullptr;

    struct VkPipelineShaderStageCreateInfo {
        u32 sType{18};
        const void* pNext{nullptr};
        u32 flags{0};
        u32 stage{32}; // 32 = COMPUTE
        void* module{nullptr};
        const char* pName{"main"};
        const void* pSpecializationInfo{nullptr};
    } csStage{};
    csStage.module = csModule;

    struct VkComputePipelineCreateInfo {
        u32 sType{29}; // VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        const void* pNext{nullptr};
        u32 flags{0};
        VkPipelineShaderStageCreateInfo stage;
        void* layout;
        void* basePipelineHandle{nullptr};
        i32 basePipelineIndex{-1};
    } pipelineInfo{};
    pipelineInfo.stage = csStage;
    pipelineInfo.layout = m_pipelineLayout;

    void* pipeline = nullptr;
    int err = vkCreateComputePipelines(dev, nullptr, 1, &pipelineInfo, nullptr, &pipeline);
    if (err == 0 && pipeline) {
        m_computePipelines[csModule] = pipeline;
        SB_LOG_INFO("PipelineManager", "Created VkComputePipeline {:p} (cs={:p})", pipeline, csModule);
        return pipeline;
    }

    SB_LOG_WARN("PipelineManager", "Failed to create VkComputePipeline (err={})", err);
    return nullptr;
}

} // namespace Sailor
