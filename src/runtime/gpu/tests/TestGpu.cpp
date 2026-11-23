#include "gpu/Pm4Decoder.hpp"
#include "gpu/AgcDriver.hpp"
#include "gpu/VideoOut.hpp"
#include "gpu/VulkanContext.hpp"
#include "gpu/TextureDetiler.hpp"
#include "gpu/VulkanPipelineManager.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>

namespace {

Sailor::u32 MakeHeader(Sailor::u32 opcode, Sailor::u32 payloadCount) {
    // Type 3: bits [31:30] = 3
    // count: bits [29:16] = (N - 1) per AMD PM4 Type-3 specification
    // opcode: bits [15:8]
    Sailor::u32 countField = (payloadCount > 0) ? (payloadCount - 1) : 0;
    return (3U << 30) | ((countField & 0x3FFFU) << 16) | ((opcode & 0xFFU) << 8);
}

} // namespace

void TestPm4Decoder() {
    std::cout << "[TEST] Running PM4 Decoder tests...\n";
    Sailor::Pm4Decoder decoder;

    bool drawAutoCalled = false;
    bool drawIndexedCalled = false;
    bool dispatchCalled = false;

    decoder.SetDrawCallback([&](const Sailor::DrawCommand& cmd) {
        if (!cmd.isIndexed) {
            drawAutoCalled = true;
            assert(cmd.vertexCount == 3); // 3 vertices
        } else {
            drawIndexedCalled = true;
            assert(cmd.indexCount == 36);
            assert(cmd.indexBufferAddress == 0x140050000ULL);
        }
    });

    decoder.SetDispatchCallback([&](const Sailor::DispatchCommand& cmd) {
        dispatchCalled = true;
        assert(cmd.threadsX == 16);
        assert(cmd.threadsY == 8);
        assert(cmd.threadsZ == 1);
    });

    std::vector<Sailor::u32> cmdbuf;

    // 1. Packet: SET_CONTEXT_REG (reg 0x100 = 0xDEADBEEF)
    cmdbuf.push_back(MakeHeader(Sailor::Pm4::IT_SET_CONTEXT_REG, 2));
    cmdbuf.push_back(0x100);
    cmdbuf.push_back(0xDEADBEEF);

    // 2. Packet: DRAW_INDEX_AUTO (3 vertices)
    cmdbuf.push_back(MakeHeader(Sailor::Pm4::IT_DRAW_INDEX_AUTO, 2));
    cmdbuf.push_back(3); // count
    cmdbuf.push_back(0); // vgt initiator

    // 3. Packet: INDEX_BASE
    cmdbuf.push_back(MakeHeader(Sailor::Pm4::IT_INDEX_BASE, 2));
    cmdbuf.push_back(0x40050000); // lo
    cmdbuf.push_back(0x00000001); // hi (0x140050000)

    // 4. Packet: DRAW_INDEX_2 (36 indices)
    cmdbuf.push_back(MakeHeader(Sailor::Pm4::IT_DRAW_INDEX_2, 5));
    cmdbuf.push_back(100); // max_size
    cmdbuf.push_back(0x40050000); // base_lo
    cmdbuf.push_back(0x00000001); // base_hi
    cmdbuf.push_back(36); // count
    cmdbuf.push_back(0); // initiator

    // 5. Packet: DISPATCH_DIRECT (16, 8, 1)
    cmdbuf.push_back(MakeHeader(Sailor::Pm4::IT_DISPATCH_DIRECT, 3));
    cmdbuf.push_back(16);
    cmdbuf.push_back(8);
    cmdbuf.push_back(1);

    // 6. Trailing 0x00000000 NOP padding and Type-0 register packets
    cmdbuf.push_back(0x00000000);
    cmdbuf.push_back(0x00000000);
    cmdbuf.push_back((1U << 16) | 0x200); // Type-0 header (count=1 -> 2 payload DWORDs)
    cmdbuf.push_back(0x11112222);
    cmdbuf.push_back(0x33334444);
    cmdbuf.push_back(0x00000000); // Trailing NOP

    // 7. Packet: IT_WRITE_DATA to test memory
    Sailor::u32 memTarget[4] = {0};
    Sailor::u64 memAddr = reinterpret_cast<Sailor::u64>(memTarget);
    cmdbuf.push_back(MakeHeader(Sailor::Pm4::IT_WRITE_DATA, 4));
    cmdbuf.push_back(0); // control flags
    cmdbuf.push_back(static_cast<Sailor::u32>(memAddr & 0xFFFFFFFF));
    cmdbuf.push_back(static_cast<Sailor::u32>((memAddr >> 32) & 0xFFFFFFFF));
    cmdbuf.push_back(0x12345678);

    // 8. Packet: IT_RELEASE_MEM (EOP fence write)
    Sailor::u64 fenceTarget = 0;
    Sailor::u64 fenceAddr = reinterpret_cast<Sailor::u64>(&fenceTarget);
    cmdbuf.push_back(MakeHeader(Sailor::Pm4::IT_RELEASE_MEM, 6));
    cmdbuf.push_back(0); // event type
    cmdbuf.push_back(0); // data sel
    cmdbuf.push_back(static_cast<Sailor::u32>(fenceAddr & 0xFFFFFFFF));
    cmdbuf.push_back(static_cast<Sailor::u32>((fenceAddr >> 32) & 0xFFFFFFFF));
    cmdbuf.push_back(0x99887766); // fence value lo
    cmdbuf.push_back(0x00000001); // fence value hi (0x199887766ULL)

    bool decodeOk = decoder.DecodeBuffer(cmdbuf);
    assert(decodeOk);
    assert(decoder.GetContextReg(0x100) == 0xDEADBEEF);
    assert(drawAutoCalled);
    assert(drawIndexedCalled);
    assert(dispatchCalled);
    assert(memTarget[0] == 0x12345678);
    assert(fenceTarget == 0x199887766ULL);

    // 9. Packet: IT_DRAW_INDIRECT & IT_EVENT_WRITE_EOP
    bool drawIndirectCalled = false;
    decoder.SetDrawCallback([&](const Sailor::DrawCommand& cmd) {
        if (cmd.isIndirect) {
            drawIndirectCalled = true;
            assert(cmd.indirectBufferAddress == 0x140080000ULL);
        }
    });

    std::vector<Sailor::u32> indirectBuf;
    indirectBuf.push_back(MakeHeader(Sailor::Pm4::IT_DRAW_INDIRECT, 2));
    indirectBuf.push_back(0x40080000);
    indirectBuf.push_back(0x00000001);

    Sailor::u64 eventFence = 0;
    Sailor::u64 eventFenceAddr = reinterpret_cast<Sailor::u64>(&eventFence);
    indirectBuf.push_back(MakeHeader(Sailor::Pm4::IT_EVENT_WRITE_EOP, 5));
    indirectBuf.push_back(0); // event
    indirectBuf.push_back(static_cast<Sailor::u32>(eventFenceAddr & 0xFFFFFFFF));
    indirectBuf.push_back(static_cast<Sailor::u32>((eventFenceAddr >> 32) & 0xFFFFFFFF));
    indirectBuf.push_back(0x55AA1122);
    indirectBuf.push_back(0x00000002);

    assert(decoder.DecodeBuffer(indirectBuf));
    assert(drawIndirectCalled);
    assert(eventFence == 0x255AA1122ULL);

    // 10. Test AgcDriver SubmitCommandBuffer
    auto& agc = Sailor::AgcDriver::Instance();
    assert(agc.Initialize() == Sailor::SCE_OK);
    assert(agc.SubmitCommandBuffer(indirectBuf.data(), static_cast<Sailor::u32>(indirectBuf.size()), 0) == Sailor::SCE_OK);

    std::cout << "[TEST] PM4 Decoder tests passed!\n";
}

void TestVideoOutAndVulkan() {
    std::cout << "[TEST] Running VideoOut and Vulkan tests...\n";
    auto& vo = Sailor::VideoOutManager::Instance();

    Sailor::i32 handle = vo.Open(1, 0, 0, nullptr);
    assert(handle > 0);

    Sailor::VideoOutResolutionStatus resStatus{};
    Sailor::i32 res = vo.GetResolutionStatus(handle, &resStatus);
    assert(res == Sailor::SCE_OK);
    assert(resStatus.width == 1920);
    assert(resStatus.height == 1080);

    void* fakeBuffers[2] = { reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x2000) };
    Sailor::VideoOutBufferAttribute attr{};
    attr.width = 3840;
    attr.height = 2160;

    res = vo.RegisterBuffers(handle, 0, fakeBuffers, 2, &attr);
    assert(res == Sailor::SCE_OK);

    // Verify 4K update
    vo.GetResolutionStatus(handle, &resStatus);
    assert(resStatus.width == 3840);
    assert(resStatus.height == 2160);

    // Submit flip
    res = vo.SubmitFlip(handle, 0, 0, 0);
    assert(res == Sailor::SCE_OK);

    vo.Close(handle);

    // Test invalid handle checks
    assert(vo.Close(0) == Sailor::SCE_KERNEL_ERROR_EINVAL);
    assert(vo.Close(-1) == Sailor::SCE_KERNEL_ERROR_EINVAL);
    assert(vo.RegisterBuffers(-1, 0, fakeBuffers, 2, &attr) == Sailor::SCE_KERNEL_ERROR_EINVAL);
    assert(vo.SubmitFlip(-1, 0, 0, 0) == Sailor::SCE_KERNEL_ERROR_EINVAL);
    assert(vo.GetResolutionStatus(-1, &resStatus) == Sailor::SCE_KERNEL_ERROR_EINVAL);

    // Verify VulkanContext status
    auto& vk = Sailor::VulkanContext::Instance();
    if (vk.IsAvailable()) {
        std::cout << "[TEST] Vulkan 1.3 is ACTIVE on host GPU: " << vk.GetDeviceInfo().deviceName << "\n";
    } else {
        std::cout << "[TEST] Running in headless Vulkan fallback mode\n";
    }

    std::cout << "[TEST] VideoOut and Vulkan tests passed!\n";
}

void TestTextureDetiler() {
    std::cout << "[TEST] Running AMD RDNA2 / GFX10 Texture Detiler tests...\n";
    auto& detiler = Sailor::TextureDetiler::Instance();

    // 1. Test 64x64 RGBA8 Surface (4 bytes per pixel)
    Sailor::TextureSurfaceInfo info{};
    info.width = 64;
    info.height = 64;
    info.depth = 1;
    info.elementBytes = 4;
    info.swizzleMode = Sailor::TextureSwizzleMode::Sw64KB_R_X;
    info.pitchBytes = 64 * 4;

    std::vector<Sailor::u32> linearSrc(64 * 64);
    for (Sailor::u32 y = 0; y < 64; ++y) {
        for (Sailor::u32 x = 0; x < 64; ++x) {
            linearSrc[y * 64 + x] = (x << 16) | (y & 0xFFFF);
        }
    }

    std::vector<Sailor::u8> tiledBuf(64 * 1024, 0); // 64 KiB block
    assert(detiler.RetileSurface(linearSrc.data(), tiledBuf.data(), info));

    // Verify tiled buffer is swizzled (non-linear micro-tiled order)
    const Sailor::u32* tiledWords = reinterpret_cast<const Sailor::u32*>(tiledBuf.data());
    assert(tiledWords[4] != linearSrc[4]); // In tiled memory, index 4 holds pixel (0, 1), not (4, 0)!

    std::vector<Sailor::u32> restoredLinear(64 * 64, 0);
    assert(detiler.DetileSurface(tiledBuf.data(), restoredLinear.data(), info));

    // Bit-for-bit round-trip equality assertion
    for (size_t i = 0; i < 64 * 64; ++i) {
        assert(restoredLinear[i] == linearSrc[i]);
    }
    std::cout << "  -> Bit-for-bit detile round-trip verified for 64x64 RGBA8 surface!\n";

    // 2. Test 16-byte elements (BC7 / RGBA32F)
    Sailor::TextureSurfaceInfo info16{};
    info16.width = 32;
    info16.height = 32;
    info16.depth = 1;
    info16.elementBytes = 16;
    info16.swizzleMode = Sailor::TextureSwizzleMode::Sw64KB_R_X;
    info16.pitchBytes = 32 * 16;

    std::vector<Sailor::u8> linear16(32 * 32 * 16);
    for (size_t i = 0; i < linear16.size(); ++i) {
        linear16[i] = static_cast<Sailor::u8>((i * 7 + 13) & 0xFF);
    }

    std::vector<Sailor::u8> tiled16(64 * 1024, 0);
    assert(detiler.RetileSurface(linear16.data(), tiled16.data(), info16));

    std::vector<Sailor::u8> restored16(32 * 32 * 16, 0);
    assert(detiler.DetileSurface(tiled16.data(), restored16.data(), info16));
    assert(std::memcmp(linear16.data(), restored16.data(), linear16.size()) == 0);
    std::cout << "  -> Bit-for-bit detile round-trip verified for 32x32 BC7 / 16-byte surface!\n";

    // 3. Test Asynchronous & Multi-Core Parallel Detiling on 256x256 surface
    Sailor::TextureSurfaceInfo infoAsync{};
    infoAsync.width = 256;
    infoAsync.height = 256;
    infoAsync.depth = 1;
    infoAsync.elementBytes = 4;
    infoAsync.swizzleMode = Sailor::TextureSwizzleMode::Sw64KB_R_X;
    infoAsync.pitchBytes = 256 * 4;

    std::vector<Sailor::u32> linearAsync(256 * 256);
    for (Sailor::u32 y = 0; y < 256; ++y) {
        for (Sailor::u32 x = 0; x < 256; ++x) {
            linearAsync[y * 256 + x] = 0xAA000000u | (x << 8) | y;
        }
    }
    std::vector<Sailor::u8> tiledAsync(256 * 256 * 4, 0);
    auto retileFut = detiler.RetileSurfaceAsync(linearAsync.data(), tiledAsync.data(), infoAsync);
    assert(retileFut.get());

    std::vector<Sailor::u32> restoredAsync(256 * 256, 0);
    auto detileFut = detiler.DetileSurfaceAsync(tiledAsync.data(), restoredAsync.data(), infoAsync);
    assert(detileFut.get());

    for (size_t i = 0; i < 256 * 256; ++i) {
        assert(restoredAsync[i] == linearAsync[i]);
    }
    std::cout << "  -> Multi-threaded async detile verified for 256x256 RGBA8 surface!\n";

    std::cout << "[TEST] Texture Detiler tests passed!\n";
}

void TestVulkanPipelineManager() {
    std::cout << "[TEST] Running Vulkan 1.3 Pipeline & Descriptor Manager tests...\n";
    auto& pm = Sailor::VulkanPipelineManager::Instance();
    pm.Initialize();

    void* set = pm.AllocateDescriptorSet();
    if (set) {
        std::cout << "  -> Successfully allocated VkDescriptorSet {:p} from managed pool\n";
    }

    assert(pm.GetPipelineLayout() != nullptr);
    assert(pm.GetDescriptorSetLayout() != nullptr);

    std::cout << "[TEST] Vulkan Pipeline Manager tests passed!\n";
}

void TestAsyncComputeQueue() {
    std::cout << "[TEST] Running Async Compute Multi-Queue & ACB tests...\n";
    auto& vk = Sailor::VulkanContext::Instance();
    assert(vk.GetComputeQueue() != nullptr);

    if (vk.HasDedicatedComputeQueue()) {
        std::cout << "  -> Hardware dedicated async compute queue verified on family " << vk.GetComputeQueueFamily() << "\n";
    } else {
        std::cout << "  -> Async compute multiplexed queue verified on family " << vk.GetComputeQueueFamily() << "\n";
    }

    auto& agc = Sailor::AgcDriver::Instance();
    agc.Initialize();

    // Prepare an ACB compute dispatch command stream
    std::vector<Sailor::u32> acbStream;
    acbStream.push_back(MakeHeader(Sailor::Pm4::IT_DISPATCH_DIRECT, 3));
    acbStream.push_back(8); // threadsX
    acbStream.push_back(4); // threadsY
    acbStream.push_back(1); // threadsZ

    Sailor::u64 computeFence = 0;
    Sailor::u64 fenceAddr = reinterpret_cast<Sailor::u64>(&computeFence);
    acbStream.push_back(MakeHeader(Sailor::Pm4::IT_RELEASE_MEM, 6));
    acbStream.push_back(0); // event
    acbStream.push_back(0); // data sel
    acbStream.push_back(static_cast<Sailor::u32>(fenceAddr & 0xFFFFFFFF));
    acbStream.push_back(static_cast<Sailor::u32>((fenceAddr >> 32) & 0xFFFFFFFF));
    acbStream.push_back(0xCAFEBABE);
    acbStream.push_back(0);

    Sailor::i32 res = agc.SubmitAcb(acbStream.data(), static_cast<Sailor::u32>(acbStream.size()), 1);
    assert(res == Sailor::SCE_OK);
    assert(computeFence == 0xCAFEBABEULL);
    std::cout << "  -> Async Compute Buffer (ACB) PM4 stream successfully dispatched & fence verified!\n";

    // Wait idle
    vk.WaitComputeIdle();
    std::cout << "[TEST] Async Compute tests passed!\n";
}

int main() {
    std::cout << "=== SailorBox Phase 3 GPU Test Suite ===\n";
    TestPm4Decoder();
    TestVideoOutAndVulkan();
    TestTextureDetiler();
    TestVulkanPipelineManager();
    TestAsyncComputeQueue();
    std::cout << "=== All Phase 3 Tests PASSED Successfully! ===\n";
    return 0;
}
