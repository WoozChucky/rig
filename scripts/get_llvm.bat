@echo off
rem get_llvm.bat: downloads the pinned LLVM release for Windows into
rem deps\llvm-windows (or RIG_LLVM_DIR), keeping only what building Rig
rem needs:
rem   - the C API's headers, the static libraries and llvm-config, which
rem     Rig's compiler is compiled against and linked with;
rem   - lld-link, the linker build.bat puts beside rig.exe, which rig.exe
rem     uses when Visual Studio has no link.exe;
rem   - clang, the compiler that builds Rig (build.bat): bin\clang++.exe, the
rem     one name build.bat runs (it compiles C++ and links C++'s runtime),
rem     and lib\clang\MAJOR\include, its own headers (stddef.h, stdarg.h,
rem     the intrinsics...), which it finds from where it's installed
rem     (bin\..\lib\clang\MAJOR). The release's clang.exe and clang-cl.exe
rem     are the same program under other names, but on Windows each is a
rem     separate copy of it (about 113 MB), not a link, and nothing runs
rem     them: clang++.exe runs its compiler (cc1) in its own process. So
rem     only clang++.exe is kept, and none of clang's other tools. Visual
rem     Studio's C and C++ libraries and headers aren't LLVM's to ship:
rem     clang uses the installed ones, as build.bat says.
rem Running it again extracts everything again over what's there, so a
rem deps\llvm-windows from before clang was kept gets it without being
rem deleted.
rem
rem The version is pinned on purpose (spec section 3): changing it is a
rem deliberate change to this file, its checksum and get_llvm.sh.
rem curl.exe and tar.exe ship with Windows 10 and later.

setlocal
cd /d "%~dp0\.."

set VERSION=23.1.5
rem MAJOR, 23: clang's resource directory is lib\clang\23.
for /f "delims=." %%m in ("%VERSION%") do set MAJOR=%%m
set ARCHIVE=clang+llvm-%VERSION%-x86_64-pc-windows-msvc.tar.xz
set SHA256=62fdada7f6fa2b73856486dfbbdba3d2c1882991621615bcb55c60207611c6d4
set URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-%VERSION%/%ARCHIVE%
set DEST=%RIG_LLVM_DIR%
if "%DEST%"=="" set DEST=deps\llvm-windows

if not exist deps mkdir deps
if not exist "%DEST%" mkdir "%DEST%"
if not exist "deps\%ARCHIVE%" (
    echo downloading %URL%
    rem -C -: resume a broken download where it stopped.
    curl.exe -fL -C - -o "deps\%ARCHIVE%.part" "%URL%" || exit /b 1
    move /y "deps\%ARCHIVE%.part" "deps\%ARCHIVE%" >nul || exit /b 1
)

rem certutil prints the hash on its second line.
set ACTUAL=
for /f "skip=1 delims=" %%h in ('certutil -hashfile "deps\%ARCHIVE%" SHA256') do if not defined ACTUAL set ACTUAL=%%h
set ACTUAL=%ACTUAL: =%
if /i not "%ACTUAL%"=="%SHA256%" (
    echo get_llvm.bat: deps\%ARCHIVE% doesn't match the pinned checksum; delete it and run again 1>&2
    exit /b 1
)

echo extracting into %DEST%
tar.exe -xf "deps\%ARCHIVE%" -C "%DEST%" --strip-components=1 "*/include/llvm-c/*" "*/include/llvm/Config/*" "*/lib/LLVM*.lib" "*/bin/llvm-config.exe" "*/bin/lld-link.exe" "*/bin/clang++.exe" "*/lib/clang/%MAJOR%/include/*" || exit /b 1

del "deps\%ARCHIVE%"
echo LLVM %VERSION% is in %DEST%
