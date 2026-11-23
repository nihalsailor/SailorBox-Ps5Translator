#include "common/Logger.hpp"
#include "relinker/Relinker.hpp"
#include <iostream>

void PrintUsage(const char* exeName) {
    std::cout << "SailorBox Relinker - Native PS5-to-Windows Executable Transmuter\n";
    std::cout << "Usage: " << exeName << " [options] <input.elf> <output.exe>\n\n";
    std::cout << "Options:\n";
    std::cout << "  --help              Print this help menu and exit\n";
    std::cout << "  --audit             Inspect and report imported NIDs without writing output\n";
    std::cout << "  --gui               Generate Windows GUI subsystem PE rather than Console\n";
    std::cout << "  --base <address>    Set custom PE image base (default: 0x140000000)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string_view firstArg = argv[1];
    if (firstArg == "--help" || firstArg == "-h") {
        PrintUsage(argv[0]);
        return 0;
    }

    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    Sailor::RelinkOptions options;
    bool auditOnly = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--audit") {
            auditOnly = true;
        } else if (arg == "--gui") {
            options.emitGuiSubsystem = true;
        } else if (arg == "--base" && i + 1 < argc) {
            options.fixedBaseAddress = std::stoull(argv[++i], nullptr, 0);
        } else if (!arg.starts_with("--")) {
            if (inputPath.empty()) {
                inputPath = arg;
            } else if (outputPath.empty()) {
                outputPath = arg;
            }
        }
    }

    if (inputPath.empty()) {
        std::cerr << "Error: No input ELF specified.\n";
        PrintUsage(argv[0]);
        return 1;
    }

    if (auditOnly) {
        Sailor::ElfReader reader;
        if (!reader.Load(inputPath)) {
            std::cerr << "Audit failed: Could not read ELF file.\n";
            return 2;
        }
        std::cout << "Audit Successful:\n";
        std::cout << "  Entrypoint: 0x" << std::hex << reader.GetEntryPoint() << "\n";
        std::cout << "  Segments:   " << std::dec << reader.GetSegments().size() << "\n";
        std::cout << "  Imports:    " << std::dec << reader.GetImports().size() << "\n";
        return 0;
    }

    if (outputPath.empty()) {
        outputPath = inputPath;
        outputPath.replace_extension(".exe");
    }

    Sailor::Relinker relinker;
    if (!relinker.Relink(inputPath, outputPath, options)) {
        std::cerr << "Relinking failed.\n";
        return 3;
    }

    std::cout << "Transmutation completed: " << outputPath << "\n";
    return 0;
}
