#pragma once

#include "common/Types.hpp"
#include <vector>

namespace Sailor {

struct EntryStubResult {
    std::vector<u8> codeData;
    u32 entryPointOffset{0}; // Offset within codeData where entry starts
};

class EntryStubBuilder {
public:
    EntryStubBuilder() = default;

    EntryStubResult Build(u64 guestEntryVirtualAddress,
                          u64 exitProcessIatVirtualAddress,
                          u64 stubBaseVirtualAddress,
                          const std::vector<u64>& initArrayVas = {},
                          u64 initTlsIatVa = 0,
                          u64 tlsTemplateVa = 0,
                          u64 tlsFileSize = 0,
                          u64 tlsMemSize = 0,
                          u64 tlsAlign = 16) const;

    // Direct jump thunk (6-byte jmp qword ptr [rip + disp32] or 14-byte absolute)
    std::vector<u8> SynthesizeDirectJumpThunk(u64 iatVirtualAddress, u64 thunkVirtualAddress) const;

    // Generates a 48-byte System V -> Windows x64 ABI translation bridge thunk
    std::vector<u8> SynthesizeSysVToMsThunk(u64 iatVirtualAddress) const;

    // Generates a specialized System V -> Windows x64 bridge thunk for sincosf(float, float*, float*)
    std::vector<u8> SynthesizeSysVToMsThunkSincosf(u64 iatVirtualAddress) const;

    // 24-byte universal fallback stub for unimplemented NIDs
    // Logs the NID once via sailor_kernel!__sailor_unhandled_nid and returns 0 (SCE_OK)
    std::vector<u8> SynthesizeUnhandledNidStub(u64 nidValue, u64 logHandlerIatVa) const;
};

} // namespace Sailor
