@echo off
setlocal enabledelayedexpansion

echo ========================================================================
echo  Building SailorBox Reverse Engineering and Debugging Tools Suite
echo ========================================================================

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to locate MSVC vcvars64.bat.
    exit /b 1
)

set "TOOLS_DIR=%~dp0"
set "BIN_DIR=%TOOLS_DIR%bin"
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

cd /d "%TOOLS_DIR%"

set "CFLAGS=/nologo /O2 /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /std:c++17"

echo Compiling debugger_harness...
cl %CFLAGS% /Fe:"%BIN_DIR%\debugger_harness.exe" debugger_harness.cpp /link /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo Compiling thread_inspector...
cl %CFLAGS% /Fe:"%BIN_DIR%\thread_inspector.exe" thread_inspector.cpp /link /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo Compiling framebuffer_extractor...
cl %CFLAGS% /Fe:"%BIN_DIR%\framebuffer_extractor.exe" framebuffer_extractor.cpp /link /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo Compiling thunk_disassembler...
cl %CFLAGS% /Fe:"%BIN_DIR%\thunk_disassembler.exe" thunk_disassembler.cpp /link /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo Compiling opcode_scanner...
cl %CFLAGS% /Fe:"%BIN_DIR%\opcode_scanner.exe" opcode_scanner.cpp /link /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo Compiling symbol_resolver...
cl %CFLAGS% /Fe:"%BIN_DIR%\symbol_resolver.exe" symbol_resolver.cpp /link dbghelp.lib /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo Compiling memory_patcher...
cl %CFLAGS% /Fe:"%BIN_DIR%\memory_patcher.exe" memory_patcher.cpp /link /SUBSYSTEM:CONSOLE
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

del /Q *.obj 2> nul

echo.
echo ========================================================================
echo  ALL TOOLS COMPILED SUCCESSFULLY!
echo  Binaries located in: %BIN_DIR%
echo ========================================================================
