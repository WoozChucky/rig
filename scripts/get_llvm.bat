@echo off
rem get_llvm.bat: downloads the pinned LLVM release for Windows into
rem deps\llvm-windows (or RIG_LLVM_DIR), keeping only what building Rig
rem needs: the C API's headers, the static libraries, llvm-config and
rem lld-link.
rem
rem The version is pinned on purpose (spec section 3): changing it is a
rem deliberate change to this file, its checksum and get_llvm.sh.
rem curl.exe and tar.exe ship with Windows 10 and later.

setlocal
cd /d "%~dp0\.."

set VERSION=23.1.5
set ARCHIVE=clang+llvm-%VERSION%-x86_64-pc-windows-msvc.tar.xz
set SHA256=854bffcfc84528e0ee92490ff5c178920abd358a79c3e98f053e41e3e8e4c6d2
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
tar.exe -xf "deps\%ARCHIVE%" -C "%DEST%" --strip-components=1 "*/include/llvm-c/*" "*/include/llvm/Config/*" "*/lib/LLVM*.lib" "*/bin/llvm-config.exe" "*/bin/lld-link.exe" || exit /b 1

del "deps\%ARCHIVE%"
echo LLVM %VERSION% is in %DEST%
