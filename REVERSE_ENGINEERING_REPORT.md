# SailorBox: PlayStation 5 Native Compatibility Research & Engineering Report

## Executive Summary

**SailorBox** is a clean-room native compatibility runtime and binary relinker designed to execute PlayStation 5 (Prospero OS / x86-64) executables directly on host Windows x64 hardware without CPU emulation or virtual machine overhead.

This document compiles the reverse engineering findings, binary contract analyses, memory layout mappings, and runtime subsystem implementations developed during our testing of commercial PS5 software (specifically *World War Z: Aftermath PS5 Base Game*).

---

## 1. System Architecture

```
+-------------------------------------------------------+
|             PlayStation 5 Binary (eboot.elf)          |
+-------------------------------------------------------+
                           |
                           v [sailor-relink]
+-------------------------------------------------------+
|  Windows PE32+ Binary (.exe) with SysV-to-MSABI Thunks |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|                    SailorBox Runtime                  |
|  +--------------------+    +-----------------------+  |
|  |   sailor_kernel    |    |      sailor_gpu       |  |
|  | - libkernel APIs   |    | - AGC Command Buffer  |  |
|  | - Direct Memory    |    |   Decoding & Vulkan   |  |
|  | - Sync Primitives  |    | - Display Swapchain   |  |
|  +--------------------+    +-----------------------+  |
|  +--------------------+    +-----------------------+  |
|  |    sailor_pad      |    |     sailor_audio      |  |
|  | - DualSense Map    |    | - libSceAudioOut      |  |
|  +--------------------+    +-----------------------+  |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|          Host Physical Hardware (Windows x64)         |
+-------------------------------------------------------+
```

### 1.1 SysV vs. Microsoft x64 Calling Convention
PlayStation 5 binaries are compiled using LLVM Clang for FreeBSD/Prospero, adhering to the **System V AMD64 ABI**:
- Function arguments: `RDI`, `RSI`, `RDX`, `RCX`, `R8`, `R9` (floating point in `XMM0`..`XMM7`).
- Callee-saved registers: `RBX`, `RSP`, `RBP`, `R12`, `R13`, `R14`, `R15`.

Windows x64 binaries use the **Microsoft x64 ABI**:
- Function arguments: `RCX`, `RDX`, `R8`, `R9` (floating point in `XMM0`..`XMM3`), with a mandatory 32-byte shadow store on the stack.
- Callee-saved registers: `RBX`, `RBP`, `RDI`, `RSI`, `R12`, `R13`, `R14`, `R15`.

The `sailor-relink` pipeline emits translation thunks into the `.thunk` PE section:
```x86asm
push   rbp
mov    rbp, rsp
sub    rsp, 0x30
mov    [rsp+0x20], r8     ; Preserve Windows shadow space arguments
mov    [rsp+0x28], r9
mov    r9, rcx            ; 4th SysV arg -> 4th Windows arg
mov    r8, rdx            ; 3rd SysV arg -> 3rd Windows arg
mov    rdx, rsi           ; 2nd SysV arg -> 2nd Windows arg
mov    rcx, rdi           ; 1st SysV arg -> 1st Windows arg
mov    r10, <IAT_Address> ; Address of SailorBox runtime DLL export
call   [r10]              ; Invoke SailorBox runtime implementation
mov    rsp, rbp
pop    rbp
ret
```

---

## 2. Reverse Engineering Findings & Subsystem Analysis

### 2.1 The Silent JobManager Failure (Kernel Event Flag Contract)
During engine bootstrap at RVA `0x6F79B0`, the engine invokes its core task scheduler: `JobManager::Initialize` (`0x1050F21`).

At RVA `0x10510D5` through `0x1051106`:
```x86asm
010510D5: call   0x1043AC0                 ; Allocate EventFlag memory struct
010510DA: lea    rsi, [rip + 0x1464C64]    ; Name string ("JobManager_EF")
010510E1: mov    rdi, rax                  ; rdi = EventFlag* dest pointer
010510E4: mov    edx, 0x21                 ; attr = 0x21 (multi-wait, clear)
010510E9: vxorps xmm0, xmm0, xmm0
010510ED: mov    r14, rax
010510F0: vmovups [rax], xmm0
010510F4: mov    qword ptr [rax+0x10], 0
010510FC: xor    ecx, ecx                  ; initPattern = 0
010510FE: xor    r8d, r8d                  ; opt = NULL
01051101: call   sceKernelCreateEventFlag  ; Call runtime export
01051106: add    eax, 0x7FFDFFF4           ; Range check: handle - 0x8002000C
0105110B: cmp    eax, 0x33                 ; Valid range: [0x8002000C .. 0x8002003F]
0105110E: ja     0x1051304                 ; BAILOUT ABORT!
01051114: mov    rcx, 0x0008000000800405   ; Handle validation bitmask
0105111E: bt     rcx, rax                  ; Check if bit (handle - 0x8002000C) is set
01051122: jnc    0x1051304                 ; BAILOUT ABORT!
```

#### The Root Cause:
- On POSIX systems, create functions often return `0` (`SCE_OK`) on success and write the handle via a pointer.
- When `sceKernelCreateEventFlag` returned `0`:
  $$\text{eax} = 0 + \text{0x7FFDFFF4} = \text{0x7FFDFFF4} > \text{0x33}$$
- The conditional jump `ja 0x1051304` branched immediately to the bailout handler.
- The bailout handler left the global `JobManager` instance at `[0x2882160]` uninitialized (all zeroes).
- Consequently, worker thread queues (`[r12 + 0x40]`, `[r12 + 0x158]`, `[r12 + 0x270]`) were never allocated. When the main thread reached `WaitForJob` (`0x10523C0`), it spun indefinitely waiting on null queue pointers.

#### The Clean-Room Fix:
In `sailor_kernel`, `sceKernelCreateEventFlag` was updated to write the internal `EventFlag*` to `*ef` and return `(Sailor::i32)0x8002000C`:
- $\text{0x8002000C} + \text{0x7FFDFFF4} = \text{0x100000000} \equiv 0 \pmod{2^{32}}$.
- $0 \le \text{0x33}$ (passes the `ja` range check).
- Bit 0 of `0x0008000000800405` is `1` (Carry Flag $CF = 1$, bypassing `jnc`).
- `JobManager` initialized completely, unblocking worker thread dispatching.

---

### 2.2 Graphics Backend Gate (`DxCommonInit` at RVA `0xCE9910`)
In the game engine's rendering setup:
```x86asm
00CE9950: mov    al, byte ptr [rip + 0x1F6B692] ; [0x2C54FE8] (Backend active flag)
00CE9956: test   al, al
00CE9958: jz     0xCE9B8B                       ; Bypasses entire DCB submission!
00CE995E: mov    eax, dword ptr [rip + 0x1F6B67C]; [0x2C54FE0] (Ready state)
00CE9964: cmp    eax, 2
00CE9967: jz     0xCE9AA5                       ; Enters main render pass!
```

To prevent the engine from skipping DCB encoding before the runtime's asynchronous display worker initializes, the runtime asserts:
- `byte ptr [0x2C54FE8] = 1`
- `dword ptr [0x2C54FE0] = 2`
- `dword ptr [0x2C54FE4] = 1`

---

### 2.3 Render Pass Skip Branches at RVA `0x6F4F50`
In `eboot.exe`, the master scene graph rendering loop evaluates bitmasks to decide which render passes to execute:
```x86asm
006F4F60: mov    ebx, [rip + 0x2165F7A]         ; [0x285AEE0] (Render pass bitmask)
006F4F66: test   ebx, 0x104
006F4F6C: jz     0x6F5058                       ; Skips 3D world pass
...
006F4FA4: test   ebx, 0x100
006F4FAA: jz     0x6F55B7                       ; Skips UI / post-processing pass
...
006F4FC2: test   al, al                         ; [0x2882688] (Master render enable)
006F4FC4: jz     0x6F5865                       ; Skips backbuffer present
```

The runtime asserts `dword ptr [0x285AEE0] = 0x105`, asserts `byte ptr [0x28823D0] = 1`, and NOPs the conditional skip branches (6 bytes `0x90` each at `0x6F4F6C`, `0x6F4FAA`, and `0x6F4FC4`), guaranteeing uninterrupted execution of all render passes.

---

### 2.4 Guest Framebuffer & Swapchain Memory Layout
Using virtual memory scanners, the committed guest triple buffers were located in process memory:
- **Buffer 0**: `0x116130000` (Size: `0x870000` bytes = 8,847,360 bytes)
- **Buffer 1**: `0x1169A0000` (Size: `0x870000` bytes)
- **Buffer 2**: `0x117210000` (Size: `0x870000` bytes)

Each buffer matches standard 1080p 32-bit RGBA dimensions ($1920 \times 1080 \times 4 = 8,294,400\text{ bytes}$) with 552,960 bytes of tile alignment overhead.

---

## 3. Reverse Engineering Tools Developed

All custom tools are published in the [`tools/`](file:///C:/Developments/@SailorBox/tools/) directory:

1. **`debugger_harness`**: Monitors execution, traps access violations, and dumps CPU registers/stack traces.
2. **`thread_inspector`**: Enumerates running threads, suspends them safely, and identifies active functions vs. synchronization waits.
3. **`framebuffer_extractor`**: Scans committed memory for framebuffers and extracts 32-bit BMP images.
4. **`thunk_disassembler`**: Disassembles ABI parameter translation thunks and inspects IAT bindings.
5. **`opcode_scanner`**: Fast code scanner for relative CALLs and post-call validation sequences.
6. **`symbol_resolver`**: Leverages DbgHelp and PDBs to map memory RVAs back to C++ source lines.
7. **`memory_patcher`**: Injects hot-patches and NOP sleds into running processes via `VirtualProtectEx`.

---

## 4. Current State & Future Roadmap

### Current Milestones Achieved
- [x] Transmutation of PS5 decrypted ELF binaries into functional Windows PE32+ executables.
- [x] Clean-room implementation of core `libkernel` synchronization and memory primitives.
- [x] Emulation of PS5 kernel object contracts (`SceKernelEventFlag`, `SceKernelSema`, `SceKernelEqueue`).
- [x] Multi-threaded engine bootstrap reaching main frame tick loops (`0x5F854F`).
- [x] Allocation and tracking of guest triple-buffered display swapchains.

### Ongoing Engineering Challenges
1. **Asynchronous Asset Pipeline**: Worker threads currently wait for background shader translation and asset decompilation before issuing draw command lists.
2. **PS5 AGC Command Buffer Translation**: Translating low-level AGC PM4 packets into host Vulkan / DirectX 12 draw calls.
3. **Proprietary Shader Recompilation**: Decompiling AMD RDNA2 GPU microcode into standard SPIR-V / DXIL formats.
