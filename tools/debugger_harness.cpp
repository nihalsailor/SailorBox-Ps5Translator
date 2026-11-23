/**
 * ============================================================================
 * Tool: debugger_harness
 * Description: Standalone native x64 Windows debugger harness that launches an
 *              executable, monitors execution, intercepts first/second chance
 *              exceptions, and dumps CPU registers and stack traces on faults.
 * Usage: debugger_harness.exe <executable_path> [working_directory] [arguments]
 * ============================================================================
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <executable_path> [working_directory] [args]\n", argv[0]);
        printf("Example: %s \"C:\\Games\\Target\\game.exe\" \"C:\\Games\\Target\"\n", argv[0]);
        return 1;
    }

    const char* exePath = argv[1];
    const char* workDir = (argc >= 3) ? argv[2] : NULL;

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    char cmdLine[32768] = {};
    if (argc >= 4) {
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\" %s", exePath, argv[3]);
    } else {
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\"", exePath);
    }

    printf("[DEBUGGER] Launching: %s\n", cmdLine);
    if (workDir) printf("[DEBUGGER] Working Dir: %s\n", workDir);

    BOOL ok = CreateProcessA(
        NULL,
        cmdLine,
        NULL,
        NULL,
        FALSE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_CONSOLE,
        NULL,
        workDir,
        &si,
        &pi
    );

    if (!ok) {
        printf("[ERROR] Failed to start process (error %lu)\n", GetLastError());
        return 1;
    }

    printf("[DEBUGGER] Process started (PID: %lu, Main TID: %lu)\n", pi.dwProcessId, pi.dwThreadId);
    printf("[DEBUGGER] Monitoring debug events...\n\n");

    DEBUG_EVENT de;
    bool running = true;

    while (running && WaitForDebugEvent(&de, INFINITE)) {
        DWORD continueStatus = DBG_CONTINUE;

        switch (de.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                printf("[EVENT] Process Created. Base: 0x%p, Entry: 0x%p\n",
                    de.u.CreateProcessInfo.lpBaseOfImage,
                    de.u.CreateProcessInfo.lpStartAddress);
                if (de.u.CreateProcessInfo.hFile) CloseHandle(de.u.CreateProcessInfo.hFile);
                break;
            }

            case CREATE_THREAD_DEBUG_EVENT: {
                // Thread spawned
                break;
            }

            case EXIT_THREAD_DEBUG_EVENT: {
                // Thread exited
                break;
            }

            case EXIT_PROCESS_DEBUG_EVENT: {
                printf("\n[EVENT] Process Exited with code: 0x%08lX\n", de.u.ExitProcess.dwExitCode);
                running = false;
                break;
            }

            case LOAD_DLL_DEBUG_EVENT: {
                if (de.u.LoadDll.hFile) CloseHandle(de.u.LoadDll.hFile);
                break;
            }

            case EXCEPTION_DEBUG_EVENT: {
                EXCEPTION_RECORD& er = de.u.Exception.ExceptionRecord;
                DWORD code = er.ExceptionCode;
                void* addr = er.ExceptionAddress;
                DWORD firstChance = de.u.Exception.dwFirstChance;

                // Ignore benign debugger startup breakpoints
                if (code == STATUS_BREAKPOINT) {
                    continueStatus = DBG_CONTINUE;
                    break;
                }

                // If second chance crash, or fatal access violation / illegal instruction
                if (!firstChance || code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;

                    HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, de.dwThreadId);
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_ALL;
                    if (hThread && GetThreadContext(hThread, &ctx)) {
                        printf("\n=======================================================\n");
                        printf("[EXCEPTION] %s (0x%08lX) at RIP: 0x%p (TID: %lu)\n",
                            firstChance ? "FIRST-CHANCE" : "FATAL CRASH", code, addr, de.dwThreadId);

                        if (code == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
                            const char* op = (er.ExceptionInformation[0] == 0) ? "READING" :
                                             (er.ExceptionInformation[0] == 1) ? "WRITING" : "EXECUTING";
                            printf("  Memory Operation: %s target address 0x%p\n", op, (void*)er.ExceptionInformation[1]);
                        } else if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
                            printf("  Cause: Undefined instruction or unmapped CPU feature\n");
                        }

                        printf("\n  Register Context:\n");
                        printf("    RAX: 0x%016llX   RBX: 0x%016llX\n", ctx.Rax, ctx.Rbx);
                        printf("    RCX: 0x%016llX   RDX: 0x%016llX\n", ctx.Rcx, ctx.Rdx);
                        printf("    RSI: 0x%016llX   RDI: 0x%016llX\n", ctx.Rsi, ctx.Rdi);
                        printf("    RBP: 0x%016llX   RSP: 0x%016llX\n", ctx.Rbp, ctx.Rsp);
                        printf("    R8:  0x%016llX   R9:  0x%016llX\n", ctx.R8, ctx.R9);
                        printf("    R10: 0x%016llX   R11: 0x%016llX\n", ctx.R10, ctx.R11);
                        printf("    R12: 0x%016llX   R13: 0x%016llX\n", ctx.R12, ctx.R13);
                        printf("    R14: 0x%016llX   R15: 0x%016llX\n", ctx.R14, ctx.R15);
                        printf("    EFLAGS: 0x%08lX\n", ctx.EFlags);

                        uint64_t stack[32] = {};
                        SIZE_T bytesRead = 0;
                        if (ReadProcessMemory(pi.hProcess, (void*)ctx.Rsp, stack, sizeof(stack), &bytesRead) && bytesRead > 0) {
                            printf("\n  Stack Dump (at RSP):\n");
                            size_t count = bytesRead / sizeof(uint64_t);
                            for (size_t i = 0; i < count; i += 2) {
                                printf("    [RSP+0x%02zX] 0x%016llX", i * 8, stack[i]);
                                if (i + 1 < count) {
                                    printf("   [RSP+0x%02zX] 0x%016llX\n", (i + 1) * 8, stack[i + 1]);
                                } else {
                                    printf("\n");
                                }
                            }
                        }
                        printf("=======================================================\n\n");
                    }
                    if (hThread) CloseHandle(hThread);

                    if (!firstChance) {
                        running = false;
                    }
                }
                break;
            }

            default:
                break;
        }

        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, continueStatus);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    printf("[DEBUGGER] Session concluded.\n");
    return 0;
}
