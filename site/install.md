---
title: Install
---

# Install Rig

[![The latest release](https://img.shields.io/github/v/release/WoozChucky/rig)](https://github.com/WoozChucky/rig/releases/latest)

Each release of Rig has the compiler built for 64-bit Windows and Linux, each with LLVM's linker, Rig's runtime and the modules that ship with Rig beside it, and the VS Code extension. The [release notes](https://github.com/WoozChucky/rig/releases/latest) say what is new; [every release](https://github.com/WoozChucky/rig/releases) is on GitHub.

Rig is early: the language will change from one release to the next.

## Install with one command

On **Linux** (x86-64), in a terminal:

```sh
curl -fsSL https://woozchucky.github.io/rig/install.sh | sh
```

On **Windows** (x64), in PowerShell (Windows PowerShell, which comes with Windows, or PowerShell 7):

```powershell
irm https://woozchucky.github.io/rig/install.ps1 | iex
```

Either one installs the latest release for your user alone, with no administrator rights and no questions asked. It does what the [steps by hand](#install-by-hand) below do:

1. It downloads the archive and `SHA256SUMS`, and checks the archive's SHA-256. If it doesn't match, the script stops, with nothing installed.
2. It unpacks the archive into `%LOCALAPPDATA%\Programs\rig` on Windows, or into `~/.local/share/rig` on Linux, linked as `~/.local/bin/rig`, overwriting the files already there. The `modules` folder there is replaced whole, so a file a module no longer has doesn't stay behind.
3. It puts Rig on your `PATH`, unless it is there already. On Windows, it adds the folder to your user `PATH` as [step 4 for Windows](#windows) does. On Linux, when `~/.local/bin` isn't on the `PATH`, it adds this line to the end of `~/.profile` and `~/.bashrc`, and of `~/.zshrc` if you have one, once:

   ```sh
   export PATH="$HOME/.local/bin:$PATH"  # added by Rig's installer
   ```

   For another shell, such as fish, it says what to add.
4. It installs the [VS Code extension](#vs-code-extension), if `code` is on your `PATH`.
5. It builds and runs hello world, to check that Rig works on your system. If the build fails, Rig stays installed, and the script says what to do. From the release after 0.3.0 on, Rig needs nothing else installed, so a failed build is a bug: the script asks you to report it, with the build's output. For 0.3.0 and earlier, it says what to install: the [requirements](#requirements) below. If hello world builds but doesn't run, the script says so: something else stopped the program, such as a temporary folder that doesn't let programs run, or an antivirus.
6. It prints `rig version`'s line, such as `rig 0.2.0 (89de631d10c7 2026-11-13) LLVM 23.1.5`. If it says to, open a new terminal (one opened before doesn't see the new `PATH`); then build a program with `rig build hello.rig`.

**To upgrade**, run the command again: it overwrites the files, and adds nothing to the `PATH` the second time.

**To uninstall**, run it with `--uninstall` on Linux, or `-Uninstall` on Windows, as the next section shows. It removes the folder (and on Linux the link), its entry in your user `PATH` (Windows) or the line it added to each shell file (Linux), and the VS Code extension when `code` is on your `PATH`.

### Options

On Linux, the options go after `sh -s --`; on Windows, the script is run as a script block to take them:

```sh
curl -fsSL https://woozchucky.github.io/rig/install.sh | sh -s -- --version 0.2.0
```

```powershell
& ([scriptblock]::Create((irm https://woozchucky.github.io/rig/install.ps1))) -Version 0.2.0
```

| Linux | Windows | |
|---|---|---|
| `--version 0.2.0` | `-Version 0.2.0` | Install that release instead of the latest. |
| `--no-modify-path` | `-NoModifyPath` | Leave your `PATH` alone: the user `PATH` on Windows, the shell files on Linux. |
| `--no-vscode` | `-NoVSCode` | Don't install the VS Code extension (with the uninstall option: don't remove it). |
| `--uninstall` | `-Uninstall` | Remove Rig. |
| `--help` | `-Help` | Say what the script does, and list the options. |

The scripts are in the repository, to read before you run them: [install.sh](https://github.com/WoozChucky/rig/blob/main/site/public/install.sh) and [install.ps1](https://github.com/WoozChucky/rig/blob/main/site/public/install.ps1).

## Install by hand

The steps the scripts take, one at a time, for those who would rather see each one. (The scripts also build hello world, to check that Rig works; [Requirements](#requirements) says how.)

### Download

| For | File |
|---|---|
| Windows x64 | [rig-windows-x64.zip](https://github.com/WoozChucky/rig/releases/latest/download/rig-windows-x64.zip): `rig.exe`, with LLVM's `lld-link.exe`, Rig's runtime (`rig_runtime.lib`, `ucrtbase.lib`, `kernel32.lib`) and the `modules` folder |
| Linux x86-64 | [rig-linux-x64.tar.gz](https://github.com/WoozChucky/rig/releases/latest/download/rig-linux-x64.tar.gz): `rig`, with LLVM's `ld.lld`, Rig's runtime (`rig_runtime.o`) and the `modules` folder |
| VS Code 1.75 or later | [rig-vscode.vsix](https://github.com/WoozChucky/rig/releases/latest/download/rig-vscode.vsix): the extension |
| | [SHA256SUMS](https://github.com/WoozChucky/rig/releases/latest/download/SHA256SUMS): the SHA-256 of each of the three |

The links always get the latest release. The commands below download it, check it, and install it for your user alone, with no administrator rights. Each archive's files are at its top level: the linker, the runtime's files and the `modules` folder must stay in the same folder as `rig`, which runs the linker from there, links every program with the runtime's files from there, and finds there Preload, which every program imports, and the modules a program imports (`#import "Basic";`): without that folder rig compiles nothing.

### Windows

In PowerShell (Windows PowerShell, which comes with Windows, or PowerShell 7):

1. Download the archive and the checksums into a folder of their own:

   ```powershell
   cd (New-Item -ItemType Directory -Force "$env:TEMP\rig-download")
   irm https://github.com/WoozChucky/rig/releases/latest/download/rig-windows-x64.zip -OutFile rig-windows-x64.zip
   irm https://github.com/WoozChucky/rig/releases/latest/download/SHA256SUMS -OutFile SHA256SUMS
   ```

2. Check the archive against `SHA256SUMS`. This prints `rig-windows-x64.zip: OK`; if it stops with `rig-windows-x64.zip: FAILED` instead, the file is not the one released: download it again, and don't go on.

   ```powershell
   $sum = ((Select-String -Path SHA256SUMS -Pattern ' rig-windows-x64\.zip$').Line -split ' ')[0]
   if ((Get-FileHash -Algorithm SHA256 rig-windows-x64.zip).Hash -eq $sum) { 'rig-windows-x64.zip: OK' } else { throw 'rig-windows-x64.zip: FAILED, its checksum is not the one in SHA256SUMS' }
   ```

3. Unpack it into `%LOCALAPPDATA%\Programs\rig`, overwriting the files already there, then delete the download. The first line removes the `modules` folder an older release put there, so that a file a module no longer has doesn't stay behind:

   ```powershell
   Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Programs\rig\modules" -ErrorAction SilentlyContinue
   Expand-Archive -Force rig-windows-x64.zip "$env:LOCALAPPDATA\Programs\rig"
   cd ~; Remove-Item -Recurse "$env:TEMP\rig-download"
   ```

4. Add that folder to your user `PATH`, unless it is there already:

   ```powershell
   $rig = "$env:LOCALAPPDATA\Programs\rig"
   $path = (Get-Item HKCU:\Environment).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
   if (($path -split ';') -notcontains $rig) {
       $new = if (-not $path) { $rig } elseif ($path.EndsWith(';')) { "$path$rig;" } else { "$path;$rig" }
       New-ItemProperty HKCU:\Environment Path -PropertyType ExpandString -Force -Value $new > $null
       [Environment]::SetEnvironmentVariable('RIG_PATH_CHANGED', '1', 'User'); [Environment]::SetEnvironmentVariable('RIG_PATH_CHANGED', [NullString]::Value, 'User')
   }
   ```

   This edits the user `PATH` as stored, so its other entries stay exactly as they are: one written with a variable, such as `%USERPROFILE%\.dotnet\tools`, keeps the variable. The folder goes at the end, before the `;` the `PATH` may end with, so that taking it off again gives back the `PATH` you had. The last line only tells Windows that the environment changed (setting and removing a variable broadcasts that; `[NullString]::Value` removes it in PowerShell 7 too, where `$null` would leave it set to nothing), so the terminals you open next see the new `PATH`.

5. Open a new terminal (one opened before doesn't see the new `PATH`), and check:

   ```powershell
   rig version
   ```

   It prints the version, the commit it was built from and LLVM's version, such as `rig 0.2.0 (89de631d10c7 2026-11-13) LLVM 23.1.5`.

**To upgrade**, run steps 1 to 3 again: they overwrite the files. The `PATH` is already set.

**To uninstall**, delete the folder, and take it off your user `PATH`:

```powershell
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Programs\rig"
$path = (Get-Item HKCU:\Environment).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
New-ItemProperty HKCU:\Environment Path -PropertyType ExpandString -Force -Value ((($path -split ';') -ne "$env:LOCALAPPDATA\Programs\rig") -join ';') > $null
[Environment]::SetEnvironmentVariable('RIG_PATH_CHANGED', '1', 'User'); [Environment]::SetEnvironmentVariable('RIG_PATH_CHANGED', [NullString]::Value, 'User')
```

### Linux

In a terminal:

1. Download the archive and the checksums into a folder of their own:

   ```sh
   download="$(mktemp -d)" && cd "$download"
   curl -fLO https://github.com/WoozChucky/rig/releases/latest/download/rig-linux-x64.tar.gz
   curl -fLO https://github.com/WoozChucky/rig/releases/latest/download/SHA256SUMS
   ```

2. Check the archive against `SHA256SUMS`. This prints `rig-linux-x64.tar.gz: OK`; if it prints `FAILED`, the file is not the one released: download it again, and don't go on.

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   ```

3. Unpack it into `~/.local/share/rig`, overwriting the files already there, then delete the download. The `modules` folder an older release put there is removed first, so that a file a module no longer has doesn't stay behind:

   ```sh
   mkdir -p ~/.local/share/rig && rm -rf ~/.local/share/rig/modules && tar xzf rig-linux-x64.tar.gz -C ~/.local/share/rig
   cd ~ && rm -rf "$download"
   ```

4. Link `rig` into `~/.local/bin`, the folder for your own programs on the `PATH` (`rig` finds `ld.lld` and `modules` beside the file the link points to):

   ```sh
   mkdir -p ~/.local/bin && ln -sf ~/.local/share/rig/rig ~/.local/bin/rig
   ```

5. Check:

   ```sh
   rig version
   ```

   It prints the version, the commit it was built from and LLVM's version, such as `rig 0.2.0 (89de631d10c7 2026-11-13) LLVM 23.1.5`.

   If the shell can't find `rig`, `~/.local/bin` isn't on your `PATH` yet. Ubuntu's `~/.profile` adds it when the folder exists as you log in, so if step 4 made it, log out and in again. Otherwise add it in `~/.bashrc` and open a new terminal:

   ```sh
   echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
   ```

**To upgrade**, run steps 1 to 3 again: they overwrite the files, and the link stays.

**To uninstall**, delete the folder and the link:

```sh
rm -rf ~/.local/share/rig
rm -f ~/.local/bin/rig
```

## Requirements

From the release after 0.3.0 on, Rig needs nothing installed besides itself. `rig build` links a program with the linker and Rig's runtime beside `rig`, against the C runtime every system has, as section 10.6 of [the specification](/spec/implementation#106-linking) says: on Windows (10 or later) the Universal C Runtime, `ucrtbase.dll`, and `kernel32.dll`; on Linux glibc's shared libraries, `libc.so.6` and `libm.so.6`. Visual Studio and glibc's development files aren't needed, and aren't read when they are installed.

0.3.0 and earlier releases link with the platform's development files, so each system needs them:

- **Windows** needs Visual Studio, or its free [Build Tools](https://visualstudio.microsoft.com/downloads/), with the **Desktop development with C++** workload: it brings the C runtime, the Windows SDK and Microsoft's linker, `link.exe`. Rig links with the newest installation's `link.exe`, or with the `lld-link.exe` beside `rig.exe` when the installation has none, against the same libraries.
- **Linux** needs glibc's development files, among them its start files (`crt1.o`, `crti.o`, `crtn.o`), `libc.so` and `libm.so`: `sudo apt install libc6-dev` on Debian and Ubuntu, `sudo dnf install glibc-devel` on Fedora; Arch's `glibc` has them. Rig links with the `ld.lld` beside it.

Then save hello world as `hello.rig`, in a folder of your own:

```rig
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
```

Build it and run it. On Windows:

```powershell
rig build hello.rig
.\hello.exe
```

On Linux:

```sh
rig build hello.rig
./hello
```

Both print `Hello, World!`. From the release after 0.3.0 on, `rig build` also says how long each of its steps took (`--quiet` leaves that out). `rig build` writes the program in the current folder (`hello.exe` with its debug information, `hello.pdb`, on Windows; `hello` on Linux) and its object file in `.build`. When the linker fails, or something it needs is missing, `rig` prints the reason, then `rig: error: linking failed`.

## VS Code extension

The extension highlights `.rig` files, shows the compiler's errors as you type, and builds, runs and debugs the current file. The [one-line install](#install-with-one-command) installs it when `code` is on your `PATH`; to install it yourself, download [rig-vscode.vsix](https://github.com/WoozChucky/rig/releases/latest/download/rig-vscode.vsix) and install it from the folder it is in:

```sh
code --install-extension rig-vscode.vsix
```

or in VS Code: the Extensions view, its `...` menu, **Install from VSIX...**, and pick the file.

::: warning Not by double-clicking
On Windows, double-clicking the `.vsix` file opens Visual Studio's VSIX Installer when Visual Studio is installed, which fails with "One or more extensions are for Visual Studio Code. Try installing them in Visual Studio Code." Install it from VS Code, as above.
:::

The extension runs the `rig` on your `PATH` (or the one its setting `rig.compilerPath` names): restart VS Code after installing Rig, so it sees the new `PATH`. Debugging also needs Microsoft's [C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools), and gdb on Linux (`sudo apt install gdb`). The extension's [README](https://github.com/WoozChucky/rig/blob/main/editors/vscode/README.md) says what it does and how to set it up.

## Build from source

To build the compiler yourself, with its pinned LLVM release, [CONTRIBUTING.md](https://github.com/WoozChucky/rig/blob/main/CONTRIBUTING.md#building) says how, on Windows and Linux, and how to run its tests.
