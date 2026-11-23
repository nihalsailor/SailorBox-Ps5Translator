#include "gpu/AgcDriver.hpp"
#include "gpu/VideoOut.hpp"
#include "gpu/VulkanContext.hpp"
#include "common/Logger.hpp"
#include "common/SceErrors.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sailor {

struct CommandBuffer {
    using Callback = bool (SAILOR_SYSV_ABI *)(CommandBuffer*, u32, void*);
    u32* bottom;
    u32* top;
    u32* cursor_up;
    u32* cursor_down;
    Callback callback;
    void* user_data;
    u32 reserved_dw;
};

struct AgcPacket {
    const u32* addr;
    u32 dw_num;
    u8 flags;
    u8 reserved[3];
};

struct ShaderRegister {
    u32 offset;
    u32 value;
};

struct SizeAlign {
    u64 m_size;
    u64 m_align;
};

struct SceAgcMemoryRange {
    void* base;
    u64 size;
};

} // namespace Sailor

static inline Sailor::u32* AllocateCommand(Sailor::CommandBuffer* buf, Sailor::u32 count) {
    if (!buf) return nullptr;
    if (buf->top && buf->cursor_up && (buf->top - buf->cursor_up < static_cast<std::ptrdiff_t>(count + buf->reserved_dw))) {
        if (buf->callback) {
            buf->callback(buf, count + buf->reserved_dw, buf->user_data);
        }
    }
    if (!buf->cursor_up) return nullptr;
    Sailor::u32* res = buf->cursor_up;
    buf->cursor_up += count;
    return res;
}

static inline Sailor::u32* EmitCommand(Sailor::CommandBuffer* buf, Sailor::u32 opcode, std::initializer_list<Sailor::u32> payload) {
    Sailor::u32 count = static_cast<Sailor::u32>(payload.size()) + 1u;
    Sailor::u32* packet = AllocateCommand(buf, count);
    if (!packet) return nullptr;
    packet[0] = 0xC0000000u | ((count - 2u) << 16u) | (opcode << 8u);
    std::copy(payload.begin(), payload.end(), packet + 1);
    return packet;
}

static inline Sailor::u32* WriteNop(Sailor::CommandBuffer* buf, Sailor::u32 count) {
    Sailor::u32* packet = AllocateCommand(buf, count);
    if (!packet) return nullptr;
    packet[0] = (count == 1) ? 0xffff1000u : (0xC0000000u | ((count - 2u) << 16u) | (0x10u << 8u));
    std::fill_n(packet + 1, count - 1u, 0u);
    return packet;
}

static Sailor::u32 s_registerDefaults[256] = {0};

extern "C" {

// ==========================================
// libSceVideoOut Exports
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutOpen(
    Sailor::i32 userId,
    Sailor::i32 type,
    Sailor::i32 index,
    const void* param) {
    return Sailor::VideoOutManager::Instance().Open(userId, type, index, param);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutClose(Sailor::i32 handle) {
    return Sailor::VideoOutManager::Instance().Close(handle);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutRegisterBuffers(
    Sailor::i32 handle,
    Sailor::i32 startIndex,
    void* const* addresses,
    Sailor::u32 bufferNum,
    const Sailor::VideoOutBufferAttribute* attr) {
    return Sailor::VideoOutManager::Instance().RegisterBuffers(handle, startIndex, addresses, bufferNum, attr);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutRegisterBuffers2(
    Sailor::i32 handle,
    Sailor::i32 startIndex,
    uintptr_t arg3,
    uintptr_t arg4,
    uintptr_t arg5,
    uintptr_t arg6,
    uintptr_t arg7,
    uintptr_t arg8) {
    (void)arg7;
    (void)arg8;

    void* const* addresses = nullptr;
    Sailor::u32 bufferNum = 0;
    const Sailor::VideoOutBufferAttribute* attr = nullptr;

    // Detect if arg3 is 'reserved/attributeIndex' (small value < 0x10000) or 'addresses' pointer
    if (arg3 < 0x10000) {
        addresses = reinterpret_cast<void* const*>(arg4);
        bufferNum = static_cast<Sailor::u32>(arg5);
        attr = reinterpret_cast<const Sailor::VideoOutBufferAttribute*>(arg6);
    } else {
        addresses = reinterpret_cast<void* const*>(arg3);
        bufferNum = static_cast<Sailor::u32>(arg4);
        attr = reinterpret_cast<const Sailor::VideoOutBufferAttribute*>(arg5);
    }

    SB_LOG_INFO("VideoOut", "sceVideoOutRegisterBuffers2 called: handle={}, startIndex={}, arg3=0x{:X}, addresses={:p}, bufferNum={}, attr={:p}",
        handle, startIndex, arg3, (void*)addresses, bufferNum, (void*)attr);

    return Sailor::VideoOutManager::Instance().RegisterBuffers(handle, startIndex, addresses, bufferNum, attr);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutSetBufferAttribute2(
    Sailor::VideoOutBufferAttribute* attr,
    Sailor::u64 pixelFormat,
    Sailor::u32 tilingMode,
    Sailor::u32 widthOrAspect,
    Sailor::u32 heightOrWidth,
    Sailor::u32 pitchOrHeight,
    Sailor::u32 optionalPitch) {
    if (!attr) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    attr->pixelFormat = static_cast<Sailor::u32>(pixelFormat);
    attr->tilingMode = tilingMode;
    if (widthOrAspect >= 128) {
        attr->width = widthOrAspect;
        attr->height = heightOrWidth ? heightOrWidth : 1080;
        attr->pitchInPixels = pitchOrHeight ? pitchOrHeight : attr->width;
    } else {
        attr->aspectRatio = widthOrAspect;
        attr->width = heightOrWidth ? heightOrWidth : 1920;
        attr->height = pitchOrHeight ? pitchOrHeight : 1080;
        attr->pitchInPixels = optionalPitch ? optionalPitch : attr->width;
    }
    SB_LOG_INFO("VideoOut", "sceVideoOutSetBufferAttribute2: res={}x{}, pitch={}, format=0x{:X}, tiling={}",
        attr->width, attr->height, attr->pitchInPixels, attr->pixelFormat, attr->tilingMode);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutAddFlipEvent(
    void* eq,
    Sailor::i32 handle,
    void* udata) {
    return Sailor::VideoOutManager::Instance().AddFlipEvent(eq, handle, udata);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutSubmitFlip(
    Sailor::i32 handle,
    Sailor::i32 bufferIndex,
    Sailor::u32 flipMode,
    Sailor::i64 flipArg) {
    return Sailor::VideoOutManager::Instance().SubmitFlip(handle, bufferIndex, flipMode, flipArg);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutGetResolutionStatus(
    Sailor::i32 handle,
    Sailor::VideoOutResolutionStatus* status) {
    return Sailor::VideoOutManager::Instance().GetResolutionStatus(handle, status);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutSetFlipRate(Sailor::i32 handle, Sailor::u32 rate) {
    (void)handle;
    (void)rate;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceVideoOutRegisterFlipEventQueue(
    Sailor::i32 handle,
    void* eq) {
    return Sailor::VideoOutManager::Instance().AddFlipEvent(eq, handle, nullptr);
}

// ==========================================
// libSceAgcDriver Submissions & Core Exports
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSubmitDcb(const Sailor::AgcPacket* packet) {
    if (!packet) return Sailor::SCE_OK;
    SB_LOG_INFO("AgcExports", "sceAgcDriverSubmitDcb: addr={:p}, dwords={}", static_cast<const void*>(packet->addr), packet->dw_num);
    if (!packet->addr || packet->dw_num == 0) return Sailor::SCE_OK;
    return Sailor::AgcDriver::Instance().SubmitCommandBuffer(packet->addr, packet->dw_num, 0);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverAgrSubmitDcb(const Sailor::AgcPacket* packet) {
    return sceAgcDriverSubmitDcb(packet);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSubmitMultiDcbs(
    const Sailor::u32* const* dcbGpuAddrs,
    const Sailor::u32* dcbSizesInDwords,
    Sailor::u32 count) {
    SB_LOG_INFO("AgcExports", "sceAgcDriverSubmitMultiDcbs: count={}", count);
    if (!dcbGpuAddrs || !dcbSizesInDwords || count == 0) return Sailor::SCE_OK;
    for (Sailor::u32 i = 0; i < count; ++i) {
        if (dcbGpuAddrs[i] && dcbSizesInDwords[i] > 0) {
            Sailor::AgcDriver::Instance().SubmitCommandBuffer(dcbGpuAddrs[i], dcbSizesInDwords[i], 0);
        }
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverAgrSubmitMultiDcbs(
    const Sailor::u32* const* dcbGpuAddrs,
    const Sailor::u32* dcbSizesInDwords,
    Sailor::u32 count) {
    return sceAgcDriverSubmitMultiDcbs(dcbGpuAddrs, dcbSizesInDwords, count);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSubmitAcb(Sailor::u32 queue, const Sailor::AgcPacket* packet) {
    if (!packet || !packet->addr || packet->dw_num == 0) return Sailor::SCE_OK;
    return Sailor::AgcDriver::Instance().SubmitAcb(packet->addr, packet->dw_num, queue);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSubmitMultiAcbs(
    Sailor::u32 queue,
    const Sailor::u32* const* acbs,
    const Sailor::u32* sizesInDwords,
    Sailor::u32 count) {
    if (!acbs || !sizesInDwords || count == 0) return Sailor::SCE_OK;
    for (Sailor::u32 i = 0; i < count; ++i) {
        if (acbs[i] && sizesInDwords[i] > 0) {
            Sailor::AgcDriver::Instance().SubmitAcb(acbs[i], sizesInDwords[i], queue);
        }
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSubmitCommandBuffer(void* queueContext, const Sailor::AgcPacket* packet) {
    (void)queueContext;
    return sceAgcDriverSubmitDcb(packet);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSubmitMultiCommandBuffers(
    void* queueContext,
    const Sailor::u32* const* commandBuffers,
    const Sailor::u32* sizesInDwords,
    Sailor::u32 count) {
    (void)queueContext;
    return sceAgcDriverSubmitMultiDcbs(commandBuffers, sizesInDwords, count);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDriverGetWaitRenderingPacketSizeInDwords() {
    return 4;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDriverGetSetFlipPacketSizeInDwords() {
    return 6;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDriverWaitUntilSafeForRendering(
    Sailor::u32** command,
    Sailor::u32 capacity,
    Sailor::u32 mode,
    Sailor::u32 handle,
    Sailor::i32 index) {
    if (command && *command && capacity >= 4) {
        (*command)[0] = 0xC0021018u;
        (*command)[1] = handle;
        (*command)[2] = static_cast<Sailor::u32>(index);
        (*command)[3] = mode;
        *command += 4;
    }
    return 0;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverClose(Sailor::i32 handle) {
    (void)handle;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverAddEqEvent(void* eq, Sailor::i32 id, void* udata) {
    (void)eq; (void)id; (void)udata;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverDeleteEqEvent(void* eq, Sailor::i32 id) {
    (void)eq; (void)id;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetEqEventType(const void* ev) {
    if (!ev) return 0;
    const Sailor::u64* p = reinterpret_cast<const Sailor::u64*>(ev);
    return static_cast<Sailor::i32>(p[2]);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDriverGetEqContextId(const void* ev) {
    if (!ev) return 0;
    const Sailor::u64* p = reinterpret_cast<const Sailor::u64*>(ev);
    return static_cast<Sailor::u32>(p[2] >> 16u);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverTriggerCapture() { return Sailor::SCE_OK; }
SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverRequestCaptureStart() { return Sailor::SCE_OK; }
SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverRequestCaptureStop() { return Sailor::SCE_OK; }
SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverIsCaptureInProgress() { return 0; }
SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverIsSubmitValidationEnabled() { return 0; }
SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverIsTraceInProgress() { return 0; }
SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetShaderDebuggingStatus() { return 1; }

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverRegisterWorkloadStream(Sailor::u32 streamId, const void* stream) {
    (void)streamId; (void)stream;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverUnregisterWorkloadStream(Sailor::u32 streamId) {
    (void)streamId;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverQueryResourceRegistrationUserMemoryRequirements(void* reqs) {
    (void)reqs;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverInitResourceRegistration(void* opt) {
    (void)opt;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetResourceRegistrationMaxNameLength(Sailor::u32* maxLen) {
    if (maxLen) *maxLen = 64;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverRegisterOwner(Sailor::u32* owner, const char* name) {
    if (owner) *owner = 1;
    (void)name;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverRegisterResource(
    Sailor::u32* handle, Sailor::u32 owner, const void* mem, size_t sz, const char* name, Sailor::u32 type, Sailor::u64 udata) {
    if (handle) *handle = 1;
    (void)owner; (void)mem; (void)sz; (void)name; (void)type; (void)udata;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverRegisterGdsResource(
    Sailor::u32* handle, Sailor::u32 owner, Sailor::u32 off, Sailor::u32 sz, const char* name, Sailor::u32 type, Sailor::u64 udata) {
    if (handle) *handle = 1;
    (void)owner; (void)off; (void)sz; (void)name; (void)type; (void)udata;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverUnregisterResource(Sailor::u32 handle) {
    (void)handle;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverUnregisterOwnerAndResources(Sailor::u32 owner) {
    (void)owner;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverUnregisterAllResourcesForOwner(Sailor::u32 owner) {
    (void)owner;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverRegisterDefaultOwner(Sailor::u32 owner) {
    (void)owner;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDriverGetDefaultOwner() {
    return 1;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverFindResourcesPublic(const void* cb, void* udata) {
    (void)cb; (void)udata;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetResourceName(Sailor::u32 handle, const char** name) {
    (void)handle;
    if (name) *name = "AgcResource";
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetResourceType(Sailor::u32 handle, Sailor::u32* type) {
    (void)handle;
    if (type) *type = 0;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetResourceBaseAddressAndSizeInBytes(Sailor::u32 handle, void** mem, size_t* sz) {
    (void)handle;
    if (mem) *mem = nullptr;
    if (sz) *sz = 0;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetResourceUserData(Sailor::u32 handle, Sailor::u64* udata) {
    (void)handle;
    if (udata) *udata = 0;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSetResourceUserData(Sailor::u32 handle, Sailor::u64 udata) {
    (void)handle; (void)udata;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetResourceShaderGuid(Sailor::u32 handle, void* guid) {
    (void)handle;
    if (guid) std::memset(guid, 0, 16);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverGetOwnerName(Sailor::u32 owner, const char** name) {
    (void)owner;
    if (name) *name = "AgcOwner";
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSetTFRing(void* param) {
    (void)param;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDriverSetHsOffchipParam(void* param) {
    (void)param;
    return Sailor::SCE_OK;
}

// ==========================================
// libSceAgc Core & Shader / State Exports
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcInit_0090(Sailor::u32* state, Sailor::u32 version) {
    (void)state; (void)version;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT void* SAILOR_SYSV_ABI sceAgcGetRegisterDefaults2(Sailor::u32 version) {
    (void)version;
    return s_registerDefaults;
}

SAILOR_EXPORT void* SAILOR_SYSV_ABI sceAgcGetRegisterDefaults2Internal(Sailor::u32 version) {
    (void)version;
    return s_registerDefaults;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSuspendPoint() {
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcGetDefaultCxStateFlat(void* outState, Sailor::u32 size) {
    if (outState && size > 0) std::memset(outState, 0, size);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDebugRaiseException(Sailor::u32 exceptionId) {
    (void)exceptionId;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcCreateShader(void** dst, void* header, const void* code) {
    (void)code;
    if (dst) *dst = header;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcGetFusedShaderSize_0080(Sailor::SizeAlign* dst, const void* front, const void* back) {
    (void)front; (void)back;
    if (dst) { dst->m_size = 256; dst->m_align = 4; }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcFuseShaderHalves_0200(void* fusedResult, const void* front, const void* back, void* scratchMem) {
    (void)back; (void)scratchMem;
    if (fusedResult && front) std::memcpy(fusedResult, front, 128);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcCreateInterpolantMapping_0100(void* regs, const void* gs, const void* ps) {
    (void)regs; (void)gs; (void)ps;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcUpdateInterpolantMapping_0100(void* regs, const void* gs, const void* ps) {
    (void)regs; (void)gs; (void)ps;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcCreatePrimState(void* cxRegs, void* ucRegs, const void* hs, const void* gs, Sailor::u32 primType) {
    (void)cxRegs; (void)ucRegs; (void)hs; (void)gs; (void)primType;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcUpdatePrimState(void* cxRegs, void* ucRegs, const void* hs, const void* gs, Sailor::u32 primType) {
    (void)cxRegs; (void)ucRegs; (void)hs; (void)gs; (void)primType;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcGetGsOversubscription(void* regs, const void* gs, Sailor::u32 p3, Sailor::u32 p4) {
    (void)regs; (void)gs; (void)p3; (void)p4;
    return Sailor::SCE_OK;
}

// ==========================================
// libSceAgc Command Buffer (CB) Exports
// ==========================================

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbBranch(
    Sailor::CommandBuffer* buf, Sailor::u8 mode, Sailor::u8 cmpFn, const Sailor::u64* cmpAddr,
    Sailor::u64 mask, Sailor::u64 ref, Sailor::u8 cp1, const Sailor::u32* b1, Sailor::u32 sz1,
    Sailor::u8 cp2, const Sailor::u32* b2, Sailor::u32 sz2) {
    Sailor::u64 addr = cmpAddr ? *cmpAddr : 0;
    Sailor::u64 f = reinterpret_cast<Sailor::u64>(b1);
    Sailor::u64 s = reinterpret_cast<Sailor::u64>(b2);
    return EmitCommand(buf, 0x3Fu, {
        static_cast<Sailor::u32>(mode | (cmpFn << 8u)),
        static_cast<Sailor::u32>(addr), static_cast<Sailor::u32>(addr >> 32u),
        static_cast<Sailor::u32>(mask), static_cast<Sailor::u32>(mask >> 32u),
        static_cast<Sailor::u32>(ref), static_cast<Sailor::u32>(ref >> 32u),
        static_cast<Sailor::u32>(f), static_cast<Sailor::u32>(f >> 32u),
        sz1 | (static_cast<Sailor::u32>(cp1) << 28u),
        static_cast<Sailor::u32>(s), static_cast<Sailor::u32>(s >> 32u),
        sz2 | (static_cast<Sailor::u32>(cp2) << 28u)
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbBranchGetSize() { return 56; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbDispatch(
    Sailor::CommandBuffer* buf, Sailor::u32 tgX, Sailor::u32 tgY, Sailor::u32 tgZ, Sailor::u32 mod) {
    return EmitCommand(buf, 0x15u, {tgX, tgY, tgZ, mod | 0x41u});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbDispatchGetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbCondWrite(
    Sailor::CommandBuffer* buf, Sailor::u32 cmpFn, Sailor::u32 wrSpace, const void* wrAddr,
    Sailor::u32 wrData, const void* pollAddr, Sailor::u32 ref, Sailor::u32 mask) {
    Sailor::u64 dst = reinterpret_cast<Sailor::u64>(wrAddr);
    Sailor::u64 src = reinterpret_cast<Sailor::u64>(pollAddr);
    return EmitCommand(buf, 0x45u, {
        0x10u | cmpFn | (wrSpace << 8u),
        static_cast<Sailor::u32>(src), static_cast<Sailor::u32>(src >> 32u),
        ref, mask,
        static_cast<Sailor::u32>(dst), static_cast<Sailor::u32>(dst >> 32u),
        wrData
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbCondWriteGetSize() { return 36; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbReleaseMem(
    Sailor::CommandBuffer* buf, Sailor::u8 action, Sailor::u16 gcrCtrl, Sailor::u8 dst,
    Sailor::u8 cp, const void* addr, Sailor::u8 dataSel, Sailor::u64 data,
    Sailor::u16 gdsOff, Sailor::u16 gdsSz, Sailor::u8 intr, Sailor::u32 intrCtxId) {
    (void)gdsOff; (void)gdsSz;
    Sailor::u64 guestAddr = reinterpret_cast<Sailor::u64>(addr);
    Sailor::u32 evIdx = (action >= 0x2Fu) ? 6u : 5u;
    return EmitCommand(buf, 0x49u, {
        action | (evIdx << 8u) | (static_cast<Sailor::u32>(gcrCtrl) << 12u) | (static_cast<Sailor::u32>(cp) << 25u),
        (static_cast<Sailor::u32>(dst) << 16u) | (static_cast<Sailor::u32>(intr) << 24u) | (static_cast<Sailor::u32>(dataSel) << 29u),
        static_cast<Sailor::u32>(guestAddr), static_cast<Sailor::u32>(guestAddr >> 32u),
        static_cast<Sailor::u32>(data), static_cast<Sailor::u32>(data >> 32u),
        intrCtxId
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbNopGetSize(Sailor::u32 sizeInDwords) {
    return sizeInDwords * 4u;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbQueueEndOfPipeActionGetSize() { return 32; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbSetShRegisterRangeDirect(
    Sailor::CommandBuffer* buf, Sailor::u32 offset, const Sailor::u32* values, Sailor::u32 numValues) {
    Sailor::u32 count = numValues + 2u;
    Sailor::u32* pkt = AllocateCommand(buf, count);
    if (!pkt) return nullptr;
    pkt[0] = 0xC0000000u | ((count - 2u) << 16u) | (0x76u << 8u);
    pkt[1] = offset;
    if (values && numValues > 0) std::memcpy(pkt + 2, values, numValues * sizeof(Sailor::u32));
    return pkt;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbSetShRegisterRangeDirectGetSize(Sailor::u32 numValues) {
    return (numValues + 2u) * 4u;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbSetUcRegisterRangeDirect(
    Sailor::CommandBuffer* buf, Sailor::u32 offset, const Sailor::u32* values, Sailor::u32 numValues) {
    Sailor::u32 count = numValues + 2u;
    Sailor::u32* pkt = AllocateCommand(buf, count);
    if (!pkt) return nullptr;
    pkt[0] = 0xC0000000u | ((count - 2u) << 16u) | (0x79u << 8u);
    pkt[1] = offset;
    if (values && numValues > 0) std::memcpy(pkt + 2, values, numValues * sizeof(Sailor::u32));
    return pkt;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbSetUcRegisterRangeDirectGetSize(Sailor::u32 numValues) {
    return (numValues + 2u) * 4u;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbSetShRegistersDirect(
    Sailor::CommandBuffer* buf, const void* regs, Sailor::u32 numRegs) {
    const Sailor::ShaderRegister* r = static_cast<const Sailor::ShaderRegister*>(regs);
    Sailor::u32* first = nullptr;
    for (Sailor::u32 i = 0; i < numRegs; ++i) {
        Sailor::u32* p = EmitCommand(buf, 0x76u, {r[i].offset, r[i].value});
        if (!first) first = p;
    }
    return first;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbSetShRegistersDirectGetSize(Sailor::u32 numRegs) {
    return numRegs * 12u;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcCbSetUcRegistersDirect(
    Sailor::CommandBuffer* buf, const void* regs, Sailor::u32 numRegs) {
    const Sailor::ShaderRegister* r = static_cast<const Sailor::ShaderRegister*>(regs);
    Sailor::u32* first = nullptr;
    for (Sailor::u32 i = 0; i < numRegs; ++i) {
        Sailor::u32* p = EmitCommand(buf, 0x79u, {r[i].offset, r[i].value});
        if (!first) first = p;
    }
    return first;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcCbSetUcRegistersDirectGetSize(Sailor::u32 numRegs) {
    return numRegs * 12u;
}

// ==========================================
// libSceAgc Async Compute Buffer (ACB) Exports
// ==========================================

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbAtomicMem(
    Sailor::CommandBuffer* buf, Sailor::u8 op, Sailor::u8 cmd, Sailor::u8 cp,
    const void* addr, Sailor::u64 src, Sailor::u64 cmp, Sailor::u16 loop) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x1Eu, {
        static_cast<Sailor::u32>(op | (cmd << 8u) | (cp << 16u)),
        static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u),
        static_cast<Sailor::u32>(src), static_cast<Sailor::u32>(src >> 32u),
        static_cast<Sailor::u32>(cmp), static_cast<Sailor::u32>(cmp >> 32u),
        loop
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbAtomicMemGetSize() { return 36; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbAcquireMem(
    Sailor::CommandBuffer* buf, Sailor::u32 gcrCtrl, const void* base, Sailor::u64 sz, Sailor::u32 poll) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(base);
    return EmitCommand(buf, 0x58u, {
        0x80000000u, static_cast<Sailor::u32>(sz >> 8u), 0,
        static_cast<Sailor::u32>(a >> 8u), 0, poll & 0xFFFFu, gcrCtrl
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbAcquireMemGetSize() { return 32; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbCondExec(
    Sailor::CommandBuffer* buf, const Sailor::u32* addr, Sailor::u32 numDwords) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x22u, {
        static_cast<Sailor::u32>(a & ~3u), static_cast<Sailor::u32>(a >> 32u), 0, numDwords & 0x3FFFu
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbCondExecGetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbCopyData(
    Sailor::CommandBuffer* buf, Sailor::u8 dst, Sailor::u8 dstCp, Sailor::u64 dstAddr,
    Sailor::u8 src, Sailor::u8 srcCp, Sailor::u64 srcAddr, Sailor::u8 itemSz, Sailor::u8 wrConfirm) {
    (void)itemSz; (void)wrConfirm;
    return EmitCommand(buf, 0x40u, {
        static_cast<Sailor::u32>(src | (srcCp << 8u) | (dst << 16u) | (dstCp << 24u)),
        static_cast<Sailor::u32>(srcAddr), static_cast<Sailor::u32>(srcAddr >> 32u),
        static_cast<Sailor::u32>(dstAddr), static_cast<Sailor::u32>(dstAddr >> 32u)
    });
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceAgcAcbCopyDataGetSize() { return 24; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbDispatchIndirect(
    Sailor::CommandBuffer* buf, const void* args, Sailor::u32 mod) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(args);
    return EmitCommand(buf, 0x16u, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), mod | 0x41u});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbDispatchIndirectGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbDmaData(
    Sailor::CommandBuffer* buf, Sailor::u8 dst, Sailor::u8 dstCp, Sailor::u64 dstAddr,
    Sailor::u8 src, Sailor::u8 srcCp, Sailor::u64 srcAddr, Sailor::u32 numBytes,
    Sailor::u8 waitForPrev, Sailor::u8 wrConfirm) {
    (void)waitForPrev; (void)wrConfirm;
    return EmitCommand(buf, 0x50u, {
        static_cast<Sailor::u32>(src | (srcCp << 8u) | (dst << 16u) | (dstCp << 24u)),
        static_cast<Sailor::u32>(srcAddr), static_cast<Sailor::u32>(srcAddr >> 32u),
        static_cast<Sailor::u32>(dstAddr), static_cast<Sailor::u32>(dstAddr >> 32u),
        numBytes
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbDmaDataGetSize() { return 28; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbEventWrite(
    Sailor::CommandBuffer* buf, Sailor::u8 eventType, const void* addr) {
    (void)addr;
    return EmitCommand(buf, 0x46u, {eventType | (eventType == 7 ? 0x400u : 0u)});
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceAgcAcbEventWriteGetSize(Sailor::u8 eventType) {
    (void)eventType;
    return 8;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbJump(
    Sailor::CommandBuffer* buf, Sailor::u8 cp, const Sailor::u32* target, Sailor::u32 sz) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(target);
    return EmitCommand(buf, 0x3Cu, {
        static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), sz | (static_cast<Sailor::u32>(cp) << 28u)
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbJumpGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbWriteData(
    Sailor::CommandBuffer* buf, Sailor::u8 dst, Sailor::u8 cp, Sailor::u64 addr,
    const void* data, Sailor::u32 numDwords, Sailor::u8 inc, Sailor::u8 wrConfirm) {
    (void)cp; (void)inc; (void)wrConfirm;
    Sailor::u32 count = numDwords + 4u;
    Sailor::u32* pkt = AllocateCommand(buf, count);
    if (!pkt) return nullptr;
    pkt[0] = 0xC0000000u | ((count - 2u) << 16u) | (0x37u << 8u);
    pkt[1] = (dst << 8u) | 0x06010000u;
    pkt[2] = static_cast<Sailor::u32>(addr);
    pkt[3] = static_cast<Sailor::u32>(addr >> 32u);
    if (data && numDwords > 0) std::memcpy(pkt + 4, data, numDwords * sizeof(Sailor::u32));
    return pkt;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbWaitRegMem(
    Sailor::CommandBuffer* buf, Sailor::u8 sz, Sailor::u8 cmpFn, Sailor::u8 cp,
    const void* addr, Sailor::u64 ref, Sailor::u64 mask, Sailor::u32 poll) {
    (void)sz; (void)cp; (void)poll;
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x3Cu, {
        cmpFn, static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u),
        static_cast<Sailor::u32>(ref), static_cast<Sailor::u32>(mask)
    });
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbWaitUntilSafeForRendering(
    Sailor::CommandBuffer* buf, Sailor::u32 handle, Sailor::u32 index) {
    Sailor::u32* pkt = AllocateCommand(buf, 4);
    if (!pkt) return nullptr;
    pkt[0] = 0xC0021018u;
    pkt[1] = handle;
    pkt[2] = index;
    pkt[3] = 0;
    return pkt;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbRewind(Sailor::CommandBuffer* buf, Sailor::u32 state) {
    return EmitCommand(buf, 0x10u, {state});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbRewindGetSize() { return 8; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbResetQueue(Sailor::CommandBuffer* buf, Sailor::u32 op, Sailor::u32 val) {
    (void)val;
    return EmitCommand(buf, 0x10u, {op});
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbPrimeUtcl2(Sailor::CommandBuffer* buf, const void* addr, Sailor::u32 sz) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x10u, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), sz, 0});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbPrimeUtcl2GetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbAtomicGds_0900(
    Sailor::CommandBuffer* buf, Sailor::u32 ctrl, Sailor::u32 mode, Sailor::u32 gdsOp,
    Sailor::u32 src, Sailor::u16 off, Sailor::u16 idx, Sailor::u32 mask,
    Sailor::u32 data, Sailor::u64 cmp, Sailor::u64 extra) {
    (void)ctrl; (void)mode; (void)gdsOp; (void)src; (void)off; (void)idx; (void)mask; (void)data; (void)cmp; (void)extra;
    return WriteNop(buf, 11);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbAtomicGdsGetSize() { return 44; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbSetMarker(
    Sailor::CommandBuffer* buf, const char* name, Sailor::u32 nameLen, Sailor::u32 color) {
    (void)name; (void)nameLen; (void)color;
    return WriteNop(buf, 4);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbPushMarker(
    Sailor::CommandBuffer* buf, const char* name, Sailor::u32 nameLen, Sailor::u32 color) {
    (void)name; (void)nameLen; (void)color;
    return WriteNop(buf, 4);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbPopMarker(Sailor::CommandBuffer* buf) {
    return WriteNop(buf, 2);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbSetFlip(
    Sailor::CommandBuffer* buf, Sailor::u32 handle, Sailor::i32 index, Sailor::u32 mode, Sailor::i64 flipArg) {
    Sailor::u32* pkt = AllocateCommand(buf, 6);
    if (!pkt) return nullptr;
    pkt[0] = 0xC004105Cu;
    pkt[1] = handle;
    pkt[2] = static_cast<Sailor::u32>(index);
    pkt[3] = mode;
    pkt[4] = static_cast<Sailor::u32>(static_cast<Sailor::u64>(flipArg));
    pkt[5] = static_cast<Sailor::u32>(static_cast<Sailor::u64>(flipArg) >> 32u);
    return pkt;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbMemSemaphore(
    Sailor::CommandBuffer* buf, const void* addr, Sailor::u8 waitMailbox, Sailor::u8 sigType, Sailor::u8 op) {
    (void)waitMailbox; (void)sigType; (void)op;
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x39u, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), 0x80100000u});
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbSetWorkloadsActive(Sailor::CommandBuffer* buf, Sailor::u32 count, const Sailor::u32* ids) {
    (void)count; (void)ids;
    return WriteNop(buf, 18);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbSetWorkloadComplete(Sailor::CommandBuffer* buf, Sailor::u32 id) {
    (void)id;
    return WriteNop(buf, 12);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcAcbSetWorkloadStreamInactive(Sailor::CommandBuffer* buf, Sailor::u32 streamId) {
    (void)streamId;
    return WriteNop(buf, 9);
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceAgcAcbWaitOnAddressGetSize(Sailor::u32 size) {
    return (size == 0) ? 56 : ((size == 1) ? 64 : 0);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcAcbQueueEndOfShaderActionGetSize() {
    return 32;
}

// ==========================================
// libSceAgc Direct Command Buffer (DCB) Exports
// ==========================================

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbAcquireMem(
    Sailor::CommandBuffer* buf, Sailor::u8 engine, Sailor::u32 cbDbOp, Sailor::u32 gcrCtrl,
    const void* base, Sailor::u64 sz, Sailor::u32 poll) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(base);
    return EmitCommand(buf, 0x58u, {
        (static_cast<Sailor::u32>(engine) << 31u) | cbDbOp,
        static_cast<Sailor::u32>(sz >> 8u), 0,
        static_cast<Sailor::u32>(a >> 8u), 0, poll & 0xFFFFu, gcrCtrl
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbAcquireMemGetSize() { return 32; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbAtomicGds_0900(
    Sailor::CommandBuffer* buf, Sailor::u32 engine, Sailor::u32 ctrl, Sailor::u32 mode,
    Sailor::u32 gdsOp, Sailor::u32 src, Sailor::u16 off, Sailor::u16 idx,
    Sailor::u32 mask, Sailor::u32 data, Sailor::u64 cmp, Sailor::u64 extra) {
    (void)engine; (void)ctrl; (void)mode; (void)gdsOp; (void)src; (void)off; (void)idx; (void)mask; (void)data; (void)cmp; (void)extra;
    return WriteNop(buf, 11);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbAtomicGdsGetSize() { return 44; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbAtomicMem(
    Sailor::CommandBuffer* buf, Sailor::u8 op, Sailor::u8 cmd, Sailor::u8 cp,
    const void* addr, Sailor::u64 src, Sailor::u64 cmp, Sailor::u16 loop) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x1Eu, {
        static_cast<Sailor::u32>(op | (cmd << 8u) | (cp << 16u)),
        static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u),
        static_cast<Sailor::u32>(src), static_cast<Sailor::u32>(src >> 32u),
        static_cast<Sailor::u32>(cmp), static_cast<Sailor::u32>(cmp >> 32u),
        loop
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbAtomicMemGetSize() { return 36; }

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbBeginOcclusionQueryGetSize(Sailor::i32 queryType) {
    return (queryType == 1) ? 0x120u : ((queryType == 0) ? 0x10u : 0u);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbCondExec(
    Sailor::CommandBuffer* buf, const Sailor::u32* addr, Sailor::u32 numDwords) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x22u, {
        static_cast<Sailor::u32>(a & ~3u), static_cast<Sailor::u32>(a >> 32u), 0, numDwords & 0x3FFFu
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbCondExecGetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbContextStateOp_0100(Sailor::CommandBuffer* buf, Sailor::u32 op) {
    Sailor::u32 count = (op == 0) ? 5u : ((op == 3) ? 32u : 27u);
    Sailor::u32* pkt = WriteNop(buf, count);
    if (pkt) {
        pkt[0] = 0xC0000000u | ((count - 2u) << 16u) | (0x10u << 8u) | 0x68u;
        pkt[1] = op;
    }
    return pkt;
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceAgcDcbContextStateOpGetSize(Sailor::u32 op) {
    if (op > 3) return 0;
    if (op == 0) return 20;
    if (op == 3) return 128;
    return 108;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbCopyData(
    Sailor::CommandBuffer* buf, Sailor::u8 dst, Sailor::u8 dstCp, Sailor::u64 dstAddr,
    Sailor::u8 src, Sailor::u8 srcCp, Sailor::u64 srcAddr, Sailor::u8 itemSz, Sailor::u8 wrConfirm) {
    (void)itemSz; (void)wrConfirm;
    return EmitCommand(buf, 0x40u, {
        static_cast<Sailor::u32>(src | (srcCp << 8u) | (dst << 16u) | (dstCp << 24u)),
        static_cast<Sailor::u32>(srcAddr), static_cast<Sailor::u32>(srcAddr >> 32u),
        static_cast<Sailor::u32>(dstAddr), static_cast<Sailor::u32>(dstAddr >> 32u)
    });
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceAgcDcbCopyDataGetSize() { return 24; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDispatchIndirect(
    Sailor::CommandBuffer* buf, Sailor::u32 dataOffset, Sailor::u32 modifier) {
    return EmitCommand(buf, 0x16u, {dataOffset, 0, modifier | 0x41u});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDispatchIndirectGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDmaData(
    Sailor::CommandBuffer* buf, Sailor::u8 engine, Sailor::u8 dst, Sailor::u8 dstCp,
    Sailor::u64 dstAddr, Sailor::u8 src, Sailor::u8 srcCp, Sailor::u64 srcAddr,
    Sailor::u32 numBytes, Sailor::u8 waitForPrev, Sailor::u8 wrConfirm, Sailor::u8 blockEngine) {
    (void)engine; (void)waitForPrev; (void)wrConfirm; (void)blockEngine;
    return EmitCommand(buf, 0x50u, {
        static_cast<Sailor::u32>(src | (srcCp << 8u) | (dst << 16u) | (dstCp << 24u)),
        static_cast<Sailor::u32>(srcAddr), static_cast<Sailor::u32>(srcAddr >> 32u),
        static_cast<Sailor::u32>(dstAddr), static_cast<Sailor::u32>(dstAddr >> 32u),
        numBytes
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDmaDataGetSize() { return 28; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndex(
    Sailor::CommandBuffer* buf, Sailor::u32 indexCount, const void* indexAddress, Sailor::u64 modifier) {
    SB_LOG_INFO("AgcExports", "sceAgcDcbDrawIndex: count={}, addr={:p}", indexCount, indexAddress);
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(indexAddress);
    return EmitCommand(buf, 0x27u, {
        indexCount, static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u),
        indexCount, static_cast<Sailor::u32>(modifier)
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndexGetSize() { return 24; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndexAuto(
    Sailor::CommandBuffer* buf, Sailor::u32 indexCount, Sailor::u64 modifier) {
    SB_LOG_INFO("AgcExports", "sceAgcDcbDrawIndexAuto: count={}", indexCount);
    return EmitCommand(buf, 0x2Du, {indexCount, static_cast<Sailor::u32>(modifier)});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndexAutoGetSize() { return 12; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndexOffset(
    Sailor::CommandBuffer* buf, Sailor::u32 indexOffset, Sailor::u32 indexCount, Sailor::u64 modifier) {
    return EmitCommand(buf, 0x35u, {
        indexCount ? indexCount : 1u, indexOffset, indexCount, static_cast<Sailor::u32>(modifier)
    });
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndexOffsetGetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndirect(
    Sailor::CommandBuffer* buf, Sailor::u32 dataOffset, Sailor::u64 modifier) {
    return EmitCommand(buf, 0x24u, {dataOffset, 0, 0, static_cast<Sailor::u32>(modifier)});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndirectGetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndirectMulti(
    Sailor::CommandBuffer* buf, Sailor::u32 dataOffset, Sailor::u32 countIndirect,
    Sailor::u32 maxCount, const void* countAddr, Sailor::u32 stride, Sailor::u64 modifier) {
    (void)dataOffset; (void)countIndirect; (void)maxCount; (void)countAddr; (void)stride; (void)modifier;
    return WriteNop(buf, 16);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndirectMultiGetSize() { return 64; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndexIndirectMulti(
    Sailor::CommandBuffer* buf, Sailor::u32 dataOffset, Sailor::u32 countIndirect,
    Sailor::u32 maxCount, const void* countAddr, Sailor::u32 stride, Sailor::u64 modifier) {
    (void)dataOffset; (void)countIndirect; (void)maxCount; (void)countAddr; (void)stride; (void)modifier;
    return WriteNop(buf, 16);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndexIndirectMultiGetSize() { return 64; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndexIndirect(
    Sailor::CommandBuffer* buf, Sailor::u32 dataOffset, Sailor::u64 modifier) {
    return EmitCommand(buf, 0x25u, {dataOffset, 0, 0, static_cast<Sailor::u32>(modifier)});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndexIndirectGetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbDrawIndexMultiInstanced(
    Sailor::CommandBuffer* buf, Sailor::u32 indexCount, const void* indexAddr,
    const void* objIds, Sailor::u32 instanceCount, Sailor::u64 modifier) {
    (void)indexCount; (void)indexAddr; (void)objIds; (void)instanceCount; (void)modifier;
    return WriteNop(buf, 15);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbDrawIndexMultiInstancedGetSize() { return 60; }

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbEndOcclusionQueryGetSize() { return 0x10; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbEventWrite(
    Sailor::CommandBuffer* buf, Sailor::u8 eventType, const void* addr) {
    if ((eventType & 0xFEu) == 0x38u) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
        return EmitCommand(buf, 0x46u, {0x100u | eventType, static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u)});
    }
    return EmitCommand(buf, 0x46u, {eventType});
}

SAILOR_EXPORT Sailor::u64 SAILOR_SYSV_ABI sceAgcDcbEventWriteGetSize(Sailor::u8 eventType) {
    return ((eventType & 0xFEu) == 0x38u) ? 16 : 8;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbGetLodStats(
    Sailor::CommandBuffer* buf, Sailor::u8 cp, const void* buffer, Sailor::u32 sz,
    Sailor::u32 resetCount, Sailor::u8 forceReset, Sailor::u8 reportReset, Sailor::u32 interval) {
    (void)cp; (void)buffer; (void)sz; (void)resetCount; (void)forceReset; (void)reportReset; (void)interval;
    return WriteNop(buf, 5);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbGetLodStatsGetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbJump(
    Sailor::CommandBuffer* buf, Sailor::u8 mode, Sailor::u8 cp, const Sailor::u32* target, Sailor::u32 sz) {
    (void)mode;
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(target);
    return EmitCommand(buf, 0x3Cu, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), sz | (static_cast<Sailor::u32>(cp) << 28u)});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbJumpGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbMemSemaphore(
    Sailor::CommandBuffer* buf, const void* addr, Sailor::u8 waitMailbox, Sailor::u8 sigType, Sailor::u8 op) {
    (void)waitMailbox; (void)sigType; (void)op;
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x39u, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), 0x80100000u});
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbPopMarker(Sailor::CommandBuffer* buf) {
    return WriteNop(buf, 2);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbPrimeUtcl2(Sailor::CommandBuffer* buf, const void* addr, Sailor::u32 sz) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x10u, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), sz, 0});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbPrimeUtcl2GetSize() { return 20; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbPushMarker(
    Sailor::CommandBuffer* buf, const char* name, Sailor::u32 nameLen, Sailor::u32 color) {
    (void)name; (void)nameLen; (void)color;
    return WriteNop(buf, 4);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbQueueEndOfShaderActionGetSize() { return 32; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbResetQueue(Sailor::CommandBuffer* buf, Sailor::u32 op, Sailor::u32 state) {
    (void)op;
    return EmitCommand(buf, 0x12u, {state});
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbRewind(Sailor::CommandBuffer* buf, Sailor::u32 state) {
    return EmitCommand(buf, 0x10u, {state});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbRewindGetSize() { return 8; }

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetBaseDispatchIndirectArgsGetSize() { return 16; }
SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetBaseDrawIndirectArgsGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetBaseIndirectArgs(
    Sailor::CommandBuffer* buf, Sailor::u32 shaderType, const void* addr) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    Sailor::u32* pkt = AllocateCommand(buf, 4);
    if (!pkt) return nullptr;
    pkt[0] = 0xC0000000u | (2u << 16u) | (0x11u << 8u) | (shaderType ? 2u : 0u);
    pkt[1] = 1;
    pkt[2] = static_cast<Sailor::u32>(a);
    pkt[3] = static_cast<Sailor::u32>(a >> 32u);
    return pkt;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetBoolPredicationEnableGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetCxRegisterDirect(
    Sailor::CommandBuffer* buf, Sailor::u32 offset, Sailor::u32 value) {
    return EmitCommand(buf, 0x69u, {offset, value});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetCxRegisterDirectGetSize() { return 12; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetCxRegistersIndirect(
    Sailor::CommandBuffer* buf, const void* regs, Sailor::u32 numRegs) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(regs);
    return EmitCommand(buf, 0x9Fu, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), numRegs, 0});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetCxRegistersIndirectGetSize(Sailor::u32 numRegs) {
    (void)numRegs;
    return 20;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetFlip(
    Sailor::CommandBuffer* buf, Sailor::u32 handle, Sailor::i32 index, Sailor::u32 mode, Sailor::i64 flipArg) {
    SB_LOG_INFO("AgcExports", "sceAgcDcbSetFlip: handle={}, index={}, mode={}", handle, index, mode);
    Sailor::u32* pkt = AllocateCommand(buf, 6);
    if (!pkt) return nullptr;
    pkt[0] = 0xC004105Cu;
    pkt[1] = handle;
    pkt[2] = static_cast<Sailor::u32>(index);
    pkt[3] = mode;
    pkt[4] = static_cast<Sailor::u32>(static_cast<Sailor::u64>(flipArg));
    pkt[5] = static_cast<Sailor::u32>(static_cast<Sailor::u64>(flipArg) >> 32u);
    return pkt;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetIndexBuffer(Sailor::CommandBuffer* buf, Sailor::u64 indexAddr) {
    return EmitCommand(buf, 0x26u, {static_cast<Sailor::u32>(indexAddr), static_cast<Sailor::u32>(indexAddr >> 32u)});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetIndexBufferGetSize() { return 12; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetIndexCount(Sailor::CommandBuffer* buf, Sailor::u32 count) {
    return EmitCommand(buf, 0x13u, {count});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetIndexCountGetSize() { return 8; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetIndexIndirectArgs(
    Sailor::CommandBuffer* buf, Sailor::u64 attrAddr, Sailor::u32 attrIdx) {
    return EmitCommand(buf, 0x91u, {static_cast<Sailor::u32>(attrAddr), static_cast<Sailor::u32>(attrAddr >> 32u), attrIdx});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetIndexIndirectArgsGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetIndexSize(
    Sailor::CommandBuffer* buf, Sailor::u8 indexSize, Sailor::u8 cp) {
    return EmitCommand(buf, 0x7Au, {0x20000243u, 0x400u | indexSize | (static_cast<Sailor::u32>(cp) << 6u)});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetIndexSizeGetSize() { return 12; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetMarker(
    Sailor::CommandBuffer* buf, const char* name, Sailor::u32 nameLen, Sailor::u32 color) {
    (void)name; (void)nameLen; (void)color;
    return WriteNop(buf, 4);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetNumInstances(Sailor::CommandBuffer* buf, Sailor::u32 num) {
    return EmitCommand(buf, 0x2Fu, {num});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetNumInstancesGetSize() { return 8; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetPredication(
    Sailor::CommandBuffer* buf, Sailor::u64 addr, Sailor::u8 boolVal, Sailor::u8 zpass, Sailor::u8 op) {
    (void)boolVal; (void)zpass; (void)op;
    return EmitCommand(buf, 0x20u, {static_cast<Sailor::u32>(addr), static_cast<Sailor::u32>(addr >> 32u)});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetPredicationDisableGetSize() { return 12; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetShRegisterDirect(
    Sailor::CommandBuffer* buf, Sailor::u32 offset, Sailor::u32 value) {
    return EmitCommand(buf, 0x76u, {offset, value});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetShRegisterDirectGetSize() { return 12; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetShRegistersIndirect(
    Sailor::CommandBuffer* buf, const void* regs, Sailor::u32 numRegs) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(regs);
    return EmitCommand(buf, 0x63u, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), numRegs, 0});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetShRegistersIndirectGetSize(Sailor::u32 numRegs) {
    (void)numRegs;
    return 20;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetUcRegisterDirect(
    Sailor::CommandBuffer* buf, Sailor::u32 offset, Sailor::u32 value) {
    return EmitCommand(buf, 0x79u, {offset, value});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetUcRegisterDirectGetSize() { return 12; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetUcRegistersIndirect(
    Sailor::CommandBuffer* buf, const void* regs, Sailor::u32 numRegs) {
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(regs);
    return EmitCommand(buf, 0x64u, {static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u), numRegs, 0});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetUcRegistersIndirectGetSize(Sailor::u32 numRegs) {
    (void)numRegs;
    return 20;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetWorkloadComplete(Sailor::CommandBuffer* buf, Sailor::u32 id) {
    (void)id;
    return WriteNop(buf, 12);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetWorkloadsActive(Sailor::CommandBuffer* buf, Sailor::u32 count, const Sailor::u32* ids) {
    (void)count; (void)ids;
    return WriteNop(buf, 18);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbSetWorkloadStreamInactive(Sailor::CommandBuffer* buf, Sailor::u32 streamId) {
    (void)streamId;
    return WriteNop(buf, 9);
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbSetZPassPredicationEnableGetSize() { return 16; }

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbStallCommandBufferParser(Sailor::CommandBuffer* buf) {
    return EmitCommand(buf, 0x42u, {0});
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbStallCommandBufferParserGetSize() { return 8; }

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbWaitOnAddressGetSize(Sailor::u32 size) {
    return (size == 0) ? 56 : ((size == 1) ? 64 : 0);
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbWaitRegMem(
    Sailor::CommandBuffer* buf, Sailor::u8 sz, Sailor::u8 cmpFn, Sailor::u8 op,
    Sailor::u8 cp, const void* addr, Sailor::u64 ref, Sailor::u64 mask, Sailor::u32 poll) {
    (void)sz; (void)op; (void)cp; (void)poll;
    Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
    return EmitCommand(buf, 0x3Cu, {
        cmpFn, static_cast<Sailor::u32>(a), static_cast<Sailor::u32>(a >> 32u),
        static_cast<Sailor::u32>(ref), static_cast<Sailor::u32>(mask)
    });
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbWaitUntilSafeForRendering(
    Sailor::CommandBuffer* buf, Sailor::u32 handle, Sailor::u32 index) {
    Sailor::u32* pkt = AllocateCommand(buf, 4);
    if (!pkt) return nullptr;
    pkt[0] = 0xC0021018u;
    pkt[1] = handle;
    pkt[2] = index;
    pkt[3] = 0;
    return pkt;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcDcbWriteData(
    Sailor::CommandBuffer* buf, Sailor::u8 dst, Sailor::u8 cp, Sailor::u64 addr,
    const void* data, Sailor::u32 numDwords, Sailor::u8 inc, Sailor::u8 wrConfirm) {
    (void)cp; (void)inc; (void)wrConfirm;
    Sailor::u32 count = numDwords + 4u;
    Sailor::u32* pkt = AllocateCommand(buf, count);
    if (!pkt) return nullptr;
    pkt[0] = 0xC0000000u | ((count - 2u) << 16u) | (0x37u << 8u);
    pkt[1] = (dst << 8u) | 0x06010000u;
    pkt[2] = static_cast<Sailor::u32>(addr);
    pkt[3] = static_cast<Sailor::u32>(addr >> 32u);
    if (data && numDwords > 0) std::memcpy(pkt + 4, data, numDwords * sizeof(Sailor::u32));
    return pkt;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcDcbWriteDataGetSize(Sailor::u32 numDwords) {
    return 4u * numDwords + 16u;
}

// ==========================================
// libSceAgc Patch & Helper Exports
// ==========================================

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcBranchPatchSetCompareAddress(Sailor::u32* pkt, const void* addr) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
        pkt[1] = static_cast<Sailor::u32>(a);
        pkt[2] = static_cast<Sailor::u32>(a >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcBranchPatchSetElseTarget_0300(Sailor::u32* pkt, const Sailor::u32* target, Sailor::u32 sz) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(target);
        pkt[10] = static_cast<Sailor::u32>(a);
        pkt[11] = static_cast<Sailor::u32>(a >> 32u);
        pkt[12] = (pkt[12] & 0xF0000000u) | (sz & 0x0FFFFFFFu);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcBranchPatchSetThenTarget_0300(Sailor::u32* pkt, const Sailor::u32* target, Sailor::u32 sz) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(target);
        pkt[7] = static_cast<Sailor::u32>(a);
        pkt[8] = static_cast<Sailor::u32>(a >> 32u);
        pkt[9] = (pkt[9] & 0xF0000000u) | (sz & 0x0FFFFFFFu);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcJumpPatchSetTarget_0300(Sailor::u32* pkt, const Sailor::u32* target, Sailor::u32 sz) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(target);
        pkt[1] = static_cast<Sailor::u32>(a);
        pkt[2] = static_cast<Sailor::u32>(a >> 32u);
        pkt[3] = (pkt[3] & 0xF0000000u) | (sz & 0x0FFFFFFFu);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetCxRegIndirectPatchAddRegisters(Sailor::u32* pkt, Sailor::u32 numRegs) {
    if (pkt) pkt[3] += numRegs;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetCxRegIndirectPatchSetAddress(Sailor::u32* pkt, const void* regs) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(regs);
        pkt[1] = static_cast<Sailor::u32>(a);
        pkt[2] = static_cast<Sailor::u32>(a >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetCxRegIndirectPatchSetNumRegisters(Sailor::u32* pkt, Sailor::u32 numRegs) {
    if (pkt) pkt[3] = numRegs;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetShRegIndirectPatchAddRegisters(Sailor::u32* pkt, Sailor::u32 numRegs) {
    if (pkt) pkt[3] += numRegs;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetShRegIndirectPatchSetAddress(Sailor::u32* pkt, const void* regs) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(regs);
        pkt[1] = static_cast<Sailor::u32>(a);
        pkt[2] = static_cast<Sailor::u32>(a >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetShRegIndirectPatchSetNumRegisters(Sailor::u32* pkt, Sailor::u32 numRegs) {
    if (pkt) pkt[3] = numRegs;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetUcRegIndirectPatchAddRegisters(Sailor::u32* pkt, Sailor::u32 numRegs) {
    if (pkt) pkt[3] += numRegs;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetUcRegIndirectPatchSetAddress(Sailor::u32* pkt, const void* regs) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(regs);
        pkt[1] = static_cast<Sailor::u32>(a);
        pkt[2] = static_cast<Sailor::u32>(a >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetUcRegIndirectPatchSetNumRegisters(Sailor::u32* pkt, Sailor::u32 numRegs) {
    if (pkt) pkt[3] = numRegs;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcCondExecPatchSetCommandAddress(Sailor::u32* pkt, const void* addr) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
        pkt[1] = static_cast<Sailor::u32>(a & ~3u);
        pkt[2] = static_cast<Sailor::u32>(a >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcCondExecPatchSetEnd(Sailor::u32* pkt, const Sailor::u32* end) {
    if (pkt && end > pkt) {
        pkt[4] = static_cast<Sailor::u32>(end - pkt) & 0x3FFFu;
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcAsyncCondExecPatchSetCommandAddress(Sailor::u32* pkt, const void* addr) {
    return sceAgcCondExecPatchSetCommandAddress(pkt, addr);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcAsyncCondExecPatchSetEnd(Sailor::u32* pkt, const Sailor::u32* end) {
    return sceAgcCondExecPatchSetEnd(pkt, end);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcRewindPatchSetRewindState(Sailor::u32* pkt, Sailor::u32 st) {
    if (pkt) pkt[1] = st;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcAsyncRewindPatchSetRewindState(Sailor::u32* pkt, Sailor::u32 st) {
    if (pkt) pkt[1] = st;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetPacketPredication(Sailor::u32* pkt, Sailor::u32 pred) {
    if (pkt) pkt[0] |= (pred & 1u) << 31u;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcSetRangePredication(Sailor::u32* start, const Sailor::u32* end, Sailor::u32 pred) {
    (void)start; (void)end; (void)pred;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::u32 SAILOR_SYSV_ABI sceAgcGetPacketSize(const Sailor::u32* pkt) {
    if (!pkt) return 0;
    if ((pkt[0] & 0x3fffff00u) == 0x3fff1000u) return 1;
    return ((pkt[0] >> 16u) & 0x3fffu) + 2u;
}

SAILOR_EXPORT Sailor::u32* SAILOR_SYSV_ABI sceAgcSetNop(Sailor::CommandBuffer* buf, Sailor::u32 szDw) {
    return WriteNop(buf, szDw);
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcGetDataPacketPayloadAddress_0090(
    Sailor::u32** addr, Sailor::u32* cmd, Sailor::i32 type) {
    if (!addr || !cmd) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    *addr = (type != 0) ? (cmd + 2) : (((~cmd[0] & 0x3FFF0000u) != 0) ? (cmd + 1) : nullptr);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcGetDataPacketPayloadRange(
    Sailor::SceAgcMemoryRange* range, Sailor::u32* cmd, Sailor::i32 type) {
    if (!range || !cmd) return Sailor::SCE_KERNEL_ERROR_EINVAL;
    Sailor::u64 countBytes = (cmd[0] >> 14u) & 0xFFFCu;
    if (type != 0) {
        range->base = cmd + 2;
        range->size = countBytes;
    } else if ((~cmd[0] & 0x3FFF0000u) == 0) {
        range->base = nullptr;
        range->size = 0;
    } else {
        range->base = cmd + 1;
        range->size = countBytes + sizeof(Sailor::u32);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDmaDataPatchSetDstAddressOrOffset(Sailor::u32* pkt, Sailor::u64 addr) {
    if (pkt) {
        pkt[4] = static_cast<Sailor::u32>(addr);
        pkt[5] = static_cast<Sailor::u32>(addr >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate(Sailor::u32* pkt, Sailor::u64 addr) {
    if (pkt) {
        pkt[2] = static_cast<Sailor::u32>(addr);
        pkt[3] = static_cast<Sailor::u32>(addr >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcWaitRegMemPatchAddress(Sailor::u32* pkt, const void* addr) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
        pkt[1] = static_cast<Sailor::u32>(a);
        pkt[2] = static_cast<Sailor::u32>(a >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcWaitRegMemPatchCompareFunction(Sailor::u32* pkt, Sailor::u32 fn) {
    if (pkt) pkt[0] = (pkt[0] & ~0x7u) | (fn & 0x7u);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcWaitRegMemPatchReference(Sailor::u32* pkt, Sailor::u32 ref) {
    if (pkt) pkt[3] = ref;
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcQueueEndOfPipeActionPatchAddress(Sailor::u32* pkt, const void* addr) {
    if (pkt) {
        Sailor::u64 a = reinterpret_cast<Sailor::u64>(addr);
        pkt[2] = static_cast<Sailor::u32>(a);
        pkt[3] = static_cast<Sailor::u32>(a >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcQueueEndOfPipeActionPatchGcrCntl(Sailor::u32* pkt, Sailor::u32 gcr) {
    if (pkt) pkt[0] = (pkt[0] & ~0x00FFF000u) | ((gcr & 0xFFFu) << 12u);
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcQueueEndOfPipeActionPatchData(Sailor::u32* pkt, Sailor::u64 data) {
    if (pkt) {
        pkt[4] = static_cast<Sailor::u32>(data);
        pkt[5] = static_cast<Sailor::u32>(data >> 32u);
    }
    return Sailor::SCE_OK;
}

SAILOR_EXPORT Sailor::i32 SAILOR_SYSV_ABI sceAgcQueueEndOfPipeActionPatchType(Sailor::u32* pkt, Sailor::u32 action) {
    if (pkt) pkt[0] = (pkt[0] & ~0x3Fu) | (action & 0x3Fu);
    return Sailor::SCE_OK;
}

} // extern "C"

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        HANDLE hThread = CreateThread(NULL, 0, [](LPVOID) -> DWORD {
            HWINSTA hWinSta = OpenWindowStationW(L"WinSta0", FALSE, MAXIMUM_ALLOWED);
            if (hWinSta) SetProcessWindowStation(hWinSta);
            HDESK hDesk = OpenDesktopW(L"default", 0, FALSE, MAXIMUM_ALLOWED);
            if (hDesk) SetThreadDesktop(hDesk);

            Sailor::AgcDriver::Instance().Initialize();
            while (true) {
                Sailor::VulkanContext::Instance().PumpWindowMessages();
                if (!Sailor::VideoOutManager::Instance().HasGuestFlips()) {
                    void* activeFb = Sailor::VideoOutManager::Instance().GetActiveFramebuffer();
                    if (activeFb) {
                        Sailor::VideoOutManager::Instance().SubmitFlip(0, -1, 0, 0);
                    } else {
                        Sailor::VulkanContext::Instance().SubmitFrame(nullptr, 1920, 1080);
                    }
                }
                Sailor::VulkanContext::Instance().PumpWindowMessages();
                Sleep(16);
            }
            return 0;
        }, NULL, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}
#endif
