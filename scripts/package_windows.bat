@echo off
rem package_windows.bat: builds the release rig.exe and packs it for a
rem release, as dist\rig-windows-x64.zip. The release workflow runs it on a
rem Windows runner; so can anyone.
rem
rem The archive holds, at its top level (no folder around them):
rem   rig.exe        the compiler, an optimised build (build.bat release)
rem   lld-link.exe   LLVM's linker, which rig uses when Visual Studio has no
rem                  link.exe (spec 10.6)
rem   modules\       the modules that ship with rig (Preload, Basic,
rem                  Compiler), which rig finds in modules\ beside rig.exe
rem                  (spec 2.14)
rem   LICENSE, README.md
rem   LICENSE-LLVM.txt, LICENSE-LLD.txt
rem                  the licences of LLVM, which rig.exe links statically,
rem                  and of lld-link.exe (third_party\README.md)
rem
rem Then it checks the archive as someone downloading it would use it: it
rem unpacks it into a temporary folder and, with the rig.exe it holds, as
rem package_linux.sh does,
rem   - builds tests\run\hello.rig, runs the program and compares what it
rem     prints with tests\run\hello.out;
rem   - builds a copy of tests\build\load_across_files, a build program,
rem     which imports the Compiler module, as rig build try\build.rig, runs
rem     the game it makes and compares what it prints with game.out;
rem   - builds tests\run\basic_string.rig, which imports Basic, runs it and
rem     compares what it prints with basic_string.out.
rem Every program imports Preload without asking, hello too, so all three
rem builds find the archive's modules\, beside its rig.exe: Preload in each,
rem Compiler and Basic in theirs.
rem
rem The zip is written and read with the tar.exe that ships with Windows 10
rem and later, by its full path (Git's GNU tar, which may come first on the
rem PATH, can't write zip files). Programs are run by their paths
rem (.\rig.exe): cmd may be set not to look for them in the current folder
rem (NoDefaultCurrentDirectoryInExePath).

setlocal
cd /d "%~dp0\.."
set "ROOT=%CD%"
set "ARCHIVE=dist\rig-windows-x64.zip"
set "TAR=%SystemRoot%\System32\tar.exe"
set "STAGE=%TEMP%\rig-package-%RANDOM%%RANDOM%"

if not exist "%TAR%" (
    echo package_windows.bat: %TAR% is missing; it ships with Windows 10 version 1803 and later 1>&2
    exit /b 1
)

call "%ROOT%\build.bat" release || exit /b 1
bin\rig.exe version || exit /b 1

mkdir "%STAGE%\pack" "%STAGE%\check" || exit /b 1
for %%f in (bin\rig.exe bin\lld-link.exe LICENSE README.md) do copy /y "%%f" "%STAGE%\pack\" >nul || goto failed
xcopy /e /i /q /y modules "%STAGE%\pack\modules" >nul || goto failed
copy /y third_party\llvm\LICENSE.TXT "%STAGE%\pack\LICENSE-LLVM.txt" >nul || goto failed
copy /y third_party\lld\LICENSE.TXT "%STAGE%\pack\LICENSE-LLD.txt" >nul || goto failed

if not exist dist mkdir dist
if exist "%ARCHIVE%" del "%ARCHIVE%"
"%TAR%" --format zip -cf "%ARCHIVE%" -C "%STAGE%\pack" rig.exe lld-link.exe modules LICENSE README.md LICENSE-LLVM.txt LICENSE-LLD.txt || goto failed

"%TAR%" -xf "%ARCHIVE%" -C "%STAGE%\check" || goto failed
pushd "%STAGE%\check"
.\rig.exe build "%ROOT%\tests\run\hello.rig" -o hello.exe && .\hello.exe > hello.txt
set STATUS=%ERRORLEVEL%
popd
if not "%STATUS%"=="0" goto failed
rem fc /l compares the two line by line, whatever their line endings: the
rem program's C runtime writes \r\n, and hello.out is checked out with \n.
"%SystemRoot%\System32\fc.exe" /l "%ROOT%\tests\run\hello.out" "%STAGE%\check\hello.txt" >nul || (
    echo package_windows.bat: the packed rig.exe's hello doesn't print tests\run\hello.out 1>&2
    goto failed
)

rem The build program, built from the folder above it, as the runner's fixed
rem case does: its files and its game are relative to try\.
xcopy /e /i /q /y "%ROOT%\tests\build\load_across_files" "%STAGE%\check\try" >nul || goto failed
pushd "%STAGE%\check"
.\rig.exe build try\build.rig && .\try\game.exe > game.txt
set STATUS=%ERRORLEVEL%
popd
if not "%STATUS%"=="0" (
    echo package_windows.bat: the packed rig.exe couldn't build and run tests\build\load_across_files 1>&2
    goto failed
)
"%SystemRoot%\System32\fc.exe" /l "%ROOT%\tests\build\load_across_files\game.out" "%STAGE%\check\game.txt" >nul || (
    echo package_windows.bat: the packed rig.exe's build program makes a game that doesn't print tests\build\load_across_files\game.out 1>&2
    goto failed
)

pushd "%STAGE%\check"
.\rig.exe build "%ROOT%\tests\run\basic_string.rig" -o basic.exe && .\basic.exe > basic.txt
set STATUS=%ERRORLEVEL%
popd
if not "%STATUS%"=="0" (
    echo package_windows.bat: the packed rig.exe couldn't build and run tests\run\basic_string.rig 1>&2
    goto failed
)
"%SystemRoot%\System32\fc.exe" /l "%ROOT%\tests\run\basic_string.out" "%STAGE%\check\basic.txt" >nul || (
    echo package_windows.bat: the packed rig.exe's basic_string doesn't print tests\run\basic_string.out 1>&2
    goto failed
)

rmdir /s /q "%STAGE%"
echo package_windows.bat: wrote %ARCHIVE%
exit /b 0

:failed
rmdir /s /q "%STAGE%" 2>nul
echo package_windows.bat: packing or checking %ARCHIVE% failed 1>&2
exit /b 1
