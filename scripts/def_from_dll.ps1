# def_from_dll.ps1: writes a module-definition (.def) file listing the names
# a DLL exports, from which build.bat makes the DLL's import library with
# lld-link /lib (spec 10.6). Rig's runtime/ucrtbase.def and
# runtime/kernel32.def are made by it, from Windows' own DLLs:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\def_from_dll.ps1 -Dll C:\Windows\System32\ucrtbase.dll -Out runtime\ucrtbase.def
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\def_from_dll.ps1 -Dll C:\Windows\System32\kernel32.dll -Out runtime\kernel32.def
#
# A program links against an import library, which says, for each name,
# which DLL the program imports it from when it starts. Visual Studio's
# Windows SDK ships them; Rig makes its own, so that a user of rig needs
# nothing of Microsoft's installed besides Windows. A .def file holds only
# the names, nothing of the DLL itself, so no file of Microsoft's is
# redistributed. Make the files again with this script; don't edit them.
#
# The names come from the DLL's export directory, read from its bytes: the
# first data directory of its PE32+ optional header (at 112 in it), an
# RVA, an address in the loaded image, which the section table turns into a
# place in the file. The directory has the number of names (at 24), and the
# RVAs of the table of functions (28), of names (32) and of ordinals (36):
# the i-th name's function is the one at its ordinal. An export whose
# function lies inside the export directory itself is forwarded to another
# DLL (kernel32's HeapAlloc, to ntdll's RtlAllocateHeap), which the loader
# follows; it is imported from this DLL all the same. One whose function
# lies in a section that isn't executable is data (ucrtbase's _wctype),
# marked DATA, so that the import library gives it only as __imp_NAME, a
# pointer to it: a direct call of it would run its bytes.
#
# The file is the export table's names in its order, which is by name, each
# four spaces in, after a comment saying where it came from and LIBRARY
# with the DLL's name in lower case, with \n line ends whatever the system.
# Windows PowerShell 5.1, Windows' own, runs it, as PowerShell 7 does.

param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes($Dll)

function Read-U16([int64]$at) { [BitConverter]::ToUInt16($bytes, $at) }
function Read-U32([int64]$at) { [BitConverter]::ToUInt32($bytes, $at) }

# The DOS header's e_lfanew (at 0x3C) is where the PE signature is; the
# 20-byte file header after it says how many sections there are (at 6) and
# how long the optional header is (at 20), which follows it.
$pe = Read-U32 0x3C
if ((Read-U32 $pe) -ne 0x00004550) { throw "$Dll isn't a PE file" }
$optional = $pe + 24
if ((Read-U16 $optional) -ne 0x20B) { throw "$Dll isn't a 64-bit (PE32+) file" }
$sectionTable = $optional + (Read-U16 ($pe + 20))
$exportRva = Read-U32 ($optional + 112)
$exportSize = Read-U32 ($optional + 116)

# Each section: its RVA (at 12), its size, the larger of its sizes in
# memory (8) and in the file (16), where it is in the file (20), and
# whether it is executable (IMAGE_SCN_MEM_EXECUTE in its flags, at 36).
$sections = foreach ($i in 0..((Read-U16 ($pe + 6)) - 1)) {
    $at = $sectionTable + 40 * $i
    [pscustomobject]@{
        Rva        = Read-U32 ($at + 12)
        Size       = [Math]::Max((Read-U32 ($at + 8)), (Read-U32 ($at + 16)))
        FileOffset = Read-U32 ($at + 20)
        Executable = ((Read-U32 ($at + 36)) -band 0x20000000) -ne 0
    }
}

function Find-Section([int64]$rva) {
    foreach ($section in $sections) {
        if ($rva -ge $section.Rva -and $rva -lt $section.Rva + $section.Size) { return $section }
    }
    throw "$Dll has no section holding RVA $rva"
}

function Get-FileOffset([int64]$rva) {
    $section = Find-Section $rva
    $rva - $section.Rva + $section.FileOffset
}

# The zero-terminated ASCII string at a place in the file.
function Read-Name([int64]$at) {
    $end = $at
    while ($bytes[$end] -ne 0) { $end++ }
    [Text.Encoding]::ASCII.GetString($bytes, $at, $end - $at)
}

$directory = Get-FileOffset $exportRva
$count = Read-U32 ($directory + 24)
$functions = Get-FileOffset (Read-U32 ($directory + 28))
$names = Get-FileOffset (Read-U32 ($directory + 32))
$ordinals = Get-FileOffset (Read-U32 ($directory + 36))

$file = [IO.Path]::GetFileName($Dll).ToLowerInvariant()
$info = (Get-Item $Dll).VersionInfo
$version = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
$lines = New-Object Collections.Generic.List[string]
$lines.Add("; Made by scripts/def_from_dll.ps1 from $file $($version): the names it exports, for build.bat's import library. Make it again; don't edit it.")
$lines.Add("LIBRARY $file")
$lines.Add('EXPORTS')
for ($i = 0; $i -lt $count; $i++) {
    $name = Read-Name (Get-FileOffset (Read-U32 ($names + 4 * $i)))
    $rva = Read-U32 ($functions + 4 * (Read-U16 ($ordinals + 2 * $i)))
    $forwarded = $rva -ge $exportRva -and $rva -lt $exportRva + $exportSize
    if ($forwarded -or (Find-Section $rva).Executable) {
        $lines.Add("    $name")
    } else {
        $lines.Add("    $name DATA")
    }
}
[IO.File]::WriteAllText($Out, ($lines -join "`n") + "`n")
