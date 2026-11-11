@echo off
rem build.bat: builds Rig on Windows.
rem
rem   build.bat            a debug build (-O0)
rem   build.bat release    an optimised build (-O2)
rem
rem Unity build: each program is one translation unit (src\rig.cpp includes
rem every source file), so this script is the whole build system. LLVM comes
rem from scripts\get_llvm.bat, in deps\llvm-windows unless RIG_LLVM_DIR says
rem otherwise, and is linked statically. clang must be on the PATH.

setlocal
cd /d "%~dp0"

set LLVM=%RIG_LLVM_DIR%
if "%LLVM%"=="" set LLVM=deps\llvm-windows
set OPT=-O0
if "%1"=="release" set OPT=-O2

rem (A goto, not a ( ) block: %LLVM% may hold a parenthesis, as in
rem "Program Files (x86)", which would end the block early.)
if not exist "%LLVM%\bin\llvm-config.exe" goto no_llvm

rem clang finds Visual Studio's headers and libraries itself, but when another
rem product of the Visual Studio installer is newer (SQL Server Management
rem Studio, say) it can pick that one, which has no C++ tools. So unless a
rem developer prompt already set it, point clang at the newest installation
rem that has them, through VCToolsInstallDir.
if not defined VCToolsInstallDir call :find_msvc || exit /b 1

rem The LLVM components Rig uses: the IR (core), and the x86 target that
rem turns it into object files. llvm-config names them (LLVMCore.lib, ...);
rem each becomes -lLLVMCore, found through -L, so a path with spaces works.
set LLVM_LIBS=
for /f "delims=" %%i in ('"%LLVM%\bin\llvm-config.exe" --link-static --libnames core target x86codegen x86asmparser x86desc x86info') do set LIBNAMES=%%i
for %%l in (%LIBNAMES%) do call set LLVM_LIBS=%%LLVM_LIBS%% -l%%~nl

rem The Windows libraries LLVM uses. (llvm-config --system-libs also names
rem zlib, zstd and libxml2, which the release was built with but doesn't
rem ship; the parts of LLVM Rig links don't need them.)
set SYSTEM_LIBS=-lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lws2_32 -lntdll

rem LLVM's release libraries are built against the static C runtime (/MT),
rem so Rig's compiler is too: mixing the two fails to link (LNK2038).
set FLAGS=-std=c++23 -fno-exceptions -fno-rtti -g %OPT% -Wall -Wextra -fms-runtime-lib=static -D_CRT_SECURE_NO_WARNINGS -I"%LLVM%\include"

if not exist bin mkdir bin
clang++ %FLAGS% src\rig.cpp -L"%LLVM%\lib" %LLVM_LIBS% %SYSTEM_LIBS% -o bin\rig.exe || exit /b 1

rem Rig falls back to LLVM's lld-link when Visual Studio has no link.exe.
copy /y "%LLVM%\bin\lld-link.exe" bin\lld-link.exe >nul || exit /b 1
exit /b 0

:find_msvc
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" goto no_msvc
set VS=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS goto no_msvc
set /p MSVC_VERSION=<"%VS%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
set VCToolsInstallDir=%VS%\VC\Tools\MSVC\%MSVC_VERSION%\
exit /b 0

:no_llvm
echo build.bat: LLVM not found in %LLVM%; run scripts\get_llvm.bat first 1>&2
exit /b 1

:no_msvc
echo build.bat: Visual Studio with the C++ build tools wasn't found; install the Visual Studio Build Tools with the "Desktop development with C++" workload 1>&2
exit /b 1
