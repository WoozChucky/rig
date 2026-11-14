# test_install.ps1: tests site/public/install.ps1, the one-line installer
# for Windows, as its users run it: fetched with Invoke-RestMethod and run
# with Invoke-Expression or as a script block. Run it with the PowerShell to
# test, Windows PowerShell 5.1 or PowerShell 7:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test_install.ps1
#   pwsh -NoProfile -File scripts\test_install.ps1
#
# It changes nothing of yours. Each case runs in a child PowerShell of the
# same kind, with LOCALAPPDATA pointing at a temporary folder, a PATH of
# Windows' own folders and a stand-in `code` (a .cmd that records its
# arguments, so no editor gets anything installed), and -NoModifyPath
# wherever it installs: your user PATH is read before and after, and must
# not change. (Editing the PATH, which -NoModifyPath skips, is checked by
# hand: CONTRIBUTING.md's "Releasing" section says how.)
#
# The script is served to the children by a small HTTP server on localhost,
# as GitHub Pages serves it: as application/octet-stream, which
# Invoke-RestMethod must still hand to Invoke-Expression as text. The same
# server has a release whose archive doesn't match its SHA256SUMS.
#
# What it checks:
#   - A script cut short anywhere, its last line included, runs nothing.
#   - Run with `irm | iex` or as a script block, it leaves the session as it
#     found it: its variables, $ErrorActionPreference, $ProgressPreference,
#     $LASTEXITCODE, the current folder and the TLS versions allowed.
#   - -Help, and each failure: an unknown option, a release that doesn't
#     exist, a checksum that doesn't match, no network. Each stops with its
#     message, before anything is installed.
#   - Installing the latest release: rig.exe and lld-link.exe in
#     %LOCALAPPDATA%\Programs\rig, hello world built and run (Visual
#     Studio's C++ tools must be installed), the extension installed.
#   - Running it again upgrades in place (a damaged rig.exe is replaced).
#   - -Version 0.2.0 installs 0.2.0.
#   - -Uninstall removes the folder and the extension, and says what it
#     did: Rig is uninstalled when it removed anything, nothing to remove
#     when it didn't.
#
# The installs download releases from GitHub, so it needs the network.

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$script = Join-Path $repo 'site\public\install.ps1'
if (-not (Test-Path -LiteralPath $script)) { throw "test_install.ps1: $script is missing" }
$shell = (Get-Process -Id $PID).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ('rig-test-install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work > $null

function Fail([string]$message, [string]$output) {
    if ($output) { Write-Host "--- the child's output:"; Write-Host $output }
    throw "test_install.ps1: FAILED: $message"
}

function Pass([string]$message) {
    Write-Host "ok: $message"
}

# The user PATH as stored, its kind and its value, to check that no case
# changes it.
function Get-RawUserPath {
    $key = Get-Item -LiteralPath 'HKCU:\Environment'
    if ($null -eq $key.GetValue('Path')) { return 'none' }
    "$($key.GetValueKind('Path')):$($key.GetValue('Path', '', 'DoNotExpandEnvironmentNames'))"
}

# Checks that the user PATH is as it was before the test. If a case changed
# it (an install that should have had -NoModifyPath, say), it is written
# back as it was, before the test fails.
function Assert-UserPathKept([string]$case, [string]$output) {
    if ((Get-RawUserPath) -eq $userPathBefore) { return }
    if ($null -eq $savedUserPathValue) {
        Remove-ItemProperty -LiteralPath 'HKCU:\Environment' -Name Path
    } else {
        New-ItemProperty -LiteralPath 'HKCU:\Environment' -Name Path -PropertyType $savedUserPathKind -Force -Value $savedUserPathValue > $null
    }
    Fail "$case changed the user PATH (now put back as it was)" $output
}

# Runs a command in a child PowerShell, between two snapshots of what
# install.ps1 must leave alone in the session it runs in. The child sets
# variables of its own first, some with the names install.ps1 uses inside,
# so that one leaking out would change them. Returns what the child printed;
# "SESSION CHANGED: ..." in it says what changed, and "THROWN: ..." the
# error the command threw.
function Invoke-Child([string]$command) {
    $child = @"
`$ErrorActionPreference = 'Continue'
`$ProgressPreference = 'Continue'
`$Version = 'mine'; `$dir = 'mine'; `$work = 'mine'; `$code = 'mine'
`$global:LASTEXITCODE = 42
Set-Location -LiteralPath '$work'
`$protocol = [Net.ServicePointManager]::SecurityProtocol
function Get-Session {
    `$skip = '^(\?|\^|\$|_|args|input|PSItem|Error|StackTrace|MyInvocation|PSBoundParameters|PSCmdlet|PSCommandPath|PSScriptRoot|ExecutionContext|true|false|null|before|after|thrown|line|protocol)$'
    `$vars = Get-Variable -Scope Global | Where-Object { `$_.Name -notmatch `$skip } | ForEach-Object { "`$(`$_.Name)=`$(`$_.Value)" }
    (@(`$vars) + "location=`$((Get-Location).Path)" + "protocol=`$([Net.ServicePointManager]::SecurityProtocol)") -join [Environment]::NewLine
}
`$before = Get-Session
try { $command } catch { Write-Host "THROWN: `$(`$_.Exception.Message)" }
`$after = Get-Session
if (`$after -ne `$before) {
    Compare-Object (`$before -split [Environment]::NewLine) (`$after -split [Environment]::NewLine) | ForEach-Object { Write-Host "SESSION CHANGED: `$(`$_.SideIndicator) `$(`$_.InputObject)" }
}
"@
    # -EncodedCommand (the script as UTF-16, in base64) rather than
    # -Command: Windows PowerShell 5.1 passes a program an argument's double
    # quotes unescaped, which would cut the script apart. The child's errors
    # arrive as error records, which this function's own preference keeps
    # from stopping the test.
    $ErrorActionPreference = 'Continue'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($child))
    $output = & $shell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded 2>&1 | ForEach-Object { "$_" }
    $output = $output -join [Environment]::NewLine
    Assert-UserPathKept 'a case' $output
    $output
}

# A fresh, empty LOCALAPPDATA, and no record of `code`'s calls.
function Reset-AppData {
    $script:appData = Join-Path $work 'appdata'
    if (Test-Path -LiteralPath $appData) { Remove-Item -LiteralPath $appData -Recurse -Force }
    New-Item -ItemType Directory -Path $appData > $null
    $env:LOCALAPPDATA = $appData
    Remove-Item -LiteralPath (Join-Path $work 'bin\code.log') -ErrorAction SilentlyContinue
}

function Assert-NothingInstalled([string]$case, [string]$output) {
    if (@(Get-ChildItem -LiteralPath $appData -Force).Count -ne 0) { Fail "$case installed something" $output }
}

function Assert-SessionKept([string]$case, [string]$output) {
    if ($output -match 'SESSION CHANGED') { Fail "$case changed the session" $output }
}

# The rig.exe installed, its `rig version` line, or $null.
function Get-RigVersion {
    $rig = Join-Path $appData 'Programs\rig\rig.exe'
    if (-not (Test-Path -LiteralPath $rig)) { return $null }
    try { (& $rig version 2>&1 | Select-Object -First 1) -as [string] } catch { $null }
}

$listener = $null
$server = $null
$savedPath = $env:Path
$savedAppData = $env:LOCALAPPDATA
$userPathBefore = Get-RawUserPath
$environmentKey = Get-Item -LiteralPath 'HKCU:\Environment'
$savedUserPathValue = $environmentKey.GetValue('Path', $null, 'DoNotExpandEnvironmentNames')
$savedUserPathKind = $null
if ($null -ne $savedUserPathValue) { $savedUserPathKind = $environmentKey.GetValueKind('Path') }
try {
    # --- The server -------------------------------------------------------
    #
    # Its folder has install.ps1 at the top, as the website does, and under
    # bad/ a release laid out as GitHub's, whose archive isn't the one its
    # SHA256SUMS names. Every file is sent as application/octet-stream.
    $site = Join-Path $work 'site'
    $bad = Join-Path $site 'bad\download\v0.2.0'
    New-Item -ItemType Directory -Path $bad > $null
    Copy-Item -LiteralPath $script -Destination $site
    [IO.File]::WriteAllText((Join-Path $bad 'rig-windows-x64.zip'), "not Rig`n")
    [IO.File]::WriteAllText((Join-Path $bad 'SHA256SUMS'), ('0' * 64) + "  rig-windows-x64.zip`n")
    # A port picked at random may be taken, or in a range Windows keeps for
    # itself (Hyper-V's): another is tried.
    for ($try = 1; -not $listener; $try++) {
        $port = Get-Random -Minimum 49152 -Maximum 65000
        $listener = [Net.HttpListener]::new()
        $listener.Prefixes.Add("http://localhost:$port/")
        try { $listener.Start() } catch { $listener = $null; if ($try -eq 20) { throw } }
    }
    $server = [powershell]::Create().AddScript({
        param($listener, $root)
        while ($listener.IsListening) {
            try { $context = $listener.GetContext() } catch { break }
            $file = Join-Path $root ($context.Request.Url.AbsolutePath.TrimStart('/') -replace '/', '\')
            if (Test-Path -LiteralPath $file -PathType Leaf) {
                $bytes = [IO.File]::ReadAllBytes($file)
                $context.Response.ContentType = 'application/octet-stream'
                $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            } else {
                $context.Response.StatusCode = 404
            }
            $context.Response.Close()
        }
    }).AddArgument($listener).AddArgument($site)
    $server.BeginInvoke() > $null
    $url = "http://localhost:$port/install.ps1"

    # The stand-in `code`, and a PATH of Windows' folders and it alone.
    $bin = Join-Path $work 'bin'
    New-Item -ItemType Directory -Path $bin > $null
    [IO.File]::WriteAllText((Join-Path $bin 'code.cmd'), "@echo %*>> `"%~dp0code.log`"`r`n")
    $env:Path = "$bin;$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0;$(Split-Path -Parent $shell)"
    $env:RIG_INSTALL_BASE_URL = $null

    # --- A download cut short runs nothing ----------------------------------
    #
    # Invoke-Expression is given the script cut after each of its lines but
    # the last, and after every character of the last line but its end.
    # Each must fail to parse (a ParseException), or parse to comments
    # alone; any other error means some of it ran. A cut inside the last
    # line matters most: one that ran the block without `@args` would
    # install where -Uninstall was asked. Should a cut run something
    # anyway, the releases' URL is one where nothing answers, so that it
    # stops before installing, or editing the PATH.
    Reset-AppData
    $lines = [IO.File]::ReadAllLines($script)
    if ($lines[-1] -ne '} @args }') { Fail "install.ps1's last line is '$($lines[-1])', not the end of its script block and if" }
    $output = Invoke-Child @"
& {
    `$env:RIG_INSTALL_BASE_URL = 'http://127.0.0.1:9'
    `$lines = [IO.File]::ReadAllLines('$script')
    `$cuts = [Collections.Generic.List[object]]::new()
    for (`$n = 1; `$n -lt `$lines.Count; `$n++) {
        `$cuts.Add(@("line `$n", ((`$lines[0..(`$n - 1)]) -join "``n")))
    }
    `$before = ((`$lines[0..(`$lines.Count - 2)]) -join "``n") + "``n"
    `$last = `$lines[-1]
    for (`$k = 1; `$k -lt `$last.Length; `$k++) {
        `$cuts.Add(@("character `$k of the last line", (`$before + `$last.Substring(0, `$k))))
    }
    `$ran = ''
    foreach (`$cut in `$cuts) {
        try {
            Invoke-Expression `$cut[1] > `$null
        } catch {
            if (`$_.Exception -isnot [Management.Automation.ParseException]) { `$ran = "`$(`$cut[0]): `$(`$_.Exception.Message)"; break }
        }
        if (Test-Path -LiteralPath (Join-Path `$env:LOCALAPPDATA 'Programs')) { `$ran = "`$(`$cut[0]): it installed"; break }
    }
    `$env:RIG_INSTALL_BASE_URL = `$null
    Write-Host "CUTS: `$(`$cuts.Count) RAN: [`$ran]"
}
"@
    if ($output -notmatch 'CUTS: (\d+) RAN: \[\]') { Fail 'a cut-short install.ps1 ran something' $output }
    Assert-NothingInstalled 'a cut-short install.ps1' $output
    Pass "cut short in $($Matches[1]) places, every character of the last line among them, install.ps1 runs nothing"

    # --- -Help and the failures -------------------------------------------

    Reset-AppData
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Help"
    if ($output -notmatch '-NoModifyPath' -or $output -match 'THROWN') { Fail '-Help' $output }
    Assert-NothingInstalled '-Help' $output
    Assert-SessionKept '-Help' $output
    Pass '-Help, fetched as application/octet-stream and run as a script block'

    Reset-AppData
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Frobnicate -NoModifyPath"
    if ($output -notmatch "THROWN: .*'Frobnicate'") { Fail 'an unknown option' $output }
    Assert-NothingInstalled 'an unknown option' $output
    Pass 'an unknown option stops it'

    # A release that doesn't exist: first on the local server, whose 404 is
    # certain, then on GitHub. Windows PowerShell 5.1 sometimes gets no
    # answer at all for GitHub's 404 (the connection closed), for every
    # request a process makes, so install.ps1's message for that case, which
    # names the release too, passes there.
    Reset-AppData
    $env:RIG_INSTALL_BASE_URL = "http://localhost:$port"
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Version 9.9.9 -NoModifyPath -NoVSCode"
    $env:RIG_INSTALL_BASE_URL = $null
    if ($output -notmatch 'THROWN: there is no Rig 9\.9\.9') { Fail '-Version 9.9.9' $output }
    Assert-NothingInstalled '-Version 9.9.9' $output
    Assert-SessionKept '-Version 9.9.9' $output

    Reset-AppData
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Version 9.9.9 -NoModifyPath -NoVSCode"
    if ($output -notmatch 'THROWN: (there is no Rig 9\.9\.9|.*and that there is a Rig 9\.9\.9)') { Fail '-Version 9.9.9 from GitHub' $output }
    Assert-NothingInstalled '-Version 9.9.9 from GitHub' $output
    Pass '-Version 9.9.9 stops it: no such release'

    Reset-AppData
    $env:RIG_INSTALL_BASE_URL = "http://localhost:$port/bad"
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Version 0.2.0 -NoModifyPath -NoVSCode"
    $env:RIG_INSTALL_BASE_URL = $null
    if ($output -notmatch 'THROWN: rig-windows-x64\.zip: FAILED, its checksum is not the one in SHA256SUMS') { Fail 'a checksum mismatch' $output }
    Assert-NothingInstalled 'a checksum mismatch' $output
    Pass 'a checksum mismatch stops it'

    # The plain `irm | iex`, which edits the PATH when it installs, can be
    # run here only where it stops before installing: with no network (port
    # 9 of this computer, where nothing listens).
    Reset-AppData
    $env:RIG_INSTALL_BASE_URL = 'http://127.0.0.1:9'
    $output = Invoke-Child "irm $url | iex"
    $env:RIG_INSTALL_BASE_URL = $null
    if ($output -notmatch 'THROWN: .*check your network connection') { Fail 'no network' $output }
    Assert-NothingInstalled 'no network' $output
    Assert-SessionKept 'irm | iex' $output
    Pass 'no network stops it; irm | iex leaves the session as it was'

    # --- Installing ---------------------------------------------------------

    Reset-AppData
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -NoModifyPath"
    if ($output -match 'THROWN') { Fail 'installing the latest release' $output }
    Assert-SessionKept 'installing' $output
    $latest = Get-RigVersion
    if ($latest -notmatch '^rig ') { Fail "rig version says '$latest'" $output }
    if (-not (Test-Path -LiteralPath (Join-Path $appData 'Programs\rig\lld-link.exe'))) { Fail "lld-link.exe isn't beside rig.exe" $output }
    if ($output -notmatch [regex]::Escape($latest)) { Fail "install.ps1 doesn't end with rig version's line" $output }
    if ($output -notmatch 'Built and ran hello world') { Fail "install.ps1's hello world check failed" $output }
    $codeLog = Get-Content -LiteralPath (Join-Path $bin 'code.log') -ErrorAction SilentlyContinue
    if (-not ($codeLog -match '^--install-extension .*\\rig-vscode\.vsix --force$')) { Fail "the extension wasn't installed: $codeLog" $output }
    Pass "installs ${latest}: rig version, hello world, the extension"

    # --- Running it again upgrades ------------------------------------------

    [IO.File]::WriteAllText((Join-Path $appData 'Programs\rig\rig.exe'), 'broken')
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -NoModifyPath -NoVSCode"
    if ($output -match 'THROWN') { Fail 'running it again' $output }
    if ((Get-RigVersion) -ne $latest) { Fail "running it again didn't replace rig.exe" $output }
    Pass 'running it again upgrades in place'

    # --- -Version -----------------------------------------------------------

    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Version 0.2.0 -NoModifyPath -NoVSCode"
    if ($output -match 'THROWN') { Fail '-Version 0.2.0' $output }
    if ((Get-RigVersion) -notmatch '^rig 0\.2\.0 ') { Fail "-Version 0.2.0 installed '$(Get-RigVersion)'" $output }
    Pass '-Version 0.2.0 installs 0.2.0'

    # --- -Uninstall ---------------------------------------------------------

    Remove-Item -LiteralPath (Join-Path $bin 'code.log') -ErrorAction SilentlyContinue
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Uninstall -NoModifyPath"
    if ($output -match 'THROWN') { Fail '-Uninstall' $output }
    Assert-SessionKept '-Uninstall' $output
    if (Test-Path -LiteralPath (Join-Path $appData 'Programs\rig')) { Fail '-Uninstall left the folder' $output }
    $codeLog = Get-Content -LiteralPath (Join-Path $bin 'code.log') -ErrorAction SilentlyContinue
    if (-not ($codeLog -match '^--uninstall-extension woozchucky\.rig-language$')) { Fail "-Uninstall didn't remove the extension: $codeLog" $output }
    if ($output -notmatch 'Rig is uninstalled\.') { Fail "-Uninstall doesn't say Rig is uninstalled" $output }
    Pass '-Uninstall removes the folder and the extension'

    # With the folder gone, -Uninstall still removes the extension (the
    # stand-in `code` says it did), and so says Rig is uninstalled; with
    # nothing left to remove, it says so.
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Uninstall -NoModifyPath"
    if ($output -match 'THROWN' -or $output -notmatch 'Removed the VS Code extension') { Fail '-Uninstall with the folder gone' $output }
    if ($output -notmatch 'Rig is uninstalled\.' -or $output -match 'nothing to remove') { Fail "-Uninstall that removed the extension says there was nothing to remove" $output }
    $output = Invoke-Child "& ([scriptblock]::Create((irm $url))) -Uninstall -NoModifyPath -NoVSCode"
    if ($output -match 'THROWN' -or $output -notmatch 'nothing to remove') { Fail "-Uninstall with nothing installed doesn't say so" $output }
    Pass '-Uninstall says Rig is uninstalled when it removed anything, the extension alone included, and nothing to remove otherwise'

    if ((Get-RawUserPath) -ne $userPathBefore) { Fail 'the user PATH changed' }
    Pass 'the user PATH is as it was'
    Write-Host "test_install.ps1: all passed ($($PSVersionTable.PSEdition) $($PSVersionTable.PSVersion))"
} finally {
    # Close stops it, and its GetContext in the server's thread then throws,
    # which ends the loop. A listener that failed to start is already closed.
    if ($listener) { try { $listener.Close() } catch { } }
    if ($server) { $server.Dispose() }
    $env:Path = $savedPath
    $env:LOCALAPPDATA = $savedAppData
    $env:RIG_INSTALL_BASE_URL = $null
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
