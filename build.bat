@echo off
rem build.bat: builds Rig on Windows.
rem
rem   build.bat            a debug build (-O0)
rem   build.bat release    an optimised build (-O2)
rem
rem Unity build: each program is one translation unit (src\rig.cpp includes
rem every source file), so this script is the whole build system. LLVM comes
rem from scripts\get_llvm.bat, in deps\llvm-windows unless RIG_LLVM_DIR says
rem otherwise, and is linked statically. The compiler that builds Rig comes
rem from there too: the pinned release's clang++.exe, so every build, here or
rem on a release's runner, is made by the same clang whatever else is
rem installed. CXX names another compiler's program to try one
rem (set "CXX=C:\Program Files\LLVM\bin\clang++.exe"). Visual Studio, or its
rem Build Tools, is still needed: clang compiles against its C and C++
rem runtime's and the Windows SDK's headers and libraries and links with its
rem link.exe (:find_msvc, below).
rem
rem Every build is stamped with Rig's version, from the VERSION file, and
rem the commit it is built from, from git (:stamp, below; `rig version`
rem shows them). build.sh does the same.

setlocal
cd /d "%~dp0"

set LLVM=%RIG_LLVM_DIR%
if "%LLVM%"=="" set LLVM=deps\llvm-windows
set OPT=-O0
set BUILD=debug
if "%1"=="release" set OPT=-O2
if "%1"=="release" set BUILD=release

call :stamp || exit /b 1

rem (A goto, not a ( ) block: %LLVM% may hold a parenthesis, as in
rem "Program Files (x86)", which would end the block early.)
if not exist "%LLVM%\bin\llvm-config.exe" goto no_llvm

rem The compiler: CXX if set, else the pinned clang++.exe. A
rem deps\llvm-windows made before get_llvm.bat kept clang has LLVM but no
rem compiler. CXX may be given with quotes around it or without: they're
rem taken off, and "%CXX%" below puts them back. (That line is reached
rem only when CXX is set: %CXX:"=% of an unset CXX isn't empty.)
set PINNED_CXX=
if not defined CXX goto pinned_cxx
set "CXX=%CXX:"=%"
goto have_cxx
:pinned_cxx
set "CXX=%LLVM%\bin\clang++.exe"
set PINNED_CXX=1
if not exist "%CXX%" goto no_clang
:have_cxx
rem Which compiler made this build, first in its log: "clang version 23.1.5
rem ...", the first line clang prints. A compiler that doesn't answer stops
rem here, with what it said on stderr left in place (for /f takes only its
rem stdout), since that names the cause, and a message naming where the
rem compiler came from: the pinned clang, or CXX. (for /f runs its command
rem with cmd /c, which takes the quotes off a command that starts with one
rem and holds a parenthesis, as a path may; starting it with call keeps
rem them.)
set CXX_VERSION=
for /f "delims=" %%v in ('call "%CXX%" --version') do if not defined CXX_VERSION set "CXX_VERSION=%%v"
if not defined CXX_VERSION goto bad_cxx
echo build.bat: %CXX%: %CXX_VERSION%

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

rem The Windows libraries LLVM uses, and COM's strings (finding Visual
rem Studio). llvm-config --system-libs also names zlib, zstd and libxml2,
rem which the release was built with but doesn't ship: the parts of LLVM Rig
rem links don't need libxml2, and src\llvm_compression_stubs.cpp stands in
rem for zlib and zstd.
set SYSTEM_LIBS=-lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lws2_32 -lntdll -loleaut32

rem LLVM's release libraries are built against the static C runtime (/MT),
rem so Rig's compiler is too: mixing the two fails to link (LNK2038).
set FLAGS=-std=c++23 -fno-exceptions -fno-rtti -g %OPT% -Wall -Wextra -fms-runtime-lib=static -D_CRT_SECURE_NO_WARNINGS -I"%LLVM%\include" %STAMP%

if not exist bin mkdir bin
"%CXX%" %FLAGS% src\rig.cpp -L"%LLVM%\lib" %LLVM_LIBS% %SYSTEM_LIBS% -o bin\rig.exe || exit /b 1
rem The unit tests: the whole compiler again, with the tests and their own main.
"%CXX%" %FLAGS% tests\unit\unit.cpp -L"%LLVM%\lib" %LLVM_LIBS% %SYSTEM_LIBS% -o bin\unit-tests.exe || exit /b 1
rem The end-to-end tests' runner: no LLVM, it only runs rig and its programs
rem (shell32: its UTF-8 arguments, os_arguments).
"%CXX%" %FLAGS% tests\runner.cpp -lshell32 -o bin\run-tests.exe || exit /b 1

rem Rig falls back to LLVM's lld-link when Visual Studio has no link.exe.
copy /y "%LLVM%\bin\lld-link.exe" bin\lld-link.exe >nul || exit /b 1
exit /b 0

rem call :stamp sets STAMP, the defines that stamp the build (src\version.h):
rem   RIG_VERSION        VERSION's one line, three numbers: 0.1.0
rem   RIG_COMMIT         the commit's full hash, from git rev-parse HEAD
rem   RIG_COMMIT_DIRTY   1 when git status lists a changed tracked file
rem                      (untracked files don't make a build dirty), else 0
rem   RIG_COMMIT_DATE    the commit's date, not the build's, so building the
rem                      same commit again gives the same compiler: 2026-11-13
rem   RIG_BUILD          debug or release
rem Without git on the PATH, or outside a checkout (no .git here: a source
rem archive), the commit and its date are "unknown" and the build isn't
rem dirty. (.git is a directory in a clone and a file in a worktree; asking
rem for it here keeps git from finding some other repository above this
rem one.) The strings reach clang as -DNAME=\"VALUE\", which its command
rem line reads as NAME="VALUE": a C string literal.
:stamp
if not exist VERSION goto no_version
set VERSION_NUMBER=
set /p VERSION_NUMBER=<VERSION
echo %VERSION_NUMBER%| findstr /r /x "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul || goto bad_version
set COMMIT=unknown
set COMMIT_DATE=unknown
set DIRTY=0
rem In for /f's command, ^ keeps = and > from being read by the for itself.
if exist .git for /f "delims=" %%i in ('git rev-parse --verify -q HEAD 2^>nul') do set COMMIT=%%i
if "%COMMIT%"=="unknown" goto stamp_defines
for /f "delims=" %%i in ('git log -1 --format^=%%cs 2^>nul') do set COMMIT_DATE=%%i
for /f "delims=" %%i in ('git --no-optional-locks status --porcelain --untracked-files^=no 2^>nul') do set DIRTY=1
:stamp_defines
set STAMP=-DRIG_VERSION=\"%VERSION_NUMBER%\" -DRIG_COMMIT=\"%COMMIT%\" -DRIG_COMMIT_DIRTY=%DIRTY% -DRIG_COMMIT_DATE=\"%COMMIT_DATE%\" -DRIG_BUILD=\"%BUILD%\"
exit /b 0

:no_version
echo build.bat: VERSION is missing; it holds Rig's version, such as 0.1.0 1>&2
exit /b 1

:bad_version
echo build.bat: VERSION holds "%VERSION_NUMBER%", not a version such as 0.1.0 1>&2
exit /b 1

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

:no_clang
echo build.bat: clang not found in %LLVM%; run scripts\get_llvm.bat again 1>&2
exit /b 1

:bad_cxx
if defined PINNED_CXX goto bad_pinned_cxx
echo build.bat: CXX=%CXX% failed to run (above); name a C++ compiler in CXX, or unset it to use the pinned clang 1>&2
exit /b 1

:bad_pinned_cxx
echo build.bat: the pinned clang, %CXX%, failed to run (above); run scripts\get_llvm.bat again 1>&2
exit /b 1

:no_msvc
echo build.bat: Visual Studio with the C++ build tools wasn't found; install the Visual Studio Build Tools with the "Desktop development with C++" workload 1>&2
exit /b 1
