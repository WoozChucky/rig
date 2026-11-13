# Third-party licences

Rig's compiler links LLVM's libraries statically, and its release archives ship LLVM's linker beside it (`ld.lld` on Linux, `lld-link.exe` on Windows). LLVM is under the Apache License 2.0 with LLVM Exceptions, which asks that whoever receives it gets a copy of the licence, so the archives carry these two files (CONTRIBUTING.md, "Releasing"):

| File | In the archives as | From |
|---|---|---|
| `llvm/LICENSE.TXT` | `LICENSE-LLVM.txt` | `llvm/LICENSE.TXT` of llvm-project at `llvmorg-23.1.5` |
| `lld/LICENSE.TXT` | `LICENSE-LLD.txt` | `lld/LICENSE.TXT` of llvm-project at `llvmorg-23.1.5` |

They are copied byte for byte. LLVM's release archives, which `scripts/get_llvm.sh` and `get_llvm.bat` download, don't include them. When the scripts' pinned version changes, copy the new release's two files here and change the tag above.
