# Native Reverse Engineering & Debugging Tools Suite

This directory contains a suite of standalone, reusable x64 reverse engineering, debugging, and memory analysis tools developed for native Windows binary analysis, ABI compatibility layers, and emulator/runtime development.

---

## Tool Overview

| Tool Binary | Source File | Description |
| :--- | :--- | :--- |
| **`debugger_harness.exe`** | [`debugger_harness.cpp`](file:///C:/Developments/@SailorBox/tools/debugger_harness.cpp) | Native Windows x64 process debugger. Traps access violations (`0xC0000005`) and illegal instructions (`0xC000001D`), dumping full CPU register contexts and stack memory. |
| **`thread_inspector.exe`** | [`thread_inspector.cpp`](file:///C:/Developments/@SailorBox/tools/thread_inspector.cpp) | Multi-threaded process inspector. Safely snapshots all running threads, reads context registers, and unwinds stack frames to diagnose deadlocks and wait states. |
| **`framebuffer_extractor.exe`** | [`framebuffer_extractor.cpp`](file:///C:/Developments/@SailorBox/tools/framebuffer_extractor.cpp) | Scans committed virtual memory for graphics framebuffers (1080p, 4K, 720p), performs statistical pixel analysis, and exports valid frames to 32-bit BMP images. |
| **`thunk_disassembler.exe`** | [`thunk_disassembler.cpp`](file:///C:/Developments/@SailorBox/tools/thunk_disassembler.cpp) | PE binary import directory analyzer. Disassembles parameter marshalling thunks (SysV-to-Microsoft ABI) and maps IAT entry addresses to exported library symbols. |
| **`opcode_scanner.exe`** | [`opcode_scanner.cpp`](file:///C:/Developments/@SailorBox/tools/opcode_scanner.cpp) | Fast `.text` code section scanner. Locates relative `CALL` opcodes (`0xE8`), extracts relative jump offsets, and analyzes post-call return value validation patterns. |
| **`symbol_resolver.exe`** | [`symbol_resolver.cpp`](file:///C:/Developments/@SailorBox/tools/symbol_resolver.cpp) | DbgHelp symbol resolver. Loads binary modules and PDB files, resolving memory RVAs and instruction pointers to demangled C++ symbol names, source files, and line numbers. |
| **`memory_patcher.exe`** | [`memory_patcher.cpp`](file:///C:/Developments/@SailorBox/tools/memory_patcher.cpp) | Live process memory hot-patcher. Modifies memory page protections with `VirtualProtectEx`, injects NOP sleds or custom machine instructions, and verifies patch integrity. |

---

## Building the Tools

Run the automated build script from a standard command prompt:
```cmd
tools\build_tools.bat
```
Binaries will be compiled using MSVC (`cl.exe`) into `tools\bin\`.

---

## Usage Examples

### 1. Debugger Harness
Launch a target process under the debugger to trap crashes:
```cmd
tools\bin\debugger_harness.exe "C:\Path\To\game.exe" "C:\Path\To"
```

### 2. Thread Inspector
Diagnose all active threads and stack references in a running process:
```cmd
tools\bin\thread_inspector.exe game.exe
```

### 3. Framebuffer Extractor
Automatically scan for committed 1080p framebuffers and save BMP dumps:
```cmd
tools\bin\framebuffer_extractor.exe game.exe
```
Or capture a specific memory address:
```cmd
tools\bin\framebuffer_extractor.exe game.exe 1920 1080 0x116130000
```

### 4. Thunk Disassembler
Analyze all kernel and graphics imports in a PE binary:
```cmd
tools\bin\thunk_disassembler.exe eboot.exe Kernel
```

### 5. Opcode Scanner
Find all calls to a specific stub RVA and inspect post-call instruction bytes:
```cmd
tools\bin\opcode_scanner.exe eboot.exe call 0x1E76CB0
```

### 6. Symbol Resolver
Resolve an RVA to source code location using debug PDBs:
```cmd
tools\bin\symbol_resolver.exe bin\sailor_kernel.dll bin\ 0x64BD6
```

### 7. Memory Patcher
Inject 6 NOPs into a running process to bypass a conditional bailout branch:
```cmd
tools\bin\memory_patcher.exe game.exe 0x1406F4F6C nop:6
```
