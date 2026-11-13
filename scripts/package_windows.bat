@echo off
rem package_windows.bat: builds the release rig.exe and packs it for a
rem release, as dist\rig-windows-x64.zip. The release workflow runs it on a
rem Windows runner; so can anyone.
rem
rem The archive holds, at its top level (no folder around them):
rem   rig.exe        the compiler, an optimised build (build.bat release)
rem   lld-link.exe   LLVM's linker, which rig uses when Visual Studio has no
rem                  link.exe (spec 10.6)
rem   LICENSE, README.md
rem
rem Then it checks the archive as someone downloading it would use it: it
rem unpacks it into a temporary folder, builds tests\run\hello.rig with the
rem rig.exe it holds, and runs the program, which must print hello.out's
rem line. The zip is written and read with the tar.exe that ships with
rem Windows 10 and later, by its full path (Git's GNU tar, which may come
rem first on the PATH, can't write zip files). Programs are run by their
rem paths (.\rig.exe): cmd may be set not to look for them in the current
rem folder (NoDefaultCurrentDirectoryInExePath).

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

if not exist dist mkdir dist
if exist "%ARCHIVE%" del "%ARCHIVE%"
"%TAR%" --format zip -cf "%ARCHIVE%" -C "%STAGE%\pack" rig.exe lld-link.exe LICENSE README.md || goto failed

"%TAR%" -xf "%ARCHIVE%" -C "%STAGE%\check" || goto failed
pushd "%STAGE%\check"
.\rig.exe build "%ROOT%\tests\run\hello.rig" -o hello.exe && .\hello.exe > hello.txt
set STATUS=%ERRORLEVEL%
popd
if not "%STATUS%"=="0" goto failed
rem hello.out's one line; findstr /x matches a whole line, whatever its
rem line ending (the program's C runtime writes \r\n).
findstr /x /c:"Hello, World!" "%STAGE%\check\hello.txt" >nul || goto failed

rmdir /s /q "%STAGE%"
echo package_windows.bat: wrote %ARCHIVE%
exit /b 0

:failed
rmdir /s /q "%STAGE%" 2>nul
echo package_windows.bat: packing or checking %ARCHIVE% failed 1>&2
exit /b 1
