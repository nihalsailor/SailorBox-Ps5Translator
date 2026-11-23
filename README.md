# SailorBox: PlayStation 5 Native Compatibility Runtime

**SailorBox** is an open-source, high-performance clean-room compatibility runtime and binary relinker for PlayStation 5 (Prospero OS / x86-64) executables on Windows x64.

Unlike traditional emulators that translate CPU instructions through JITs or interpreters, SailorBox executes PS5 x86-64 machine code directly on the host physical CPU with **zero CPU emulation overhead**, translating operating system syscalls and low-level graphics commands into native Windows APIs (Win32, DirectX 12, Vulkan).

---

## Architecture Overview

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

1. **Relinker (`sailor-relink`)**: Parses decrypted PS5 ELF binaries, resolves imported function NIDs, synthesizes System V to Microsoft x64 calling convention translation thunks, and emits valid Windows PE32+ (`.exe`) executables.
2. **Kernel Runtime (`sailor_kernel`)**: Clean-room C++20 implementation of PS5 kernel services:
   - Direct memory mapping (`VirtualAlloc`, flexible address spaces, batch maps).
   - POSIX / Prospero thread management and TLS setup.
   - Synchronization primitives (Mutexes, Condition Variables, Semaphores, Event Flags).
3. **Graphics & Display (`sailor_gpu`)**:
   - Manages triple-buffered display swapchains.
   - Translates low-level PlayStation 5 AGC (AMD RDNA2) command buffers into Vulkan draw calls.
4. **Input & Audio (`sailor_pad`, `sailor_audio`)**:
   - DualSense controller state mapping to XInput / DirectInput.
   - Low-latency multi-channel PCM audio rendering.

---

## Reverse Engineering Tools Suite

The repository includes a suite of standalone x64 reverse engineering, debugging, and binary analysis utilities located in [`tools/`](file:///C:/Developments/@SailorBox/tools/):

* **`debugger_harness`**: Intercepts first/second chance exceptions, dumping registers and stack memory.
* **`thread_inspector`**: Inspects all active threads in a running process and identifies call paths.
* **`framebuffer_extractor`**: Scans committed memory for framebuffers and extracts 32-bit BMP images.
* **`thunk_disassembler`**: Analyzes SysV-to-MSABI marshalling thunks and resolves IAT symbols.
* **`opcode_scanner`**: Scans executable sections for relative CALLs and post-call validation patterns.
* **`symbol_resolver`**: Leverages DbgHelp to resolve execution RVAs back to C++ source lines.
* **`memory_patcher`**: Hot-patches running processes with NOP sleds or custom byte sequences.

To build the tools suite:
```cmd
tools\build_tools.bat
```

For detailed documentation, see the [Tools README](file:///C:/Developments/@SailorBox/tools/README.md).

---

## Deep Dive & Research Report

A detailed technical report documenting our binary analyses, assembly instruction discoveries, memory layouts, and runtime fixes is available in:
- [**Reverse Engineering & Research Report (`REVERSE_ENGINEERING_REPORT.md`)**](file:///C:/Developments/@SailorBox/REVERSE_ENGINEERING_REPORT.md)

Key technical topics covered:
- Resolving the `JobManager` contract and fixing the `sceKernelCreateEventFlag` return value check.
- Bypassing rendering gates (`DxCommonInit` at RVA `0xCE9950`).
- Managing guest triple-buffered swapchains in virtual memory.
- SysV vs Microsoft x64 register translation thunks.

---

## Building SailorBox

### Prerequisites
- Windows 10/11 x64
- Visual Studio 2022 / Build Tools 2026 (MSVC v143+)
- CMake 3.24+ & Ninja build system
- Vulkan SDK 1.3+

### Build Commands
```cmd
mkdir build
cd build
cmake -GNinja -DCMAKE_BUILD_TYPE=Release ..
ninja
```

---

## License

This project is licensed under the MIT License. Clean-room engineering principles are strictly maintained throughout all runtime modules.
