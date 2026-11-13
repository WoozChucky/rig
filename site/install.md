---
title: Install
---

# Install Rig

[![The latest release](https://img.shields.io/github/v/release/WoozChucky/rig)](https://github.com/WoozChucky/rig/releases/latest)

Each release of Rig has the compiler built for 64-bit Windows and Linux, each with LLVM's linker beside it, and the VS Code extension. The [release notes](https://github.com/WoozChucky/rig/releases/latest) say what is new; [every release](https://github.com/WoozChucky/rig/releases) is on GitHub.

Rig is early: the language will change from one release to the next.

## Download

| For | File |
|---|---|
| Windows x64 | [rig-windows-x64.zip](https://github.com/WoozChucky/rig/releases/latest/download/rig-windows-x64.zip): `rig.exe`, with LLVM's `lld-link.exe` |
| Linux x86-64 | [rig-linux-x64.tar.gz](https://github.com/WoozChucky/rig/releases/latest/download/rig-linux-x64.tar.gz): `rig`, with LLVM's `ld.lld` |
| VS Code 1.75 or later | [rig-vscode.vsix](https://github.com/WoozChucky/rig/releases/latest/download/rig-vscode.vsix): the extension |
| | [SHA256SUMS](https://github.com/WoozChucky/rig/releases/latest/download/SHA256SUMS): the SHA-256 of each of the three |

The links always get the latest release. The commands below download it, check it, and install it for your user alone, with no administrator rights. Each archive's files are at its top level: the linker must stay in the same folder as `rig`, which runs it from there.

## Windows

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

3. Unpack it into `%LOCALAPPDATA%\Programs\rig`, replacing what is there:

   ```powershell
   Expand-Archive -Force rig-windows-x64.zip "$env:LOCALAPPDATA\Programs\rig"
   ```

4. Add that folder to your user `PATH`, unless it is there already:

   ```powershell
   $path = [Environment]::GetEnvironmentVariable('Path', 'User')
   if (($path -split ';') -notcontains "$env:LOCALAPPDATA\Programs\rig") { [Environment]::SetEnvironmentVariable('Path', "$path;$env:LOCALAPPDATA\Programs\rig".TrimStart(';'), 'User') }
   ```

5. Open a new terminal (one opened before doesn't see the new `PATH`), and check:

   ```powershell
   rig version
   ```

   It prints the version, the commit it was built from and LLVM's version, such as `rig 0.2.0 (89de631d10c7 2026-11-13) LLVM 23.1.5`.

**To upgrade**, run steps 1 to 3 again: they replace the files. The `PATH` is already set.

**To uninstall**, delete the folder, and take it off your user `PATH`:

```powershell
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Programs\rig"
[Environment]::SetEnvironmentVariable('Path', (([Environment]::GetEnvironmentVariable('Path', 'User') -split ';') -ne "$env:LOCALAPPDATA\Programs\rig") -join ';', 'User')
```

## Linux

In a terminal:

1. Download the archive and the checksums into a folder of their own:

   ```sh
   cd "$(mktemp -d)"
   curl -fLO https://github.com/WoozChucky/rig/releases/latest/download/rig-linux-x64.tar.gz
   curl -fLO https://github.com/WoozChucky/rig/releases/latest/download/SHA256SUMS
   ```

2. Check the archive against `SHA256SUMS`. This prints `rig-linux-x64.tar.gz: OK`; if it prints `FAILED`, the file is not the one released: download it again, and don't go on.

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   ```

3. Unpack it into `~/.local/share/rig`, replacing what is there:

   ```sh
   mkdir -p ~/.local/share/rig && tar xzf rig-linux-x64.tar.gz -C ~/.local/share/rig
   ```

4. Link `rig` into `~/.local/bin`, the folder for your own programs on the `PATH` (`rig` finds `ld.lld` beside the file the link points to):

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

**To upgrade**, run steps 1 to 3 again: they replace the files, and the link stays.

**To uninstall**, delete the folder and the link:

```sh
rm -rf ~/.local/share/rig
rm -f ~/.local/bin/rig
```

## Requirements

`rig build` links a program with the platform's C runtime, as section 10.6 of [the specification](/spec/implementation) says, so each system needs that runtime's files:

- **Windows** needs Visual Studio, or its free [Build Tools](https://visualstudio.microsoft.com/downloads/), with the **Desktop development with C++** workload: it brings the C runtime, the Windows SDK and Microsoft's linker, `link.exe`. Rig links with the newest installation's `link.exe`, or with the `lld-link.exe` beside `rig.exe` when the installation has none, against the same libraries.
- **Linux** needs glibc's development files, its start files (`crt1.o`, `crti.o`, `crtn.o`) and `libc.so` among them: `sudo apt install libc6-dev` on Debian and Ubuntu, `sudo dnf install glibc-devel` on Fedora; Arch's `glibc` has them. Rig links with the `ld.lld` beside it.

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

Both print `Hello, World!`. `rig build` writes the program in the current folder (`hello.exe` with its debug information, `hello.pdb`, on Windows; `hello` on Linux) and its object file in `.build`. When the linker fails, or something it needs is missing, `rig` prints the reason, then `rig: error: linking failed`.

## VS Code extension

The extension highlights `.rig` files, shows the compiler's errors as you type, and builds, runs and debugs the current file. Download [rig-vscode.vsix](https://github.com/WoozChucky/rig/releases/latest/download/rig-vscode.vsix) and install it from the folder it is in:

```sh
code --install-extension rig-vscode.vsix
```

or in VS Code: the Extensions view, its `...` menu, **Install from VSIX...**, and pick the file.

::: warning Not by double-clicking
On Windows, double-clicking the `.vsix` file opens Visual Studio's VSIX Installer when Visual Studio is installed, which fails with "One or more extensions are for Visual Studio Code. Try installing them in Visual Studio Code." Install it from VS Code, as above.
:::

The extension runs the `rig` on your `PATH` (or the one its setting `rig.compilerPath` names): restart VS Code after installing Rig, so it sees the new `PATH`. Debugging also needs Microsoft's [C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools). The extension's [README](https://github.com/WoozChucky/rig/blob/main/editors/vscode/README.md) says what it does and how to set it up.

## Build from source

To build the compiler yourself, with its pinned LLVM release, [CONTRIBUTING.md](https://github.com/WoozChucky/rig/blob/main/CONTRIBUTING.md#building) says how, on Windows and Linux, and how to run its tests.
