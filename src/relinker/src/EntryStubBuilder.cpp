#include "relinker/EntryStubBuilder.hpp"
#include <cstring>

namespace Sailor {

EntryStubResult EntryStubBuilder::Build(u64 guestEntryVirtualAddress,
                                        u64 exitProcessIatVirtualAddress,
                                        u64 stubBaseVirtualAddress,
                                        const std::vector<u64>& initArrayVas,
                                        u64 initTlsIatVa,
                                        u64 tlsTemplateVa,
                                        u64 tlsFileSize,
                                        u64 tlsMemSize,
                                        u64 tlsAlign) const {
    EntryStubResult result;
    auto& code = result.codeData;

    // 1. sub rsp, 40 (0x28) -> 32 bytes shadow space + 8 bytes align
    code.push_back(0x48);
    code.push_back(0x83);
    code.push_back(0xEC);
    code.push_back(0x28);

    // If DllMain (exitProcessIatVirtualAddress == 0), check edx == 1 (DLL_PROCESS_ATTACH)
    // If not DLL_PROCESS_ATTACH, skip initArray and guestEntry, return TRUE (1) immediately
    size_t skipAttachJumpIndex = 0;
    if (exitProcessIatVirtualAddress == 0) {
        // cmp edx, 1 (83 FA 01)
        code.push_back(0x83);
        code.push_back(0xFA);
        code.push_back(0x01);
        // jne rel32 (0F 85 xx xx xx xx)
        code.push_back(0x0F);
        code.push_back(0x85);
        skipAttachJumpIndex = code.size();
        for (int i = 0; i < 4; ++i) code.push_back(0); // placeholder
    }

    // 2. Initialize Thread-Local Storage & %fs Base if TLS handler is available
    if (initTlsIatVa > 0) {
#if defined(__clang__) || defined(__GNUC__)
        // System V AMD64 ABI: rdi = templateVa, rsi = fileSize, rdx = memSize, rcx = align
        // mov rdi, <tlsTemplateVa>
        code.push_back(0x48); code.push_back(0xBF);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsTemplateVa >> (i * 8)) & 0xFF));
        // mov rsi, <tlsFileSize>
        code.push_back(0x48); code.push_back(0xBE);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsFileSize >> (i * 8)) & 0xFF));
        // mov rdx, <tlsMemSize>
        code.push_back(0x48); code.push_back(0xBA);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsMemSize >> (i * 8)) & 0xFF));
        // mov rcx, <tlsAlign>
        code.push_back(0x48); code.push_back(0xB9);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsAlign >> (i * 8)) & 0xFF));
#else
        // Microsoft x64 ABI: rcx = templateVa, rdx = fileSize, r8 = memSize, r9 = align
        // mov rcx, <tlsTemplateVa>
        code.push_back(0x48); code.push_back(0xB9);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsTemplateVa >> (i * 8)) & 0xFF));
        // mov rdx, <tlsFileSize>
        code.push_back(0x48); code.push_back(0xBA);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsFileSize >> (i * 8)) & 0xFF));
        // mov r8, <tlsMemSize>
        code.push_back(0x49); code.push_back(0xB8);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsMemSize >> (i * 8)) & 0xFF));
        // mov r9, <tlsAlign>
        code.push_back(0x49); code.push_back(0xB9);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((tlsAlign >> (i * 8)) & 0xFF));
#endif
        // mov r10, <initTlsIatVa> (49 BA imm64)
        code.push_back(0x49);
        code.push_back(0xBA);
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<u8>((initTlsIatVa >> (i * 8)) & 0xFF));
        }
        // call qword ptr [r10] (41 FF 12)
        code.push_back(0x41);
        code.push_back(0xFF);
        code.push_back(0x12);
    }

    // 3. Invoke DT_INIT_ARRAY global constructors
    for (u64 initVa : initArrayVas) {
        if (initVa == 0) continue;
        // mov rax, <initVa> (48 B8 imm64)
        code.push_back(0x48);
        code.push_back(0xB8);
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<u8>((initVa >> (i * 8)) & 0xFF));
        }
        // call rax (FF D0)
        code.push_back(0xFF);
        code.push_back(0xD0);
    }

    // 4. Pass PlayStation 5 process arguments block in RDI and invoke guest entry
    size_t rdiPatchIndex = 0;
    if (guestEntryVirtualAddress != 0) {
        if (exitProcessIatVirtualAddress > 0) {
            // mov rdi, <argsVa> (48 BF imm64)
            code.push_back(0x48);
            code.push_back(0xBF);
            rdiPatchIndex = code.size();
            for (int i = 0; i < 8; ++i) code.push_back(0); // placeholder for argsVa
        } else {
            // For DLL: xor edi, edi (rdi = 0 for argc/args)
            code.push_back(0x31);
            code.push_back(0xFF);
        }

        // xor esi, esi (rsi = 0)
        code.push_back(0x31);
        code.push_back(0xF6);

        // xor edx, edx (rdx = 0)
        code.push_back(0x31);
        code.push_back(0xD2);

        // xor ebp, ebp (rbp = 0, zero frame)
        code.push_back(0x31);
        code.push_back(0xED);

        // 5. mov rax, <guestEntryVirtualAddress>
        code.push_back(0x48);
        code.push_back(0xB8);
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<u8>((guestEntryVirtualAddress >> (i * 8)) & 0xFF));
        }

        // 6. call rax
        code.push_back(0xFF);
        code.push_back(0xD0);
    }

    if (exitProcessIatVirtualAddress == 0) {
        u32 targetOffset = static_cast<u32>(code.size());
        u32 rel32 = targetOffset - static_cast<u32>(skipAttachJumpIndex + 4);
        std::memcpy(&code[skipAttachJumpIndex], &rel32, sizeof(rel32));
    }

    // 7 & 8. Call ExitProcess for EXE or return TRUE (1) for DLL DllMain
    if (exitProcessIatVirtualAddress > 0) {
        // mov ecx, eax (pass exit code to ExitProcess)
        code.push_back(0x89);
        code.push_back(0xC1);
        // mov r10, <exitProcessIatVirtualAddress>
        code.push_back(0x49);
        code.push_back(0xBA);
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<u8>((exitProcessIatVirtualAddress >> (i * 8)) & 0xFF));
        }
        // call qword ptr [r10]
        code.push_back(0x41);
        code.push_back(0xFF);
        code.push_back(0x12);
    } else {
        // For DllMain: return TRUE (1)
        // mov eax, 1 (B8 01 00 00 00)
        code.push_back(0xB8);
        code.push_back(0x01);
        code.push_back(0x00);
        code.push_back(0x00);
        code.push_back(0x00);
    }

    // 9. add rsp, 40 (0x28)
    code.push_back(0x48);
    code.push_back(0x83);
    code.push_back(0xC4);
    code.push_back(0x28);

    // 10. ret
    code.push_back(0xC3);

    // 11. Append SceProcessArgs structure for PlayStation 5 EXE entry
    if (exitProcessIatVirtualAddress > 0 && rdiPatchIndex > 0) {
        while (code.size() % 16 != 0) {
            code.push_back(0xCC); // int 3 alignment padding
        }
        size_t argsOffset = code.size();
        u64 argsVa = stubBaseVirtualAddress + argsOffset;

        // Patch mov rdi, <argsVa>
        for (int i = 0; i < 8; ++i) {
            code[rdiPatchIndex + i] = static_cast<u8>((argsVa >> (i * 8)) & 0xFF);
        }

        // Layout:
        // +0:  u32 argc = 3
        // +4:  u32 reserved = 0
        // +8:  u64 argv[0] = argsVa + 40 (pointer to "/app0/eboot.bin")
        // +16: u64 argv[1] = argv0 + 16 (pointer to "+config")
        // +24: u64 argv[2] = argv1 + 8 (pointer to "game.retail.cfg")
        // +32: u64 argv[3] = 0 (NULL termination)
        // +40: string "/app0/eboot.bin\0"
        // +56: string "+config\0"
        // +64: string "game.retail.cfg\0"
        u32 argc = 3;
        code.insert(code.end(), reinterpret_cast<const u8*>(&argc), reinterpret_cast<const u8*>(&argc) + 4);
        u32 reserved = 0;
        code.insert(code.end(), reinterpret_cast<const u8*>(&reserved), reinterpret_cast<const u8*>(&reserved) + 4);

        u64 argv0 = argsVa + 40;
        u64 argv1 = argv0 + 16;
        u64 argv2 = argv1 + 8;
        u64 argv3 = 0;
        code.insert(code.end(), reinterpret_cast<const u8*>(&argv0), reinterpret_cast<const u8*>(&argv0) + 8);
        code.insert(code.end(), reinterpret_cast<const u8*>(&argv1), reinterpret_cast<const u8*>(&argv1) + 8);
        code.insert(code.end(), reinterpret_cast<const u8*>(&argv2), reinterpret_cast<const u8*>(&argv2) + 8);
        code.insert(code.end(), reinterpret_cast<const u8*>(&argv3), reinterpret_cast<const u8*>(&argv3) + 8);

        const char appPathStr[] = "/app0/eboot.bin";
        const char arg1Str[] = "+config";
        const char arg2Str[] = "game.retail.cfg";
        code.insert(code.end(), reinterpret_cast<const u8*>(appPathStr),
                    reinterpret_cast<const u8*>(appPathStr) + sizeof(appPathStr));
        code.insert(code.end(), reinterpret_cast<const u8*>(arg1Str),
                    reinterpret_cast<const u8*>(arg1Str) + sizeof(arg1Str));
        code.insert(code.end(), reinterpret_cast<const u8*>(arg2Str),
                    reinterpret_cast<const u8*>(arg2Str) + sizeof(arg2Str));
    }

    return result;
}

std::vector<u8> EntryStubBuilder::SynthesizeDirectJumpThunk(u64 iatVirtualAddress, u64 thunkVirtualAddress) const {
    std::vector<u8> code;

    i64 nextRip = static_cast<i64>(thunkVirtualAddress + 6);
    i64 disp = static_cast<i64>(iatVirtualAddress) - nextRip;

    if (disp >= INT32_MIN && disp <= INT32_MAX) {
        code.reserve(6);
        code.push_back(0xFF);
        code.push_back(0x25);
        u32 udisp = static_cast<u32>(disp);
        code.push_back(static_cast<u8>(udisp & 0xFF));
        code.push_back(static_cast<u8>((udisp >> 8) & 0xFF));
        code.push_back(static_cast<u8>((udisp >> 16) & 0xFF));
        code.push_back(static_cast<u8>((udisp >> 24) & 0xFF));
    } else {
        code.reserve(14);
        code.push_back(0xFF);
        code.push_back(0x25);
        code.push_back(0x00);
        code.push_back(0x00);
        code.push_back(0x00);
        code.push_back(0x00);
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<u8>((iatVirtualAddress >> (i * 8)) & 0xFF));
        }
    }

    return code;
}

std::vector<u8> EntryStubBuilder::SynthesizeUnhandledNidStub(u64 nidValue, u64 logHandlerIatVa) const {
    std::vector<u8> code;
    code.reserve(32);
    // sub rsp, 40 (0x28) -> 32-byte shadow space + 16-byte stack alignment
    code.insert(code.end(), {0x48, 0x83, 0xEC, 0x28});
    // mov rcx, <nidValue> (48 B9 imm64) -> MS x64 1st arg
    code.push_back(0x48); code.push_back(0xB9);
    for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((nidValue >> (i * 8)) & 0xFF));
    if (logHandlerIatVa > 0) {
        // mov r10, <logHandlerIatVa> (49 BA imm64)
        code.push_back(0x49); code.push_back(0xBA);
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>((logHandlerIatVa >> (i * 8)) & 0xFF));
        // call qword ptr [r10] (41 FF 12)
        code.insert(code.end(), {0x41, 0xFF, 0x12});
    } else {
        code.insert(code.end(), {0x31, 0xC0}); // xor eax, eax
    }
    // add rsp, 40 (0x28); ret
    code.insert(code.end(), {0x48, 0x83, 0xC4, 0x28, 0xC3});
    return code;
}

std::vector<u8> EntryStubBuilder::SynthesizeSysVToMsThunk(u64 iatVirtualAddress) const {
    std::vector<u8> code;
    code.reserve(48);

    // 1. push rbp
    code.push_back(0x55);

    // 2. mov rbp, rsp
    code.push_back(0x48); code.push_back(0x89); code.push_back(0xE5);

    // 3. sub rsp, 48 (0x30) -> 32 bytes shadow space + 16 bytes for 5th/6th args
    code.push_back(0x48); code.push_back(0x83); code.push_back(0xEC); code.push_back(0x30);

    // 4. mov qword ptr [rsp + 32], r8  (pass 5th arg from SysV r8 to Windows stack slot)
    code.push_back(0x4C); code.push_back(0x89); code.push_back(0x44); code.push_back(0x24); code.push_back(0x20);

    // 5. mov qword ptr [rsp + 40], r9  (pass 6th arg from SysV r9 to Windows stack slot)
    code.push_back(0x4C); code.push_back(0x89); code.push_back(0x4C); code.push_back(0x24); code.push_back(0x28);

    // 6. mov r9, rcx                   (4th arg: SysV rcx -> Windows r9)
    code.push_back(0x49); code.push_back(0x89); code.push_back(0xC9);

    // 7. mov r8, rdx                   (3rd arg: SysV rdx -> Windows r8)
    code.push_back(0x49); code.push_back(0x89); code.push_back(0xD0);

    // 8. mov rdx, rsi                  (2nd arg: SysV rsi -> Windows rdx)
    code.push_back(0x48); code.push_back(0x89); code.push_back(0xF2);

    // 9. mov rcx, rdi                  (1st arg: SysV rdi -> Windows rcx)
    code.push_back(0x48); code.push_back(0x89); code.push_back(0xF9);

    // 10. mov r10, <iatVirtualAddress> (49 BA imm64)
    code.push_back(0x49); code.push_back(0xBA);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<u8>((iatVirtualAddress >> (i * 8)) & 0xFF));
    }

    // 11. call qword ptr [r10]         (41 FF 12)
    code.push_back(0x41); code.push_back(0xFF); code.push_back(0x12);

    // 12. mov rsp, rbp                 (48 89 EC)
    code.push_back(0x48); code.push_back(0x89); code.push_back(0xEC);

    // 13. pop rbp                      (5D)
    code.push_back(0x5D);

    // 14. ret                          (C3)
    code.push_back(0xC3);

    return code;
}

std::vector<u8> EntryStubBuilder::SynthesizeSysVToMsThunkSincosf(u64 iatVirtualAddress) const {
    std::vector<u8> code;
    code.reserve(32);

    // 1. push rbp
    code.push_back(0x55);

    // 2. mov rbp, rsp
    code.push_back(0x48); code.push_back(0x89); code.push_back(0xE5);

    // 3. sub rsp, 32 (0x20 shadow space)
    code.push_back(0x48); code.push_back(0x83); code.push_back(0xEC); code.push_back(0x20);

    // 4. mov r8, rsi  (SysV 2nd int arg 'cos*' -> MSVC 3rd arg R8)
    code.push_back(0x49); code.push_back(0x89); code.push_back(0xF0);

    // 5. mov rdx, rdi (SysV 1st int arg 'sin*' -> MSVC 2nd arg RDX)
    code.push_back(0x48); code.push_back(0x89); code.push_back(0xFA);

    // (XMM0 is untouched and holds float 'x', which is MSVC 1st FP arg)

    // 6. mov r10, <iatVirtualAddress> (49 BA imm64)
    code.push_back(0x49); code.push_back(0xBA);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<u8>((iatVirtualAddress >> (i * 8)) & 0xFF));
    }

    // 7. call qword ptr [r10] (41 FF 12)
    code.push_back(0x41); code.push_back(0xFF); code.push_back(0x12);

    // 8. mov rsp, rbp (48 89 EC)
    code.push_back(0x48); code.push_back(0x89); code.push_back(0xEC);

    // 9. pop rbp (5D)
    code.push_back(0x5D);

    // 10. ret (C3)
    code.push_back(0xC3);

    return code;
}

} // namespace Sailor
