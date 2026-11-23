#include "common/Types.hpp"
#include "common/Logger.hpp"
#include "common/Nid.hpp"
#include "relinker/Relinker.hpp"
#include "relinker/ElfReader.hpp"
#include "relinker/DynamicLinker.hpp"
#include "tools/GamePatch.hpp"
#include "tools/StubGenerator.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <format>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace Sailor;
using namespace Sailor::Tools;

void PrintBanner() {
    std::cout << R"(
  ███████╗ █████╗ ██╗██╗      ██████╗ ██████╗  ██████╗ ██╗  ██╗
  ██╔════╝██╔══██╗██║██║     ██╔═══██╗██╔══██╗██╔═══██╗╚██╗██╔╝
  ███████╗███████║██║██║     ██║   ██║██████╔╝██║   ██║ ╚███╔╝ 
  ╚════██║██╔══██║██║██║     ██║   ██║██╔══██╗██║   ██║ ██╔██╗ 
  ███████║██║  ██║██║███████╗╚██████╔╝██████╔╝╚██████╔╝██╔╝ ██╗
  ╚══════╝╚═╝  ╚═╝╚═╝╚══════╝ ╚═════╝ ╚═════╝  ╚═════╝ ╚═╝  ╚═╝
   Next-Generation Native PS5 Relinker & Runtime for Windows (x64)
)" << std::endl;
}

void PrintUsage() {
    std::cout << "Usage: sailor <command> [arguments]\n\n"
              << "Commands:\n"
              << "  run <game_dir_or_eboot> [--patch <patches.toml>]\n"
              << "      Auto-relinks all PRXs and EBOOT, applies patches, sets up runtime, and launches game.\n\n"
              << "  relink <eboot.bin> [output.exe]\n"
              << "      Transmutes a PS5 ELF executable or PRX library directly into a native Windows PE (PE32+).\n\n"
              << "  relink-all <game_dir>\n"
              << "      Batch-transmutes EBOOT and all PRX/SPRX modules across the entire game directory into DLLs.\n\n"
              << "  patch <target_exe> <patches.toml>\n"
              << "      Applies byte patches (60 FPS unlock, intro skip, memory tweaks) from TOML.\n\n"
              << "  stubgen <unhandled_apis.log> [out_dir]\n"
              << "      Auto-generates compilation-ready C++ templates for newly discovered APIs.\n\n"
              << "  info <eboot.bin>\n"
              << "      Inspects and dumps PS5 ELF headers, segments, DT_NEEDED libraries, and NIDs.\n\n"
              << "Options:\n"
              << "  -v, --verbose    Enable verbose diagnostic logging\n"
              << "  -h, --help       Show this help dialog\n"
              << std::endl;
}

int CommandInfo(const std::string& elfPath) {
    std::cout << "[INFO] Inspecting PS5 ELF binary: " << elfPath << std::endl;

    ElfReader reader;
    if (!reader.Load(elfPath)) {
        std::cerr << "[ERROR] Failed to load ELF file: " << elfPath << std::endl;
        return 1;
    }

    std::cout << "  -> Architecture: x86-64 (AMD64)\n"
              << "  -> ELF Class: 64-bit Little Endian\n"
              << std::format("  -> Entry Point Address: 0x{:016X}\n", reader.GetEntryPoint())
              << std::format("  -> Program Segments: {} segments\n", reader.GetSegments().size());

    int segIdx = 0;
    for (const auto& seg : reader.GetSegments()) {
        std::string flags;
        if (seg.flags & 4) flags += 'R';
        if (seg.flags & 2) flags += 'W';
        if (seg.flags & 1) flags += 'X';
        std::cout << std::format("     [{}] Flags [{}] VAddr 0x{:016X} MemSz 0x{:08X} DataSz 0x{:08X}\n",
            segIdx++, flags, seg.vaddr, seg.memsz, seg.data.size());
    }

    const auto& dynInfo = reader.GetDynamicLinker().GetInfo();
    std::cout << "\n[Imported Libraries (DT_NEEDED)]:\n";
    for (const auto& lib : dynInfo.neededLibraries) {
        std::cout << "  - " << lib << "\n";
    }

    std::cout << std::format("\n[Dynamic Symbols]: {} symbols found\n", dynInfo.symbolNames.size());
    std::cout << std::format("[Relocations]: {} relocations parsed\n", dynInfo.relocations.size());
    std::cout << std::format("[PLT Relocations]: {} PLT relocations parsed\n", dynInfo.pltRelocations.size());

    std::cout << "\n[PLT Relocations 1040-1060]:\n";
    for (size_t idx = 1040; idx <= 1060 && idx < dynInfo.pltRelocations.size(); ++idx) {
        const auto& rel = dynInfo.pltRelocations[idx];
        std::string name = rel.symbolIndex < dynInfo.symbolNames.size() ? dynInfo.symbolNames[rel.symbolIndex] : "UNKNOWN";
        std::string nidStr = rel.symbolIndex < dynInfo.symbolNids.size() ? dynInfo.symbolNids[rel.symbolIndex].ToString() : "NONE";
        std::cout << std::format("  [{:3}] Offset: 0x{:X}, Sym: {} (NID: {})\n", idx, rel.offset, name, nidStr);
    }

    return 0;
}

int CommandSymbols(const std::string& elfPath, const std::string& filter) {
    ElfReader reader;
    if (!reader.Load(elfPath)) {
        std::cerr << "[ERROR] Failed to load: " << elfPath << std::endl;
        return 1;
    }
    const auto& dynInfo = reader.GetDynamicLinker().GetInfo();
    std::cout << std::format("[SYMBOLS] {} dynamic symbols in '{}':\n", dynInfo.symbolNames.size(), elfPath);
    for (size_t idx = 0; idx < dynInfo.pltRelocations.size(); ++idx) {
        if (dynInfo.pltRelocations[idx].symbolIndex == 585 || dynInfo.pltRelocations[idx].symbolIndex == 608) {
            std::cout << std::format("PLT Reloc [{}] offset 0x{:X} for sym {}\n", idx, dynInfo.pltRelocations[idx].offset, dynInfo.pltRelocations[idx].symbolIndex);
        }
    }
    for (size_t i = 0; i < dynInfo.symbolNames.size(); ++i) {
        const std::string& name = dynInfo.symbolNames[i];
        std::string nidStr = i < dynInfo.symbolNids.size() ? dynInfo.symbolNids[i].ToString() : "";
        if (!filter.empty() && name.find(filter) == std::string::npos && nidStr.find(filter) == std::string::npos) continue;
        u64 val = i < dynInfo.symbolValues.size() ? dynInfo.symbolValues[i] : 0;
        std::cout << std::format("  [{:4}] Value: 0x{:08X} NID: {:11} Sym: {}\n", i, val, nidStr, name);
    }
    return 0;
}

int CommandRelink(const std::string& inputElf, std::string outputExe) {
    if (outputExe.empty()) {
        std::filesystem::path p(inputElf);
        p.replace_extension(".exe");
        outputExe = p.string();
    }

    std::cout << std::format("[RELINK] Transmuting '{}' -> '{}'...\n", inputElf, outputExe);

    Relinker relinker;
    RelinkOptions opts;
    if (!relinker.Relink(inputElf, outputExe, opts)) {
        std::cerr << "[ERROR] Transmutation failed for: " << outputExe << std::endl;
        return 1;
    }

    std::cout << std::format("[SUCCESS] Native Windows executable generated at '{}'!\n", outputExe);
    return 0;
}

size_t BatchRelinkDirectory(const std::filesystem::path& baseDir) {
    std::cout << std::format("[RELINK-ALL] Scanning directory '{}' for PS5 PRX/SPRX modules...\n", baseDir.string());
    size_t count = 0;
    std::error_code ec;

    Relinker relinker;
    RelinkOptions dllOpts{ .isDll = true };

    for (const auto& entry : std::filesystem::recursive_directory_iterator(baseDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file(ec)) continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".prx" || ext == ".sprx") {
            std::filesystem::path prxPath = entry.path();
            std::filesystem::path dllPath = prxPath;
            dllPath.replace_extension(".dll");

            bool needsRelink = true;
            if (std::filesystem::exists(dllPath, ec)) {
                auto prxTime = std::filesystem::last_write_time(prxPath, ec);
                auto dllTime = std::filesystem::last_write_time(dllPath, ec);
                if (dllTime >= prxTime && std::filesystem::file_size(dllPath, ec) > 0) {
                    needsRelink = false;
                }
            }

            if (needsRelink) {
                std::cout << std::format("  -> Transmuting module '{}' -> '{}'...\n",
                    std::filesystem::relative(prxPath, baseDir, ec).string(),
                    dllPath.filename().string());
                if (relinker.Relink(prxPath, dllPath, dllOpts)) {
                    count++;
                    std::filesystem::path rootDll = baseDir / dllPath.filename();
                    if (rootDll != dllPath) {
                        std::filesystem::copy_file(dllPath, rootDll, std::filesystem::copy_options::overwrite_existing, ec);
                    }
                } else {
                    std::cerr << std::format("  [WARN] Failed to relink module '{}'\n", prxPath.string());
                }
            } else {
                std::filesystem::path rootDll = baseDir / dllPath.filename();
                if (rootDll != dllPath && !std::filesystem::exists(rootDll, ec)) {
                    std::filesystem::copy_file(dllPath, rootDll, std::filesystem::copy_options::overwrite_existing, ec);
                }
            }
        }
    }

    if (count > 0) {
        std::cout << std::format("[SUCCESS] Successfully relinked {} PRX module(s)!\n", count);
    } else {
        std::cout << "[INFO] All PRX modules are up to date.\n";
    }
    return count;
}

int CommandRelinkAll(const std::string& path) {
    std::filesystem::path target(path);
    if (!std::filesystem::exists(target)) {
        std::cerr << "[ERROR] Path does not exist: " << path << std::endl;
        return 1;
    }

    std::filesystem::path gameDir = std::filesystem::is_directory(target) ? target : target.parent_path();

    // 1. Relink EBOOT if present
    for (const char* ebootName : { "eboot.bin", "eboot.elf" }) {
        std::filesystem::path ebootPath = gameDir / ebootName;
        if (std::filesystem::exists(ebootPath)) {
            std::filesystem::path outExe = gameDir / "eboot.exe";
            CommandRelink(ebootPath.string(), outExe.string());
            break;
        }
    }

    // 2. Batch relink all PRX/SPRX modules
    BatchRelinkDirectory(gameDir);
    return 0;
}

int CommandPatch(const std::string& targetPath, const std::string& patchToml) {
    std::cout << std::format("[PATCH] Applying '{}' to '{}'...\n", patchToml, targetPath);

    PatchEngine engine;
    if (!engine.LoadFromFile(patchToml)) {
        std::cerr << "[ERROR] Failed to load patches from: " << patchToml << std::endl;
        return 1;
    }

    size_t applied = engine.PatchFile(targetPath);
    std::cout << std::format("[SUCCESS] Successfully applied {} patches!\n", applied);
    return 0;
}

int CommandStubGen(const std::string& logPath, std::string outDir) {
    if (outDir.empty()) {
        outDir = "generated_stubs";
    }

    std::cout << std::format("[STUBGEN] Parsing log '{}' -> output dir '{}'...\n", logPath, outDir);

    StubGenerator generator;
    size_t parsed = generator.ParseLogFile(logPath);
    if (parsed == 0) {
        std::cout << "[WARN] No unhandled stubs found in log.\n";
        return 0;
    }

    // Group by module
    std::unordered_map<std::string, bool> modules;
    for (const auto& entry : generator.GetEntries()) {
        modules[entry.moduleName] = true;
    }

    for (const auto& [mod, _] : modules) {
        generator.GenerateFiles(mod, outDir);
    }

    std::cout << std::format("[SUCCESS] Generated stubs for {} modules ({} total APIs)!\n",
        modules.size(), parsed);
    return 0;
}

int CommandRun(const std::string& gamePath, const std::string& patchFile) {
    std::filesystem::path target(gamePath);
    std::filesystem::path gameDir = std::filesystem::is_directory(target) ? target : target.parent_path();
    std::string exeToLaunch;

    // Batch relink all PRX modules found in game directory
    if (std::filesystem::exists(gameDir)) {
        BatchRelinkDirectory(gameDir);
    }

    if (std::filesystem::is_directory(target)) {
        // Look for eboot.bin, eboot.elf, or eboot.exe
        if (std::filesystem::exists(target / "eboot.exe")) {
            exeToLaunch = (target / "eboot.exe").string();
        } else if (std::filesystem::exists(target / "eboot.bin")) {
            const std::string elf = (target / "eboot.bin").string();
            exeToLaunch = (target / "eboot.exe").string();
            if (CommandRelink(elf, exeToLaunch) != 0) {
                return 1;
            }
        } else if (std::filesystem::exists(target / "eboot.elf")) {
            const std::string elf = (target / "eboot.elf").string();
            exeToLaunch = (target / "eboot.exe").string();
            if (CommandRelink(elf, exeToLaunch) != 0) {
                return 1;
            }
        } else {
            std::cerr << "[ERROR] No eboot.bin, eboot.elf, or eboot.exe found in directory: " << gamePath << std::endl;
            return 1;
        }
    } else {
        if (target.extension() == ".bin" || target.extension() == ".elf") {
            exeToLaunch = target.replace_extension(".exe").string();
            if (CommandRelink(gamePath, exeToLaunch) != 0) {
                return 1;
            }
        } else {
            exeToLaunch = gamePath;
        }
    }

    // Apply patch file if provided
    if (!patchFile.empty() && std::filesystem::exists(patchFile)) {
        CommandPatch(exeToLaunch, patchFile);
    }

    std::cout << std::format("\n[LAUNCH] Starting native execution of '{}'...\n", exeToLaunch);

    // Set SAILOR_APP_DIR so guest VFS routes /app0 to game directory
    SetEnvironmentVariableA("SAILOR_APP_DIR", gameDir.string().c_str());

    // Ensure runtime DLLs can be located by child process
    char selfPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, selfPath, MAX_PATH);
    std::filesystem::path sailorBinDir = std::filesystem::path(selfPath).parent_path();
    SetDllDirectoryA(sailorBinDir.string().c_str());

    // Prepend sailorBinDir to PATH so child process can resolve runtime DLLs
    char currentPath[32768] = {};
    DWORD pathLen = GetEnvironmentVariableA("PATH", currentPath, sizeof(currentPath));
    std::string newPath = sailorBinDir.string();
    if (pathLen > 0) {
        newPath += ";";
        newPath += currentPath;
    }
    SetEnvironmentVariableA("PATH", newPath.c_str());

    std::filesystem::path launchPath(exeToLaunch);
    std::string workDir = launchPath.parent_path().string();
    const char* lpWorkDir = workDir.empty() ? nullptr : workDir.c_str();

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.lpDesktop = const_cast<char*>("WinSta0\\default");
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::string cmdLine = "\"" + exeToLaunch + "\"";
    // Launch transmuted process directly with inherited handles and command line
    BOOL ok = CreateProcessA(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        lpWorkDir,
        &si,
        &pi
    );

    if (!ok) {
        std::cerr << "[ERROR] Failed to launch process (Error " << GetLastError() << ")\n" << std::flush;
        return 1;
    }

    std::cout << "[INFO] Process running with PID " << pi.dwProcessId << ".\n" << std::flush;
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::cout << std::format("[INFO] Process exited with code {}\n", exitCode);
    return static_cast<int>(exitCode);
}

int main(int argc, char* argv[]) {
    PrintBanner();

    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            Logger::Instance().SetLevel(LogLevel::Debug);
        } else {
            args.push_back(std::move(arg));
        }
    }

    if (args.empty()) {
        PrintUsage();
        return 0;
    }

    const std::string& command = args[0];

    if (command == "-h" || command == "--help" || command == "help") {
        PrintUsage();
        return 0;
    }

    if (command == "info") {
        if (args.size() < 2) {
            std::cerr << "Usage: sailor info <eboot.bin>\n";
            return 1;
        }
        return CommandInfo(args[1]);
    }

    if (command == "symbols") {
        if (args.size() < 2) {
            std::cerr << "Usage: sailor symbols <elf_or_prx> [filter]\n";
            return 1;
        }
        std::string filter = (args.size() >= 3) ? args[2] : "";
        return CommandSymbols(args[1], filter);
    }

    if (command == "relink") {
        if (args.size() < 2) {
            std::cerr << "Usage: sailor relink <eboot.bin> [output.exe]\n";
            return 1;
        }
        std::string out = (args.size() >= 3) ? args[2] : "";
        return CommandRelink(args[1], out);
    }

    if (command == "relink-all") {
        if (args.size() < 2) {
            std::cerr << "Usage: sailor relink-all <game_dir>\n";
            return 1;
        }
        return CommandRelinkAll(args[1]);
    }

    if (command == "patch") {
        if (args.size() < 3) {
            std::cerr << "Usage: sailor patch <target_exe> <patches.toml>\n";
            return 1;
        }
        return CommandPatch(args[1], args[2]);
    }

    if (command == "stubgen") {
        if (args.size() < 2) {
            std::cerr << "Usage: sailor stubgen <unhandled_apis.log> [out_dir]\n";
            return 1;
        }
        std::string outDir = (args.size() >= 3) ? args[2] : "generated_stubs";
        return CommandStubGen(args[1], outDir);
    }

    if (command == "run") {
        if (args.size() < 2) {
            std::cerr << "Usage: sailor run <game_dir_or_eboot> [--patch <patches.toml>]\n";
            return 1;
        }
        std::string patchFile;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--patch" && i + 1 < args.size()) {
                patchFile = args[++i];
            }
        }
        return CommandRun(args[1], patchFile);
    }

    std::cerr << "[ERROR] Unknown command: " << command << "\n";
    PrintUsage();
    return 1;
}
