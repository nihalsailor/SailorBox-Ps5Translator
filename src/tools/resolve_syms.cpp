#include <windows.h>
#include <dbghelp.h>
#include <iostream>
#include <iomanip>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

int main() {
    HANDLE hProcess = GetCurrentProcess();
    SymInitialize(hProcess, "build\\bin", TRUE);

    DWORD64 base = (DWORD64)LoadLibraryExA("build\\bin\\sailor_gpu.dll", NULL, DONT_RESOLVE_DLL_REFERENCES);
    std::cout << "Loaded sailor_gpu.dll at 0x" << std::hex << base << "\n";

    SymLoadModuleEx(hProcess, NULL, "build\\bin\\sailor_gpu.dll", NULL, base, 0, NULL, 0);

    std::vector<DWORD64> offsets = {
        0xd87cc, 0xd87f9
    };

    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)]{};
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    for (DWORD64 off : offsets) {
        DWORD64 addr = base + off;
        DWORD64 displacement = 0;
        if (SymFromAddr(hProcess, addr, &displacement, pSymbol)) {
            std::cout << "0x" << std::hex << off << " -> " << pSymbol->Name 
                      << " + 0x" << displacement << "\n";
        } else {
            std::cout << "0x" << std::hex << off << " -> [unknown] error: " << GetLastError() << "\n";
        }
    }

    SymCleanup(hProcess);
    return 0;
}
