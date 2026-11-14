@echo off
rem install.bat: installs Rig's VS Code extension.
rem
rem VS Code installs extensions from a .vsix: a zip holding a manifest,
rem [Content_Types].xml and the extension's files under extension\. This
rem script lays those out in a temporary folder, zips them with the tar.exe
rem that ships with Windows 10 and later (it writes zip files too; called by
rem its full path, since Git's or MSYS2's GNU tar may come first on the
rem PATH and can't), and
rem hands the result to `code --install-extension`. Nothing is downloaded.
rem
rem Any arguments go to `code` first, e.g. install.bat --profile Rig.
rem Run it again after changing the extension; --force replaces the old copy.
rem It installs for Windows windows; in WSL, install.sh installs for WSL
rem windows, where the extension runs in them.

setlocal
set "HERE=%~dp0"
set "STAGE=%TEMP%\rig-vsix-%RANDOM%%RANDOM%"
set "VSIX=%STAGE%.vsix"
set "TAR=%SystemRoot%\System32\tar.exe"

rem The extension's files, from this folder: what the .vsix holds under
rem extension\. install.sh has the same list, and test\check.js checks the
rem two agree and hold every file package.json names.
set "FILES=package.json language-configuration.json README.md extension.js debug.js diagnostics.js program.js syntaxes\rig.tmLanguage.json snippets\rig.json"

if not exist "%TAR%" (
    echo install.bat: %TAR% is missing; it ships with Windows 10 version 1803 and later. 1>&2
    exit /b 1
)

where code >nul 2>nul || (
    echo install.bat: 'code' isn't on the PATH. In VS Code, run "Shell Command: Install 'code' command in PATH", or reinstall VS Code with "Add to PATH" ticked. 1>&2
    exit /b 1
)

mkdir "%STAGE%\extension" || exit /b 1
copy /y "%HERE%vsix\[Content_Types].xml" "%STAGE%\" >nul || goto failed
copy /y "%HERE%vsix\extension.vsixmanifest" "%STAGE%\" >nul || goto failed
for %%f in (%FILES%) do call :pack "%%f" || goto failed

"%TAR%" --format zip -cf "%VSIX%" -C "%STAGE%" "[Content_Types].xml" extension.vsixmanifest extension || goto failed
call code %* --install-extension "%VSIX%" --force || goto failed

rmdir /s /q "%STAGE%"
del "%VSIX%"
echo Installed. Reload VS Code's windows (Developer: Reload Window) to use it.
exit /b 0

:failed
rmdir /s /q "%STAGE%" 2>nul
del "%VSIX%" 2>nul
echo install.bat: installing the extension failed 1>&2
exit /b 1

rem call :pack FILE copies one of the extension's files (a path from this
rem folder) into the stage's extension\, making its folder first: %%~dpg is
rem the drive and folder of the destination %%g.
:pack
for %%g in ("%STAGE%\extension\%~1") do if not exist "%%~dpg" mkdir "%%~dpg" || exit /b 1
copy /y "%HERE%%~1" "%STAGE%\extension\%~1" >nul || exit /b 1
exit /b 0
