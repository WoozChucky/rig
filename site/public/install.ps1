# install.ps1: installs Rig on Windows x64, for the user running it.
#
#   irm https://woozchucky.github.io/rig/install.ps1 | iex
#   & ([scriptblock]::Create((irm https://woozchucky.github.io/rig/install.ps1))) -Version 0.2.0
#
# It does what the website's Install page (https://woozchucky.github.io/rig/install)
# does by hand: downloads a release's rig-windows-x64.zip and SHA256SUMS,
# checks the archive's SHA-256, unpacks it into %LOCALAPPDATA%\Programs\rig
# and adds that folder to the user PATH if it isn't there yet. Then it
# installs the VS Code extension when `code` is on the PATH, and builds hello
# world with the installed rig.exe to check that the computer has what
# linking needs. Running it again upgrades: the files are overwritten.
# -Help lists the options. It asks nothing, and needs no administrator.
#
# RIG_INSTALL_BASE_URL, if set, replaces the releases' URL ($releaseBase
# below), for tests: a release's files are then fetched from
# $env:RIG_INSTALL_BASE_URL/latest/download/FILE, or
# $env:RIG_INSTALL_BASE_URL/download/vVERSION/FILE with -Version, as from
# GitHub.
#
# It works in Windows PowerShell 5.1, which comes with Windows, and in
# PowerShell 7. The whole script is one command: a script block, called
# with &, inside an `if ($true) { ... }`, both of which the last line
# closes. That is for three reasons:
#
#   - `irm ... | iex` runs the text in the user's own session, in the scope
#     they typed it in. Inside a script block called with &, the script's
#     variables, functions and preferences ($ErrorActionPreference,
#     $ProgressPreference) are the block's own, and are gone when it ends.
#     The few things that belong to the whole session (the current folder,
#     $LASTEXITCODE, the TLS versions .NET allows) are put back as they were
#     by the block's finally. Failures are thrown, never `exit`, which would
#     close the user's window.
#   - A download cut short can't be parsed, so PowerShell runs none of it.
#     A cut anywhere before the last line leaves the script block open. The
#     `if` is for a cut inside the last line: without it, a text that ended
#     between the block's `}` and ` @args` would parse, and run the block
#     without the user's options. With it, every cut leaves the `if` open.
#     (`if` makes no scope of its own, so $args is still the script's.)
#   - The options: `& ([scriptblock]::Create(text)) -Version 0.2.0` passes
#     them to the script, and `@args` hands them on to the block. Splatting
#     $args keeps each option's name with its value, so the block's param()
#     receives them as if they had been given to it. [CmdletBinding()] makes
#     an option it doesn't have an error; without it, PowerShell would put
#     the option in the block's own $args, and install with it ignored.
#
# The text is plain ASCII, so it reads the same whatever encoding
# Invoke-RestMethod decodes it with.

if ($true) { & {
    [CmdletBinding()]
    param(
        # A release, such as 0.2.0, instead of the latest.
        [string]$Version = '',
        # Leave the user PATH alone.
        [switch]$NoModifyPath,
        # Don't install (or, with -Uninstall, remove) the VS Code extension.
        [switch]$NoVSCode,
        # Remove Rig: its folder, its PATH entry and the VS Code extension.
        [switch]$Uninstall,
        [switch]$Help
    )

    # Where the releases are, and their files: the release workflow's names,
    # which the Install page's links also use (CONTRIBUTING.md, "Releases").
    $releaseBase = 'https://github.com/WoozChucky/rig/releases'
    if ($env:RIG_INSTALL_BASE_URL) { $releaseBase = $env:RIG_INSTALL_BASE_URL }
    $archive = 'rig-windows-x64.zip'
    $vsix = 'rig-vscode.vsix'
    $extensionId = 'woozchucky.rig-language'
    $requirementsUrl = 'https://woozchucky.github.io/rig/install#requirements'

    # The block's own preferences. Stop makes a failing cmdlet throw, rather
    # than write an error and go on. The progress bar is hidden because
    # drawing it makes Windows PowerShell 5.1's Invoke-WebRequest many times
    # slower. (Functions of script modules, such as 5.1's Expand-Archive,
    # don't see these preferences, so the archive is unpacked with .NET
    # instead; see Install-Rig.) A user's strict mode would turn reading a property an
    # exception doesn't have into an error, so it is off here. In
    # PowerShell 7.3 and later, a program's non-zero exit status is not an
    # error here: the script reads $LASTEXITCODE itself.
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'
    Set-StrictMode -Off
    $PSNativeCommandUseErrorActionPreference = $false

    function Show-Usage {
        Write-Host @'
Installs Rig, the compiler and its linker, for your user, on Windows x64.

  irm https://woozchucky.github.io/rig/install.ps1 | iex
  & ([scriptblock]::Create((irm https://woozchucky.github.io/rig/install.ps1))) OPTIONS

It downloads the latest release of Rig, checks its SHA-256, unpacks it into
%LOCALAPPDATA%\Programs\rig, adds that folder to your user PATH unless it is
there already, installs the VS Code extension when `code` is on your PATH,
and builds hello world to check that your computer has what Rig needs. Run
it again to upgrade.

Options:
  -Version VERSION  install that release (such as 0.2.0) instead of the latest
  -NoModifyPath     leave your user PATH alone
  -NoVSCode         don't install (or, with -Uninstall, remove) the VS Code extension
  -Uninstall        remove Rig: its folder, its PATH entry, and the VS Code extension
  -Help             show this

More, and how to install by hand: https://woozchucky.github.io/rig/install
'@
    }

    # Refuses any system but Windows on x64, the one the release has a
    # Windows build for (Linux has install.sh). PowerShell 7 also runs on
    # Linux and macOS, where $IsWindows is false; Windows PowerShell 5.1 only
    # on Windows, where $IsWindows doesn't exist. The architecture is the
    # operating system's: .NET's RuntimeInformation knows it, even in an x64
    # PowerShell emulated on ARM64, and the environment variables are for a
    # .NET Framework too old to have it (where an x86 PowerShell on a 64-bit
    # Windows sees PROCESSOR_ARCHITEW6432).
    function Assert-Platform {
        if ($PSVersionTable.PSEdition -eq 'Core' -and -not $IsWindows) {
            $hint = ''
            if ($IsLinux) { $hint = ': on Linux, install it with install.sh (curl -fsSL https://woozchucky.github.io/rig/install.sh | sh)' }
            throw "Rig is built for 64-bit Linux and Windows only for now$hint"
        }
        $arch = $env:PROCESSOR_ARCHITECTURE
        if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
        try { $arch = [string][Runtime.InteropServices.RuntimeInformation]::OSArchitecture } catch { }
        if ($arch -ne 'AMD64' -and $arch -ne 'X64') {
            throw "Rig is built for 64-bit Linux and Windows only for now (x64), and this Windows is $arch"
        }
        if ($PSVersionTable.PSVersion.Major -lt 5) {
            throw "installing Rig needs PowerShell 5.1 or later, and this is $($PSVersionTable.PSVersion)"
        }
        if (-not $env:LOCALAPPDATA) {
            throw 'LOCALAPPDATA is not set, so there is no folder to install Rig into'
        }
    }

    # Downloads a URL into a file. A file that isn't there (404, a release
    # that doesn't exist) and a network that can't be reached throw
    # different messages, since they call for different fixes. The
    # exception's Response is the server's answer: Windows PowerShell 5.1's
    # WebException and PowerShell 7's HttpResponseException both have one,
    # and an exception for no answer at all has none.
    function Get-Download([string]$url, [string]$file) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file
        } catch {
            $status = 0
            $response = $_.Exception.Response
            if ($response) { $status = [int]$response.StatusCode }
            if ($status -eq 404 -and $Version) {
                throw "there is no Rig $Version to download ($url isn't there): the releases are listed at https://github.com/WoozChucky/rig/releases"
            }
            if ($status -eq 404) {
                throw "$url isn't there: the latest release of Rig may be being published; try again in a few minutes"
            }
            if ($status) { throw "couldn't download $url (HTTP status $status)" }
            # No answer at all. Windows PowerShell 5.1 sometimes gets that
            # for GitHub's 404 too, the connection closed without one, so
            # with -Version the release may be what is wrong.
            if ($Version) {
                throw "couldn't download $url ($($_.Exception.Message)): check your network connection, and that there is a Rig $Version (the releases are listed at https://github.com/WoozChucky/rig/releases)"
            }
            throw "couldn't download $url ($($_.Exception.Message)): check your network connection"
        }
    }

    # Checks a downloaded file against its line in SHA256SUMS, whose lines
    # are "HASH  NAME", as sha256sum writes them. A file that doesn't match
    # isn't the one released: it was changed, or cut short.
    function Assert-Checksum([string]$folder, [string]$name) {
        $expected = $null
        foreach ($line in Get-Content -LiteralPath (Join-Path $folder 'SHA256SUMS')) {
            $fields = -split $line
            if ($fields.Count -eq 2 -and $fields[1].TrimStart('*') -eq $name) { $expected = $fields[0] }
        }
        if (-not $expected) { throw "the release's SHA256SUMS has no line for $name" }
        # Get-FileHash writes the hash in uppercase, and sha256sum, which
        # made SHA256SUMS, in lowercase. Both are put in lowercase and
        # compared with -cne, case and all, so that the comparison doesn't
        # rest on -ne happening to ignore case.
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $folder $name)).Hash.ToLowerInvariant()
        if ($actual -cne $expected.ToLowerInvariant()) {
            throw "${name}: FAILED, its checksum is not the one in SHA256SUMS (the download is damaged or not the one released); nothing was installed"
        }
        Write-Host "${name}: OK"
    }

    # Runs a program in a folder and returns its exit status and what it
    # printed (both streams), without letting it print. Inside, a program's
    # errors are not PowerShell's: Windows PowerShell 5.1 turns each line a
    # program writes to stderr, captured with 2>&1, into an error record,
    # which $ErrorActionPreference = 'Stop' would throw. The current folder
    # is the session's, not the block's, so it is put back afterwards.
    function Invoke-Program([string]$program, [string[]]$arguments, [string]$folder) {
        $ErrorActionPreference = 'Continue'
        Push-Location -LiteralPath $folder
        try {
            $output = & $program @arguments 2>&1 | ForEach-Object { "$_" }
            return @{ Status = $LASTEXITCODE; Output = ($output -join [Environment]::NewLine) }
        } catch {
            return @{ Status = -1; Output = $_.Exception.Message }
        } finally {
            Pop-Location
        }
    }

    # Writes a program's output indented, a line at a time.
    function Write-Indented([string]$text) {
        Write-Host (($text -split '\r?\n' | ForEach-Object { "    $_" }) -join [Environment]::NewLine)
    }

    # Tells Windows that the environment changed, so that the terminals
    # opened next see the new PATH: setting and removing a variable through
    # .NET broadcasts WM_SETTINGCHANGE, which Explorer listens to. Writing
    # the registry, as the PATH is written here, doesn't.
    function Send-EnvironmentChange {
        [Environment]::SetEnvironmentVariable('RIG_PATH_CHANGED', '1', 'User')
        [Environment]::SetEnvironmentVariable('RIG_PATH_CHANGED', $null, 'User')
    }

    # The user PATH as it is stored: HKCU\Environment's Path, read without
    # expanding its %VARIABLES%. [Environment]::GetEnvironmentVariable('Path',
    # 'User') would expand them, and writing that back would replace, say,
    # %USERPROFILE%\.dotnet\tools with C:\Users\...\.dotnet\tools for good.
    function Get-UserPath {
        (Get-Item -LiteralPath 'HKCU:\Environment').GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    }

    # Writes the user PATH back as REG_EXPAND_SZ, the kind Windows gives it,
    # in which %VARIABLES% are expanded when it is read. A PATH left empty
    # (one that held Rig's folder alone) is removed rather than kept empty.
    function Set-UserPath([string]$value) {
        if ($value) {
            New-ItemProperty -LiteralPath 'HKCU:\Environment' -Name Path -PropertyType ExpandString -Force -Value $value > $null
        } else {
            Remove-ItemProperty -LiteralPath 'HKCU:\Environment' -Name Path
        }
    }

    function Install-Rig([string]$work) {
        if ($Version) {
            $releaseUrl = "$releaseBase/download/v$Version"
            Write-Host "Downloading Rig $Version from $releaseUrl"
        } else {
            $releaseUrl = "$releaseBase/latest/download"
            Write-Host "Downloading the latest Rig from $releaseUrl"
        }
        $code = $null
        if (-not $NoVSCode) { $code = Get-Command code -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1 }

        # 1. Download into the temporary folder, which the caller removes.
        Get-Download "$releaseUrl/SHA256SUMS" (Join-Path $work 'SHA256SUMS')
        Get-Download "$releaseUrl/$archive" (Join-Path $work $archive)
        if ($code) { Get-Download "$releaseUrl/$vsix" (Join-Path $work $vsix) }

        # 2. Check every download before changing anything.
        Assert-Checksum $work $archive
        if ($code) { Assert-Checksum $work $vsix }

        # 3. Unpack into the temporary folder first: an archive that can't
        # be read stops here, with nothing installed. Then copy its files
        # over the old ones. rig.exe finds lld-link.exe beside it, so they
        # stay together. The archive is unpacked with .NET's ZipFile rather
        # than Expand-Archive: in Windows PowerShell 5.1 that is a function
        # of a script module, which doesn't see the block's preferences, so
        # it would draw its progress bar, slowly, file by file. (5.1 needs
        # the assembly loaded; PowerShell 7 has it already.)
        $unpacked = Join-Path $work 'unpacked'
        try {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            [IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $work $archive), $unpacked)
        } catch {
            throw "couldn't unpack $archive ($($_.Exception.Message))"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $unpacked 'rig.exe'))) { throw "$archive has no rig.exe in it" }
        New-Item -ItemType Directory -Force -Path $dir > $null
        try {
            Copy-Item -Path (Join-Path $unpacked '*') -Destination $dir -Force
        } catch {
            throw "couldn't replace the files in $dir ($($_.Exception.Message)). Windows can't overwrite a program that is running: if rig.exe is (a build, or VS Code's extension checking a file), wait for it or close VS Code, then run this again"
        }
        Write-Host "Installed Rig into $dir"

        # 4. The user PATH, as the Install page edits it: only when the
        # folder isn't one of its entries already, keeping the others as
        # they are written. The folder goes at the end, before the ';' a PATH
        # may end with, so that taking it off again (-Uninstall) gives back
        # exactly the PATH there was.
        $pathChanged = $false
        $userPath = Get-UserPath
        if (($userPath -split ';') -notcontains $dir) {
            if ($NoModifyPath) {
                Write-Host "$dir isn't on your PATH: add it to run rig by name (the Install page says how)."
            } else {
                if (-not $userPath) { $newPath = $dir }
                elseif ($userPath.EndsWith(';')) { $newPath = "$userPath$dir;" }
                else { $newPath = "$userPath;$dir" }
                Set-UserPath $newPath
                Send-EnvironmentChange
                Write-Host "Added $dir to your user PATH"
                $pathChanged = $true
            }
        }

        # 5. The VS Code extension. --force replaces an installed copy, so a
        # second run upgrades it too. A failure here leaves Rig installed.
        if ($code) {
            $result = Invoke-Program $code.Source @('--install-extension', (Join-Path $work $vsix), '--force') $work
            if ($result.Status -eq 0) {
                Write-Host 'Installed the VS Code extension: restart VS Code to use it.'
            } else {
                Write-Host "Couldn't install the VS Code extension:"
                Write-Host $result.Output
                Write-Host 'To try again: code --install-extension rig-vscode.vsix'
            }
        }

        # 6. Check the requirements: build and run hello world in a folder of
        # its own (rig writes the program, and its object files under .build,
        # in the current folder). A failure doesn't undo the install: rig
        # works once Visual Studio's tools are installed. It says what is
        # missing instead. WriteAllText writes UTF-8 without a byte order
        # mark, as a .rig file is.
        $rig = Join-Path $dir 'rig.exe'
        $hello = Join-Path $work 'hello'
        New-Item -ItemType Directory -Path $hello > $null
        [IO.File]::WriteAllText((Join-Path $hello 'hello.rig'), "puts :: (s: *u8) -> s32 #foreign libc;`nmain :: () { puts(`"Hello, World!`"); }`n")
        $build = Invoke-Program $rig @('build', 'hello.rig') $hello
        if ($build.Status -ne 0) {
            # A build that fails is what the requirements are about: the
            # link step is the one that needs Visual Studio's tools.
            Write-Host ''
            Write-Host "Rig is installed, but it couldn't build hello world:"
            Write-Indented $build.Output
            Write-Host 'Linking a program needs Visual Studio, or its free Build Tools'
            Write-Host '(https://visualstudio.microsoft.com/downloads/), with the "Desktop development with C++"'
            Write-Host "workload. More: $requirementsUrl"
            Write-Host ''
        } else {
            # Built, so the requirements are there. A program that then
            # doesn't run, or prints something else, has another cause,
            # which the requirements message would only hide: an antivirus
            # that stops new programs in the temporary folder, say.
            $run = Invoke-Program (Join-Path $hello 'hello.exe') @() $hello
            if ($run.Status -eq 0 -and $run.Output.Trim() -eq 'Hello, World!') {
                Write-Host 'Built and ran hello world: Rig has what it needs.'
            } else {
                # Status -1 is Invoke-Program's: Windows refused to start it.
                Write-Host ''
                if ($run.Status -eq 0) {
                    Write-Host 'Rig is installed and built hello world, but the program printed something else:'
                } elseif ($run.Status -eq -1) {
                    Write-Host "Rig is installed and built hello world, but couldn't run it:"
                } else {
                    Write-Host "Rig is installed and built hello world, but running it failed (exit status $($run.Status)):"
                }
                if ($run.Output) { Write-Indented $run.Output }
                Write-Host 'Building works, so Rig has what it needs. Running a program from the temporary'
                Write-Host 'folder went wrong (an antivirus may stop new programs there): build and run'
                Write-Host 'hello world in a folder of your own to see.'
                Write-Host ''
            }
        }

        # 7. Say how it went, and what to do next. A terminal opened before
        # the PATH changed doesn't see the change, this one included.
        Write-Host (Invoke-Program $rig @('version') $hello).Output
        Write-Host ''
        $onPath = ($env:Path -split ';') -contains $dir
        if ($pathChanged -or (-not $onPath -and -not $NoModifyPath)) {
            Write-Host "Open a new terminal (this one hasn't got the new PATH), then build a program:"
        } elseif (-not $onPath) {
            Write-Host 'With its folder on your PATH, build a program:'
        } else {
            Write-Host 'Build a program:'
        }
        Write-Host '    rig build hello.rig'
    }

    # Removes what Install-Rig put in place. $removed is set by each step
    # that took something away, the PATH entry and the extension included:
    # Rig's folder may have been deleted by hand already.
    function Uninstall-Rig([string]$work) {
        $removed = $false
        if (Test-Path -LiteralPath $dir) {
            try {
                Remove-Item -LiteralPath $dir -Recurse -Force
            } catch {
                throw "couldn't remove $dir ($($_.Exception.Message)). Windows can't delete a program that is running: if rig.exe is, wait for it or close VS Code, then run this again"
            }
            Write-Host "Removed $dir"
            $removed = $true
        }
        # Only Rig's entry comes off the PATH; the others are written back
        # as they were stored.
        if (-not $NoModifyPath) {
            $userPath = Get-UserPath
            $entries = $userPath -split ';'
            if ($entries -contains $dir) {
                Set-UserPath (($entries -ne $dir) -join ';')
                Send-EnvironmentChange
                Write-Host "Removed $dir from your user PATH"
                $removed = $true
            }
        }
        if (-not $NoVSCode) {
            $code = Get-Command code -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            # code fails when the extension isn't installed; that is fine.
            if ($code -and (Invoke-Program $code.Source @('--uninstall-extension', $extensionId) $work).Status -eq 0) {
                Write-Host 'Removed the VS Code extension'
                $removed = $true
            }
        }
        if ($removed) {
            Write-Host 'Rig is uninstalled.'
        } else {
            Write-Host "Rig wasn't installed in ${dir}: there was nothing to remove."
        }
    }

    if ($Help) {
        Show-Usage
        return
    }
    Assert-Platform
    # The releases are tagged vVERSION; 0.2.0 and v0.2.0 name the same one.
    $Version = $Version.TrimStart('v')
    $dir = "$env:LOCALAPPDATA\Programs\rig"

    # What belongs to the whole session, kept to be put back: $LASTEXITCODE,
    # which running a program sets globally, and the TLS versions .NET
    # allows. GitHub takes TLS 1.2 or later; Windows PowerShell 5.1 on a
    # .NET Framework that doesn't defer to Windows (SecurityProtocol not
    # SystemDefault, 0) may allow only older ones, so TLS 1.2 is added for
    # the download. SystemDefault lets Windows choose, TLS 1.3 included, and
    # is left alone.
    # Get-Variable returns the variable itself, whose Value follows it, so
    # the value is copied.
    $hadExitCode = [bool](Get-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue)
    $savedExitCode = $global:LASTEXITCODE
    $savedProtocol = [Net.ServicePointManager]::SecurityProtocol
    $work = Join-Path ([IO.Path]::GetTempPath()) ('rig-install-' + [Guid]::NewGuid().ToString('N'))
    try {
        if ([int]$savedProtocol -ne 0 -and -not ($savedProtocol -band [Net.SecurityProtocolType]::Tls12)) {
            [Net.ServicePointManager]::SecurityProtocol = $savedProtocol -bor [Net.SecurityProtocolType]::Tls12
        }
        New-Item -ItemType Directory -Path $work > $null
        if ($Uninstall) { Uninstall-Rig $work } else { Install-Rig $work }
    } finally {
        [Net.ServicePointManager]::SecurityProtocol = $savedProtocol
        if ($hadExitCode) {
            $global:LASTEXITCODE = $savedExitCode
        } else {
            Remove-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
} @args }
