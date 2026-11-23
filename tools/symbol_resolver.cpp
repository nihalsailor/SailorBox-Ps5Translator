/**
 * ============================================================================
 * Tool: symbol_resolver
 * Description: DbgHelp-based utility that loads binary modules and their PDB
 *              debug symbols, resolving execution RVAs and stack addresses to
 *              demangled C++ symbols, source files, and line numbers.
 * Usage: symbol_resolver.exe <module_path> <pdb_search_path> <hex_address_or_rva>
 * ============================================================================
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: %s <module_path> <pdb_search_path> <hex_address_or_rva>\n", argv[0]);
        printf("Example: %s bin\\sailor_kernel.dll bin\\ 0x64BD6\n", argv[0]);
        return 1;
    }

    const char* modulePath = argv[1];
    const char* pdbSearchPath = argv[2];
    uint64_t targetAddress = strtoull(argv[3], NULL, 16);

    HANDLE hProcess = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);

    if (!SymInitialize(hProcess, pdbSearchPath, FALSE)) {
        printf("[ERROR] SymInitialize failed: %lu\n", GetLastError());
        return 1;
    }

    // Load module with fixed base address
    DWORD64 baseAddress = 0x10000000ULL;
    DWORD64 loadedBase = SymLoadModuleEx(
        hProcess,
        NULL,
        modulePath,
        NULL,
        baseAddress,
        0,
        NULL,
        0
    );

    if (!loadedBase) {
        printf("[ERROR] Failed to load module symbols for %s: %lu\n", modulePath, GetLastError());
        SymCleanup(hProcess);
        return 1;
    }

    printf("[RESOLVER] Module loaded at base: 0x%016llX\n", loadedBase);

    // If query is small, treat as RVA
    DWORD64 queryAddr = (targetAddress < loadedBase) ? (loadedBase + targetAddress) : targetAddress;

    char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = 511;

    DWORD64 displacement = 0;
    if (SymFromAddr(hProcess, queryAddr, &displacement, pSymbol)) {
        printf("\n=======================================================\n");
        printf(" Query Address: 0x%016llX (RVA +0x%llX)\n", queryAddr, queryAddr - loadedBase);
        printf(" Symbol Name:   %s\n", pSymbol->Name);
        printf(" Displacement:  +0x%llX\n", displacement);

        IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(hProcess, queryAddr, &lineDisplacement, &line)) {
            printf(" Source File:   %s\n", line.FileName);
            printf(" Source Line:   %lu (offset +%lu bytes)\n", line.LineNumber, lineDisplacement);
        } else {
            printf(" Source Line:   (No source line info available in PDB)\n");
        }
        printf("=======================================================\n\n");
    } else {
        printf("[WARN] Could not resolve symbol for address 0x%llX: %lu\n", queryAddr, GetLastError());
    }

    SymUnloadModule64(hProcess, loadedBase);
    SymCleanup(hProcess);
    return 0;
}
