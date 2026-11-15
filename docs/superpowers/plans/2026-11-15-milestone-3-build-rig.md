# build.rig: Build Programs and the Compiler Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Jai's build file, trimmed to what Rig has. `build.rig` is an ordinary Rig program that `#import`s the Compiler module and has no `main`. `rig build` runs its top-level `#run`s, which make workspaces through procedures the compiler implements itself (`#compiler`). Then rig builds each workspace as a separate compilation, with the build options it was given (`optimize` runs LLVM's `default<O2>`).

**Architecture:**
- **`#import "NAME";`** is an `Ast_Load` with `is_import`. The loader resolves it to `NAME/module.rig` in the first of two places beside rig: `<rig's directory>/modules`, then `<rig's directory>/../modules`. It goes into the same queue as a `#load`, with the same file identity. An imported file is named by its absolute path.
- **`#compiler`** is a bodiless procedure, allowed only in files inside those places. The checker keeps it out of the C rules and refuses it as a value. The program's lowering refuses a call of one as a diagnostic. The backend skips it.
- **The interpreter** calls a `#compiler` procedure through a hook that the compile-time world installs, with a context pointer. The hook is `compiler_module_call` (`src/compiler_module.cpp`), and the context is a `Build_State`, one per compilation. It reads and writes the Rig structs and strings in the interpreter's native memory.
- **The driver's** `read_and_check` is split into the steps of a `Compilation`. rig build uses them for an ordinary program, for a build program and for each of its workspaces; rig check and interpret mode use them too. A build program is known from the tree right after loading: it reaches the Compiler module's `module.rig` and declares no procedure `main`.
- **The runner** gets placeholders for the modules' places, and a new kind, `tests/build/NAME/`: rig build is run there with no file.

**Tech Stack:** C-style C++23 built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), LLVM 23.1.5's C API (now with the `passes` component), no STL; Rig for `modules/`; POSIX sh, batch and PowerShell for packaging and the installers.

**Spec:** GitHub issue #116 (`gh issue view 116`, its Decisions section is the design), the maintainer's decisions restated below (binding), the decisions this plan makes, and `docs/rig-spec.md`, which Task 8 updates. Executors read all of them. The issue's last "Done when" bullets (closing the milestone, the Release workflow, the roadmap) are the maintainer's steps after the merge, not tasks of this plan.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL.
  - Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`. A new source file is added to `all.cpp` only, and a new test file at the end of `unit.cpp`'s list. A `TEST` name and a file-level `static` name must be unique across the whole build.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material: each new piece says what it does and why, in the existing comments' style. A comment an edit touches cites the spec's current section numbers (2.14 and 7.9 are this plan's new sections).
  - The spec is `docs/rig-spec.md`. The old design doc is no longer updated.
  - Rig code in `modules/` follows `modules/Basic`'s style: a header comment saying what the file is and how a program gets it, then a comment over each declaration.
- **Both hosts for every commit:** Windows before it is pushed, Linux right after (it pulls from origin). A failure on Linux is fixed in a new commit. Everything must pass with no warnings. The debugger tests must PASS, not SKIP: cdb on Windows, gdb on Linux.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\116`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\116\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - Whenever `tests/` or `editors/vscode` changes: `node editors/vscode/test/check.js`. It tokenizes every `tests/run` program and reads every `tests/errors/*.err`.
  - **Linux** (WSL), in `~/rig-wt/116`:
    - If the worktree isn't there, make it from `~/rig`: `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/116 issue/116-build-rig`.
    - PowerShell mangles `$` and quotes in inline WSL commands, so each Linux step is a bash script written into the scratchpad and run with `wsl -e bash -lc "tr -d '\r' < /mnt/c/.../NAME.sh | bash"`.
    - Push first, then bring the commit over from origin: `cd ~/rig-wt/116 && git pull -q --ff-only`.
    - Build and test: `RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests`.
    - The extension's checks: `$(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js`.
  - Before the final review, the `release` builds too: `cmd /c "C:\dev\rig-wt\116\build.bat release"`, and `./build.sh release` on Linux, each followed by both test programs.
- **Messages, verbatim.** These come from the issue and the maintainer. The plan's own additions are in its decisions below.
  - `there's no module 'NAME' (looked in DIR1 and DIR2)`, at the `#import`'s string. DIR1 is `<rig's directory>/modules` and DIR2 is `<rig's directory>/../modules`, each absolute.
  - `'../x' isn't a module's name`, at the string.
  - `#compiler is only for the modules that ship with rig`, at the `#compiler`.
  - `'create_workspace' can't be a value: the compiler does it`, at the name.
  - `'create_workspace' can only be called while compiling`, at the call (a diagnostic of the program's lowering).
  - `a program with 'main' can't create workspaces`: a runtime error, with frames.
  - `there's no workspace with id N`, `there's already a workspace 'game'`, and `there's no file 'src/x.rig'`: runtime errors, with frames.
  - `the build program created no workspace`, at its `#import "Compiler"`, or at the `#load` that reached the module.
  - `workspace 'game' has no files`, at the `create_workspace` call that made it.
  - `workspace 'game' is a build program; a workspace builds an ordinary program`, at that `create_workspace` call.
  - `rig: error: no file given and no build.rig in the current directory` (status 1).
  - `rig: error: -o can't be used with a build program; its build options set the output` (status 2).
  - A runtime error during a `#run` is reported as today: `#run failed: runtime error: MESSAGE`, at the `#run`, followed by the frames.
- **Work flow:**
  - Branch `issue/116-build-rig`, worktree `C:\dev\rig-wt\116`. This plan is its first commit, already pushed.
  - Open the draft PR after Task 1's commit if it isn't open: `Closes #116`, labels `language`, `tooling` and `enhancement`, milestone `3: Compile-time execution`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at 7122e47)

1. **Lexer:** `#import` and `#compiler` lex already, as `TOKEN_DIRECTIVE` with text `import` and `compiler` (`lexer.cpp:173`). No lexer change is needed.
2. **Parser** (`parser.cpp`):
   - `is_directive` (`113`).
   - The `#load` sites, each of which needs an `#import` twin:
     - `error_misplaced_load` (`118-122`) and `error_expected_or_load` (`132-139`);
     - the for-loop iterator exemption (`918`);
     - `declares_in_column_1` (`1012-1017`);
     - `skip_misplaced_load` and `report_misplaced_loads` (`1037-1045`), used by `parse_block_rest` (`1097`) and `parse_struct` (`1490`);
     - `parse_procedure`'s directive loop (`1414`);
     - `parse_load` (`1537-1561`) and `parse_declaration` (`1620-1621`);
     - `starts_load` (`1690-1693`) and `load_resumes_here`;
     - the recovery's `lost_a_file` (`1777`, `1810`);
     - `parameters_follow` (`1849`) and `variable_follows` (`1883`);
     - `parse_into` (`1994-2007`).
   - `parse_procedure`'s directive loop (`1399-1442`) takes `#c_call`, `#no_abc` and `#foreign`. Anything else is `unknown directive '#NAME'`.
   - The grammar comment is `parser.h:7-45`, and the `#load` prose is `parser.h:165-190`.
3. **AST:**
   - `Ast_Proc` (`ast.h:146-180`): `body` is null for a foreign procedure, and the struct has `is_foreign`, `is_c_call` with `c_call_pos`, `body_resolve` and `had_errors`.
   - `Ast_Load` (`ast.h:438-442`): `path` and `path_pos`.
   - `Ast_File` (`ast.h:479-485`, its comment above): `procs`, `globals`, `loads`, `runs` and `lost_a_file`.
   - `ast.cpp` dumps a procedure (`110-147`, `(foreign libc)` at `133-141`) and a load (`434-438`, `(load "a.rig")`).
4. **Loader** (`load.cpp`, its rules in `load.h:1-46`):
   - `load_program(a, d, path, text, stdin_for = {}, stdin_text = {}, stdin_for_reached = nullptr)` (`load.h:72`). It has eight call sites: `driver.cpp:118`, `tests/unit/interpret_mode.cpp:93`, `test_ir.cpp:3357`, and `test_load.cpp:40, 194, 714, 740, 801`.
   - The queue loop is at `load.cpp:107-158`, its loop over each file's new loads at `118`. Each new `tree->loads[k]` is resolved with `load_path` (`133`).
   - File identity is `File_Key`, `file_key` and `first_time_seen` (`19-69`). A file named twice is skipped silently (`144`).
   - Each added file gets `diag_add_file` and `diag_locate_file` (`155-156`; the main file's at `102-103`).
5. **Paths and the OS:**
   - `os_executable_directory(Arena *)` (`os.h:58-59`): Windows' is `GetModuleFileNameW`'s directory; Linux's is `/proc/self/exe` with its links resolved.
   - `path_absolute` and `load_path` (`base.h:322-353`): they work on the text alone. On Windows `path_absolute` gives `\` only; `load_path` keeps the text as written (`from` up to its last separator, then `written`), and normalises only after a `\\?\` prefix.
   - `os_list_directory` (`os.h:56`) lists a directory's names, sorted.
   - `Source_File` (`diag.h:31-41`): `path`, `text`, `absolute_path` and `directory`.
6. **Checker:**
   - `check_signature` (`checker.cpp:1751-1782`): `c_side = proc->is_foreign || proc->is_c_call` drives `check_c_part`.
   - The `main` rule (`checker.cpp:1960-1973`): `a procedure called 'main' is needed` at `{0, 1, 1}`, unless `lost_a_name` or `lost_a_file`.
   - `check_name`'s `AST_PROC` case (`check_expr.cpp:626-647`) refuses `main` as a value and warns about a variadic one.
   - A bodiless procedure is already fine for the checker and for the `#run` walk (`checker.cpp:1956`; `check_reached_body`).
7. **Lowering:**
   - `Lowering.diag` is "only for the source files' paths" (`lower.cpp:223`).
   - `declare_proc` (`lower.cpp:2737-2794`) reads `source->body->pos` and queues on-demand bodies for every non-foreign procedure (`2779-2788`).
   - `ir_lower` (`2855-2913`) declares every procedure and lowers every body but foreign ones.
   - `lower_call_results` (`1578-1622`) makes direct calls (`1610-1611`). An emitted instruction's position is `l->pos`, the statement's (`326`, `337`). A runtime check sets `l->pos` to its own place around its instructions (`emit_runtime_check`, `816-829`).
   - `ir_print` prints `foreign proc`, `export proc` and `proc` (`ir.cpp:460-474`). `Ir_Proc` is at `ir.h:351-378`.
8. **Backend:**
   - `backend_build` (`llvm_backend.cpp:1706-1845`) makes the machine with `LLVMCodeGenLevelNone` (`1724`) and verifies the module (`1839`).
   - Foreign procedures are skipped in the definition loop and the body loop (`1803-1833`).
   - `llvm_ir_text` (`1855-1861`) and `llvm_emit_object` (`1863-1879`, declared at `llvm_backend.h:49-52`).
   - `LLVMRunPasses`, `LLVMCreatePassBuilderOptions` and `LLVMDisposePassBuilderOptions` are in `llvm-c/Transforms/PassBuilder.h`.
   - The component lists are `build.sh:94-96` and `build.bat:73-78`. The libraries `passes` pulls in are in `deps/llvm-windows/lib`.
9. **Interpreter and world:**
   - `IR_CALL` (`interp.cpp:537-600`) branches to C for a foreign callee. A callee with no blocks fails with `call of an address that isn't a procedure`.
   - `interp_fail` (`461-479`) records the frames.
   - `Compile_Time` (`run.cpp:62-69`) makes its parts lazily in `make_parts` (`89-94`).
   - `report_failure` (`run.cpp:339-359`) makes `#run failed: runtime error: MESSAGE` and the frames.
   - A string is 16 bytes: `count` at `VIEW_COUNT_OFFSET` 0, `data` at `VIEW_DATA_OFFSET` 8 (`types.h`).
   - An aggregate result is a hidden `ptr` parameter, first. An aggregate parameter is a `ptr` to the caller's copy (`lower.cpp:106-117`, `1584-1599`).
10. **Driver:**
    - `read_and_check` (`driver.cpp:89-153`) runs, in order: load; `diag_print_at_exit(&d)`; the world; `check`; the top-level runs (rig build only); print if errors; `diag_print_at_exit(nullptr)`; free the world; `ir_lower` when ok; `diag_free`.
    - `build()` (`156-188`) writes `.build/NAME.obj` and links `executable_path`'s output.
    - `command_build` (`211-252`) gives 2 and the usage when there is no file (`242-245`).
    - The usage is `main.cpp:5-10`.
    - `tests/unit/interpret_mode.cpp:78-125` copies `read_and_check`. It is in the same unity build as `driver.cpp` (`unit.cpp:13`), so it can call `driver.cpp`'s static functions.
11. **Runner** (`tests/runner.cpp`):
    - Its header is `1-157`.
    - `build_command` (`313-321`) always passes `-o`.
    - `read_expected_errors` (`510-518`) replaces `@NO_SUCH_FILE@`.
    - `error_test` (`520-547`).
    - `COMMAND_CASES` (`734-762`) and `command_test` (`775-814`).
    - `check_error_test` (`1192`).
    - `run_directory` (`1001`).
    - `main` (`2188-2231`): RIG is `args[1]` or `bin/rig`, and the interpreter is always `bin/unit-tests`.
    - The runner includes `base.cpp` and the OS layer (`161-167`), so it has `path_absolute`.
12. **Modules and tests:**
    - `modules/Basic/module.rig:8-15` and `modules/Basic/README.md` ("Loading it") say to `#load` Basic.
    - 26 `tests/run/basic_*.rig` load it with `#load "../../modules/Basic/module.rig";`. 16 `.err` files name its files as `tests/run/../../modules/Basic/FILE.rig`.
    - `editors/vscode/test/check.js` walks `tests/run` and `modules` for the grammar test (`985-1015`). It requires every non-indented line of a `tests/errors/*.err` that isn't a `.check.err` to be `tests/errors/NAME.rig:L:C: error: ...` or a file of `tests/errors/load/` (`107-150`, `251-299`).
13. **Packaging:**
    - `scripts/package_linux.sh` and `scripts/package_windows.bat` pack a flat archive, then build and run `tests/run/hello.rig` with the packed rig.
    - `site/public/install.sh:357-362` moves each top-level entry with `mv -f`, which fails on a non-empty directory.
    - `site/public/install.ps1:287` copies `unpacked\*` without `-Recurse`, which copies a directory without its contents.
    - `scripts/test_install.sh:300-332` has a stand-in release served from `file://`. `test_install.ps1` has none.
    - CONTRIBUTING's "Releasing" lists the archives' contents (`CONTRIBUTING.md:96`). `site/install.md:81` says the archive's files are at its top level.

## The maintainer's decisions (binding; they settle what the issue leaves open)

1. **Where modules are found: two places, in order.**
   - The first place is `<rig's directory>/modules`: a release archive, flat as today, with `modules/` beside `rig.exe`.
   - The second is `<rig's directory>/../modules`: the repository, with `bin/rig.exe` beside the root's `modules/`.
   - The first place that holds `NAME/module.rig` wins.
   - This amends the issue's lookup rule, which names only `../modules` and a `bin/` layout in the archive.
   - The installers and the archive layout stay as they are, apart from shipping `modules/`. The packaging scripts copy `modules/` beside the executable. Their try-out builds a small build program, and a program that `#import`s Basic.
   - The unknown-module message names both places: `there's no module 'NAME' (looked in DIR1 and DIR2)`.
2. **Module names** are letters, digits and `_`, starting with a letter.
   - Anything else is `'../x' isn't a module's name`, at the string.
   - The lookup is case-sensitive on Windows too: it compares the name against the directory's entries, so `#import "basic"` doesn't find `Basic` there either.
3. **Paths of imported files.**
   - An imported file is named by its absolute path, in diagnostics and in runtime errors, so editors and debuggers find it.
   - The test runner replaces the modules directory with `@MODULES@` when it compares `.err`, `.interp.err` and `.check.err` files. CONTRIBUTING documents the placeholder.
   - The unknown-module message's directories get the same treatment in tests.
4. **`#import` in the tree** is an `Ast_Load` with `is_import`, so the loader's loop and its file identity serve both. A file imported or loaded twice is read once, under the name of whichever reached it first.
5. **What counts as shipped with rig:**
   - A file is a module's when its path is inside the modules directory found in decision 1, whether `#import` or `#load` reached it.
   - `#compiler` outside such a file is `#compiler is only for the modules that ship with rig`, at the directive.
6. **Build programs.**
   - A build program is a program that reaches the Compiler module's `module.rig`, through `#import` or `#load`, and declares no procedure called `main`. A global variable or constant named `main` doesn't count: it is no procedure, as the `main` rule already has it (`checker.cpp:1962-1966`). (It has no error of its own: in an ordinary program it gets `a procedure called 'main' is needed`, a rule a build program skips, so in a build program it is an ordinary global.)
   - The command-line refusals (`-o`, `--no-bounds-checks`) come after checking, before the top-level `#run`s. A program with errors reports them, with status 1, before any refusal.
7. **`#compiler` procedures.**
   - The checker exempts them from the C rules for foreign procedures: they take and return Rig structs and strings.
   - **As values:** a `#compiler` procedure can't be a value: `'create_workspace' can't be a value: the compiler does it`, at the name.
   - **At run time:**
     - The final program's lowering refuses a call with `'create_workspace' can only be called while compiling`, at the call. It is raised as a diagnostic: lowering gets a way to report errors, and the driver prints diagnostics after lowering too.
     - The compile-time module calls them through a new interpreter branch: a hook the driver installs, with a context pointer.
8. **Failures.**
   - These are runtime errors, with frames:
     - a workspace that doesn't exist: `there's no workspace with id N`;
     - two workspaces of one name: `there's already a workspace 'game'`;
     - `add_build_file` of a file that isn't there, checked when the call is made: `there's no file 'src/x.rig'`. The message has the path as given; the check uses it resolved against the build program's directory.
   - These are diagnostics in the build program:
     - `the build program created no workspace`, at its `#import "Compiler"` (or the `#load` that reached it);
     - `workspace 'game' has no files`, at the `create_workspace` call that made it, whose position the workspace keeps.
   - **Nested build programs:** a workspace whose program is itself a build program is `workspace 'game' is a build program; a workspace builds an ordinary program`, at that `create_workspace` call.
   - **A failed top-level `#run` stops the build:** no workspace is built, and rig exits 1.
   - **`exit(n)`, anywhere:** in a build program's `#run`, or in the `#run`s of a workspace's program, `exit(n)` exits rig with `n`. It prints the diagnostics found so far first, as `exit()` does in any `#run` today.
9. **Optimisation.**
   - Link LLVM's `passes` component, in the first task that needs it. Check the build and the test binaries on both hosts early, including any new zlib/zstd stub symbols.
   - With `optimize`, run `default<O2>` through `LLVMRunPasses`, and give the target machine the optimised codegen level. Debug information stays.
10. **Tests.**
    - A new kind, `tests/build/NAME/`: a directory holding `build.rig` and its sources. The runner copies it to `.build/tests/build/NAME/`, runs `rig build` there with no file, and compares:
      - the status with `NAME/expected.exit` (0 if absent);
      - rig's stdout and stderr with `expected.build.out` and `expected.err` (empty if absent);
      - for each executable listed in `expected.programs` (one name per line, the platform's suffix added), the program's output with `PROGRAM.out`.
    - The runner never passes `-o` to a build program, and leaves build programs out of its `-o` and interpreted passes.
    - Positioned errors in build files stay in `tests/errors` as single files, so the extension's checks read them as they read the others. `rig check` writes nothing for a build program, so the cases that only `rig build` finds have empty `.check.err` files.
    - A spec example of a build program is a `rig fragment`, checked by `rig check`.
11. **Basic switches to `#import`.**
    - The `tests/run/basic_*` programs `#import "Basic"`.
    - One keeps `#load` (`basic_print`). A new `basic_import_and_load` does both, and shows the file is read once.
    - Their `.err` files use `@MODULES@`.
    - `modules/Basic/README.md` and `module.rig`'s header say how to import it.
12. **Spec.**
    - The issue's list: 2.7, a new section on modules and `#import`, a new section on build programs and the Compiler module, 10.1, 10.2 and appendix A.
    - Also 2.13 (load order with imports) and 9.3 (the new messages).
    - The two new sections go where #115 put 7.8: inside existing chapters, so chapters 9 and 10 keep their numbers.

## Decisions this plan makes (where the decisions above leave a choice)

1. **Task order.** Optimisation in the backend is Task 1. It depends on nothing, and linking `passes` is the riskiest build change, so it is checked first on both hosts (decision 9). Its end-to-end test (`tests/build/options`) comes in Task 6, with workspaces.
2. **The new sections:**
   - 2.14, "Modules and #import", after 2.13 "Program structure";
   - 7.9, "Build programs and the Compiler module", after 7.8.
3. **The places, exactly.**
   - `module_places` makes them with `path_absolute(a, exe_dir, "modules", HOST_IS_WINDOWS)` and `path_absolute(a, exe_dir, "../modules", ...)`: absolute, `..` collapsed, the host's separators.
   - "The modules directory found in decision 1", for decision 5, is either place: a file is a module's when its `absolute_path` is inside one of the two. On Windows the comparison folds case (`os_fold_case`). This holds for the main file too, so `rig check modules/Compiler/module.rig` checks clean (with Task 5's build-program rule).
   - A module's path is `PLACE + separator + NAME + separator + "module.rig"`, the host's separator.
   - The lookup lists the place (`os_list_directory`), looks for an entry exactly `NAME`, and then requires `os_file_exists` of its `module.rig`.
4. **The messages the decisions don't give:**
   - `#import can only be at the top level`, the twin of `#load`'s;
   - `expected a string after #import, as in #import "Basic";`;
   - `a #compiler procedure is the compiler's: it takes no '#c_call'`, at the `#c_call` (as `#foreign` has it). `#no_abc` before `#compiler` is `expected '{'`, as before `#foreign`.
5. **`load_program`'s optional parameters become one `const Load_Options *options = nullptr`** (stdin-for, the module places, and Task 6's further files). The two call sites that use the stdin-for parameters change; the other six don't.
6. **Placeholders.**
   - The runner works out the two places from RIG's directory, as rig does: `@BIN_MODULES@` for `<RIG's directory>/modules` and `@MODULES@` for `<RIG's directory>/../modules`, each made absolute from the current directory. The interpreted pass uses `bin/unit-tests`'s directory.
   - Before comparing a stderr with a `.err`, `.interp.err`, `.check.err` or `expected.err`, the runner replaces each place's text with its placeholder. On Windows it also turns each `\` into `/` in the rest of that path, up to the first `:`, `'`, `)`, space or line break. So one expectation serves both hosts: `@MODULES@/Basic/array.rig:43:23: runtime error: ...`.
   - The tests assume the repository's layout (`bin/` beside `modules/`). CONTRIBUTING says so.
7. **The build-program fact.**
   - The loader sets `Ast_File.reaches_compiler` and `compiler_pos` when it adds the file that `module_lookup(places, "Compiler")` finds, compared by file identity (`file_key`). `compiler_pos` is the position of the `#import` or `#load` that first named that file; for the main file itself, `{its index, 1, 1}`.
   - `ast_is_build_program(file)` is `reaches_compiler` and no `Ast_Proc` named `main` in `file->procs`. Broken entries count: a broken `main` is no build program.
8. **A `#compiler` call is at the call's name.** Lowering emits a `#compiler` call with `l->pos` set to the callee name's position (`call->callee->pos`) and restores it after, as a runtime check does (Facts 7). So these are all the name:
   - a failure's innermost frame;
   - the position the hook gets, which `Build_Workspace.made_at` keeps;
   - the lowering refusal.
9. **The hook.**
   - The type is `Interp_Compiler_Call` (Task 4's interfaces). An interpreter with no hook fails a `#compiler` call with `'NAME' can only be called while compiling`.
   - `compiler_module_call` fails a name it doesn't implement with `the compiler has no procedure 'NAME'`, and a declaration whose IR parameter count isn't the one it expects with `'NAME' isn't declared as the compiler implements it`. It trusts the parameter types of `modules/Compiler/module.rig`, which ships with it.
   - It reads a string's bytes straight from native memory. A string whose `data` isn't readable crashes the compiler, as one passed to C at compile time does.
10. **Every compilation has a `Build_State`:** rig build's, rig check's, each workspace's and interpret mode's.
    - Its `is_build_program` is `ast_is_build_program(tree)`. A workspace's, and an ordinary program's, is false, so `create_workspace` there fails with `a program with 'main' can't create workspaces`.
    - Workspace ids start at 1, in creation order, so a zeroed `Workspace` names none.
    - A workspace's name may be `""`.
11. **Two more runtime errors**, by analogy with `#load` and with `-o`:
    - `add_build_file` of a path that doesn't end in `.rig`: `'src/x.txt' isn't a .rig file`, checked before the file's existence;
    - `set_build_options` with an `output_name` ending in `.rig`: `the output 'game.rig' ends in .rig; is it the source file?`.
    - Every procedure checks its workspace first.
12. **`--no-bounds-checks`'s refusal** is `rig: error: --no-bounds-checks can't be used with a build program; its build options set the bounds checks` (status 2). With both options, `-o`'s alone is said.
13. **A workspace of several files.**
    - Its first file is the main file. The others are loaded right after it, before the files it `#load`s, in the order they were added, as if it `#load`ed each first.
    - Each is named by its path resolved against the build program's directory (`load_path`), which `add_build_file` stores.
    - The driver reads them, as it reads a main file: one that can't be read is `rig: error: cannot read 'PATH': MESSAGE`, and that workspace fails.
14. **A workspace's outputs**, D being the build program's main file's path:
    - the executable is `executable_path(a, load_path(a, D, NAME, windows), {}, target)`, NAME being `output_name`, or `program_name` of the first file when that is empty;
    - the objects go to `load_path(a, D, ".build", windows)`, as `NAME.obj` or `NAME.o` after the first file, as rig build names them.
15. **The order after a build program's runs:**
    1. If no workspace was made, the no-workspace error, and stop (status 1).
    2. Every workspace with no files gets its error, and these are printed together.
    3. Each workspace with files is compiled, in creation order: its diagnostics are printed when its compilation ends. A nested build program is reported in the build program's diagnostics, printed right then.
    4. The status is 1 if anything failed, else 0.
    - The build program's diagnostics stay alive until the end. After each print their list is emptied, and their error count stays.
16. **rig check installs the `Build_State` too,** since an expression `#run` may make a workspace. It builds nothing, and says none of the workspace errors (decision 10's empty `.check.err`).
17. **Interpret mode uses the driver's `Compilation` steps** in place of its copy. It refuses a build program with `unit-tests: error: a build program can't be interpreted`, status 1.
18. **Build programs in `tests/errors` are named `build_*.rig`.** `error_test` builds them without `-o`, and fails if `tests/errors/.build` appears.
19. **The `tests/build` kind:**
    - A case is a subdirectory holding `build.rig`, reported as `tests/build/NAME`.
    - Its copy is emptied first, and its files are copied at any depth.
    - Each listed program must exit 0 with nothing on stderr.
    - With no `expected.programs`, rig must have made no `.build` in the copy.
    - A fixed case, `rig build sub/build.rig from its parent`, covers a build program outside the current directory.
20. **The installers:** each replaces `modules/` whole when it installs. `install.sh` removes `$share_dir/modules` before its `mv`, and `install.ps1` removes `$dir\modules` before copying with `-Recurse`. That is the least change that lets them install the new archive (Facts 13). `test_install.sh`'s stand-in release gains a `modules/`, and an upgrade must replace it.
21. **`modules/Compiler/README.md`** lists the module's declarations, as `modules/Basic/README.md` does Basic's.
22. **`llvm_ir_text` gains `bool optimize = false`,** for the unit tests of the pipeline. `llvm_emit_object` takes `optimize` before its arena.

## Review Focus

1. **Two compilations in one process share nothing.** Each workspace has its own globals, `#run`s, diagnostics and `diag_print_at_exit` registration. Task 6's `tests/build/two_workspaces` covers it: both programs declare `main` and `label`, and each prints from its own top-level `#run`.
2. **A build program outside the current directory.** Its files, `.build` and outputs are relative to its own directory. Task 6's fixed case `rig build sub/build.rig from its parent` covers it.
3. **Strings crossing into C++.** A name or an `output_name` made in a buffer the program overwrites after the call must keep the value it had. Task 4's `build_options_keep_their_own_copy_of_the_name` covers it.
4. **One module named two ways.** `#import "Basic"` plus a `#load` of its `module.rig` reads the file once, under the first name. Windows' case-blind file system must not find `basic`. Task 2's `import_loads_a_module_once` and `import_is_case_sensitive`, and the tests `basic_import_and_load` and `tests/errors/import_wrong_case`, cover it.
5. **Errors only lowering finds.** In an ordinary program they are printed sorted, rig exits 1, and no object is written. That includes a never-called helper that calls a `#compiler` procedure. Task 3's `lowering_refuses_compiler_calls` and `tests/errors/compiler_called_at_run_time` cover it.

---

### Task 1: Optimisation in the backend

**Files:**
- Modify: `build.sh` (`94-96`), `build.bat` (`73-78`): add `passes` to the components, with the comments saying what it is for
- Modify: `src/llvm_backend.h` (the header's "Milestone 2a doesn't optimise.", `llvm_ir_text`, `llvm_emit_object`)
- Modify: `src/llvm_backend.cpp` (`backend_build`, a new `run_passes`, `llvm_ir_text`, `llvm_emit_object`)
- Modify: `src/llvm_compression_stubs.cpp`, only if the link names new zlib or zstd symbols
- Modify: `src/driver.cpp` (`build()` passes `false`), `CONTRIBUTING.md` (the component list, if it names them)
- Test: `tests/unit/test_llvm.cpp`

**Interfaces:**
```cpp
// llvm_backend.h:
// With optimize, LLVM's default<O2> pipeline runs on the module once it
// verifies, and the target machine compiles at its default (optimising)
// level; without, neither (spec 10.2). Debug information is kept either way.
bool llvm_ir_text(Arena *a, Ir_Module *m, Target t, String *text, String *error, bool optimize = false);
bool llvm_emit_object(Ir_Module *m, Target t, String path, bool optimize, Arena *a, String *error);
// llvm_backend.cpp:
static bool backend_build(Backend *b, Ir_Module *m, Target t, bool optimize, Arena *a, String *error);
// default<O2> on b->module with b->machine (LLVMRunPasses, its options made
// and disposed); false with *error from LLVMGetErrorMessage when it fails.
static bool run_passes(Backend *b, Arena *a, String *error);
```

- [ ] **Step 1: Write the failing tests** in `test_llvm.cpp`, under `// Issue #116: optimize (spec 10.2).`
  - `llvm_optimises_with_the_o2_pipeline`:
    - Lower a program as `objects_for_both_targets` lowers its module (`lower_tokens`), so the module has debug information. Its `main` has a local and calls a small procedure, scalars only (no aggregate stays in memory once SROA has run), so the unoptimised IR has an `alloca` and the optimised one none.
    - For each target:
      - `llvm_ir_text(..., false)` contains `alloca` and `DICompileUnit`;
      - `llvm_ir_text(..., true)` succeeds, contains no `alloca`, and contains `DICompileUnit`.
  - `objects_for_both_targets` loops over `optimize` false and true. Each object is written, and has its debug sections, for both targets.
- [ ] **Step 2: Run** the Windows build, then `.\bin\unit-tests.exe llvm_`. The new test fails to compile (the parameters).
- [ ] **Step 3: Implement.**
  - Add `passes` to both scripts' `llvm-config` component lists. If the link reports undefined zlib or zstd symbols, add failing stubs in the existing style.
  - `backend_build` gets `optimize`: `LLVMCodeGenLevelDefault` when set, `LLVMCodeGenLevelNone` otherwise.
  - Both public functions call `run_passes` after a clean `backend_build` when `optimize` is set.
  - `build()` passes `false`.
  - The header comment says what `optimize` does, and that the default is unoptimised.
- [ ] **Step 4: Run all tests** on both hosts, debug and release. Compare `bin/rig`'s size before and after in the commit body, if it grew noticeably.
- [ ] **Step 5: Commit** `Optimise with LLVM's O2 pipeline when asked`. Push, and open the draft PR.

### Task 2: `#import` and the modules' places

**Files:**
- Modify: `src/ast.h` (`Ast_Load.is_import`, its comment, and `Ast_File`'s comment), `src/ast.cpp` (the dump `(import "Basic")`)
- Modify: `src/parser.h` (the grammar comment, and the `#load` prose extended to `#import`)
- Modify: `src/parser.cpp`:
  - a new `is_load_or_import(Token *)`, used at every site of Facts 2;
  - `error_misplaced_load` gets the directive's name;
  - `parse_load` serves both, with the string message per directive.
- Modify: `src/load.h` (the rules comment, `Module_Places`, `module_places`, `module_lookup`, `Load_Options`, `load_program`), `src/load.cpp`
- Modify: `src/driver.cpp:118`, `tests/unit/test_load.cpp:801` (to `Load_Options`)
- Modify: `tests/runner.cpp` (the placeholders: header, `main`, and every comparison of a stderr with an expectation file)
- Modify: `modules/Basic/module.rig` (`8-15`), `modules/Basic/README.md` ("Loading it")
- Modify: the 25 `tests/run/basic_*.rig` other than `basic_print` (`#import "Basic";`, and any comment that says `#load`), and their 16 `.err` files
- Modify: `CONTRIBUTING.md` (the tests list: `@MODULES@`, `@BIN_MODULES@`, the repository's layout)
- Test: `tests/unit/test_parser.cpp`, `tests/unit/test_load.cpp`
- Create: `tests/run/basic_import_and_load.rig` and `.out`
- Create, each `.rig` and `.err`: `tests/errors/import_unknown_module`, `import_bad_name`, `import_wrong_case`, `import_in_a_body`

**Interfaces:**
```cpp
// ast.h, Ast_Load:
bool is_import;  // #import "NAME": path holds NAME, a module that ships with rig (spec 2.14),
                 // which the loader finds; #load "PATH" otherwise

// load.h:
// Where #import looks for a module (spec 2.14), in order:
// <rig's directory>/modules, a release archive's, beside rig; then
// <rig's directory>/../modules, the repository's, bin/ beside modules/.
// Each absolute, with the host's separators (path_absolute).
struct Module_Places {
    String places[2];
};
// The places of the rig running (os_executable_directory).
Module_Places module_places(Arena *a);
// The module NAME's module.rig: the first place with an entry named exactly
// NAME (os_list_directory: case counts on Windows too) whose module.rig
// exists; {} when there is none. NAME must be a module's name (letters,
// digits and '_', a letter first), which the caller checked.
String module_lookup(Arena *a, const Module_Places *places, String name);

struct Load_Options {
    String stdin_for;            // as load_program's stdin_for was (rig check --stdin-for)
    String stdin_text;
    bool *stdin_for_reached;
    const Module_Places *modules;  // null: module_places(), rig's own
};
Ast_File *load_program(Arena *a, Diagnostics *d, String path, String text, const Load_Options *options = nullptr);
```

- **The loader's `#import` branch,** in the loop at `load.cpp:118`. When `load->is_import`:
  1. A name that isn't a module's is `'NAME' isn't a module's name`. `""`, `../x`, `a/b`, `9lives` and a zero byte are all refused. `lost_a_file` is set.
  2. If `module_lookup` gives `{}`, the error is `there's no module 'NAME' (looked in P0 and P1)`, and `lost_a_file` is set.
  3. Otherwise the found path goes down the `#load` path from `file_key` on: the same identity, read once, the same read error. It is named by that absolute path.
  - The `.rig`, empty and zero-byte checks stay `#load`'s.
- **Recovery:** every rule `#load` has, `#import` has too, through `is_load_or_import`.
  - A misplaced one is `#import can only be at the top level`.
  - One without a string is `expected a string after #import, as in #import "Basic";`.
- **Runner:**
  - Two statics are set in `main`: `MODULES` and `BIN_MODULES` for RIG, and the same pair for `bin/unit-tests`.
  - `static String with_module_placeholders(Arena *a, String text, String modules, String bin_modules)` implements decision 6.
  - It is applied to the actual stderr before every comparison with an expectation file: in `run_test`, `interpreted_test`, `error_test`, the check tests from a file and from stdin, and `check_error_test`. `@NO_SUCH_FILE@` stays an expected-side replacement.

- [ ] **Step 1: Write the failing tests.**
  - `test_parser.cpp`, under `// Issue #116: #import (spec 2.14).`:
    - `parse_import`: `#import "Basic";\nmain :: () {}\n` gives no errors and the dump `(import "Basic")\n(proc main (params) (block))\n`. `loads.count` is 1 and `loads[0]->is_import` is set. `#load "a.rig";` stays `(load "a.rig")` with `is_import` clear.
    - `parse_import_errors`. The errors, then the facts, for each:
      - `#import Basic;\nmain :: () {}\n`: `1:9: expected a string after #import, as in #import "Basic";`, and `lost_a_file`;
      - `main :: () { #import "Basic"; }\n`: `1:14: #import can only be at the top level`, and `lost_a_file`;
      - `f :: ()\n#import "Basic";\nmain :: () {}\n`: `2:1: expected '{', found directive`, and the dump `(broken f)\n(import "Basic")\n(proc main (params) (block))\n`;
      - `S :: struct {\n    x: int;\n#import "Basic";\nmain :: () {}\n`: `3:1: expected '}', found directive`, and the import kept, as `#load`'s twin test has it.
  - `test_load.cpp`, under `// Issue #116: #import (spec 2.14).`. Each test makes two fixture places, made absolute, under its `.build/tests/load/NAME/` (`first/` and `second/`), and passes them through `Load_Options.modules`:
    - `import_finds_a_module_in_the_first_place_that_has_it`:
      - `first/Alpha/module.rig`, and `second/` holding both `Alpha/module.rig` and `Beta/module.rig`;
      - `main.rig` is `#import "Alpha";\n#import "Beta";\nmain :: () {}\n`;
      - no messages; `d.files` is main, `FIRST/Alpha/module.rig`, `SECOND/Beta/module.rig`, the last two being exactly those absolute paths.
    - `import_loads_a_module_once`:
      - main imports Alpha twice, then `#load`s `first/Alpha/module.rig` by its relative path: 2 files, the module named by its absolute path;
      - with the `#load` first and the import after: 2 files, the module named by the relative path.
    - `import_module_loads_relative_to_itself`: Alpha's `module.rig` holds `#load "part.rig";`, which is `FIRST/Alpha/part.rig`, an absolute path.
    - `import_reports_names_and_missing_modules`:
      - `#import "Nope";` gives `1:9: there's no module 'Nope' (looked in FIRST and SECOND)`, the two exactly as `Module_Places` has them;
      - `#import "../x";`, `#import "";`, `#import "a/b";` and `#import "9lives";` each give `1:9: 'TEXT' isn't a module's name`;
      - each sets `lost_a_file`, and `main` isn't reported missing.
    - `import_is_case_sensitive`: `#import "alpha";` with `first/Alpha/` is the no-module message, on both hosts.
    - `module_places_are_beside_rig_and_above_it`:
      - `module_places` gives `path_absolute(exe_dir, "modules")` and `path_absolute(exe_dir, "../modules")`;
      - from `bin/unit-tests`, the second ends in `modules`, and its `Basic/module.rig` exists.
    - `import_finds_basic_with_rigs_own_places`: `load_program` with no options, on `#import "Basic";\nmain :: () {}\n`, gives no messages. Its second file is `module_lookup(places, "Basic")`.
  - `tests/run/basic_import_and_load.rig`:
    - Header: `#import` and `#load` of one module read it once, under the first name (spec 2.14).
    - `#import "Basic";`, then `#load "../../modules/Basic/module.rig";`.
    - `main` calls `print_line("read once")`.
    - `.out` is `read once\n`.
  - `tests/errors`, each with a header comment:
    - `import_unknown_module`: `#import "Nope";` and `main`. Its `.err` is the message with `(looked in @BIN_MODULES@ and @MODULES@)`.
    - `import_bad_name`: `#import "../modules/Basic";`.
    - `import_wrong_case`: `#import "basic";`, the no-module message.
    - `import_in_a_body`: `#import "Basic";` inside `main`'s body.
  - The Basic programs switch, and their `.err` lines become `@MODULES@/Basic/FILE.rig:L:C: runtime error: ...`, with the same lines and columns.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe import` and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement** as in the interfaces.
  - `load.h`'s rules comment, `ast.h`'s `Ast_File` comment and `parser.h` say what `#import` does.
  - Basic's header and README say `#import "Basic";`, and that a `#load` by path still works and reads the files once.
- [ ] **Step 4: Run all tests** on both hosts, including the interpreted pass of the Basic programs and of `basic_import_and_load`, and `check.js`.
- [ ] **Step 5: Commit** `Import the modules that ship with rig`.

### Task 3: `#compiler` procedures and the Compiler module's declarations

**Files:**
- Modify: `src/ast.h` (`Ast_Proc.is_compiler`, `compiler_pos`, and `body`'s comment), `src/ast.cpp` (` (compiler)` in the dump, after `(no_abc)`)
- Modify: `src/parser.h`, `src/parser.cpp` (`parse_procedure`'s directive loop and its grammar comment at `1340-1367`)
- Modify: `src/diag.h` (`Source_File.in_module`), `src/load.cpp` (sets it for every file added, the main one included)
- Modify: `src/checker.cpp` (`check_signature`), `src/check_expr.cpp` (`check_name`)
- Modify: `src/ir.h` (`Ir_Proc.is_compiler`), `src/ir.cpp` (`ir_print`)
- Modify: `src/lower.cpp`:
  - `Lowering.diag`'s comment;
  - `declare_proc`, `ir_lower`'s body loop, and `lower_call_results` (the refusal, and decision 8's position);
  - the header's calling-convention paragraph.
- Modify: `src/llvm_backend.cpp` (both loops at `1803-1833`, and `declare_foreign_functions` stays foreign-only)
- Modify: `src/interp.cpp` (`IR_PROC_ADDR` asserts its procedure isn't `is_compiler`)
- Modify: `src/driver.cpp` (`read_and_check`: diagnostics from lowering are printed, and fail the build), `tests/unit/interpret_mode.cpp` (the same)
- Create: `modules/Compiler/module.rig`, `modules/Compiler/README.md`
- Test: `tests/unit/test_parser.cpp`, `test_load.cpp`, `test_checker.cpp`, `test_ir.cpp`
- Create, each `.rig` and `.err`: `tests/errors/compiler_outside_module`, `compiler_as_value`, and `compiler_called_at_run_time` (with an empty `.check.err`)

**Interfaces:**
```cpp
// ast.h, Ast_Proc:
bool is_compiler;       // #compiler: no body; the compiler does it, while compiling (spec 7.9)
Position compiler_pos;  // where that #compiler is written
// diag.h, Source_File:
bool in_module;  // inside one of the modules' places (load.h, spec 2.14): a file of a module that ships
                 // with rig, which may declare #compiler procedures; set by the loader
// ir.h, Ir_Proc:
bool is_compiler;  // a #compiler procedure: declared, no blocks; the interpreter calls the compiler's
                   // C++ for it (interp.h), and the program's lowering refuses a call of it
```
- **Parser:** `#compiler` after a signature, then `;`.
  - It sets `is_compiler` and `compiler_pos`, and the body stays null.
  - After `#c_call` it is decision 4's message. After `#no_abc` it is `expected '{'`, as `#foreign` is.
  - `#compiler` followed by anything but `;` is `expected ';'`.
- **Checker:**
  - In `check_signature`, `c_side` stays false for a `#compiler` procedure.
  - A `#compiler` procedure in a file without `in_module` gets the outside-a-module error at `compiler_pos`.
  - `check_name` refuses it as a value, at the name, returning null.
- **Lowering:**
  - `declare_proc` sets `is_compiler`, reads no body and queues none.
  - `ir_lower` lowers no body for it.
  - `lower_call_results`, for a `proc->is_compiler` callee: it sets `l->pos` to `call->callee->pos` around `emit_call` (decision 8). When `!l->on_demand`, it also reports `'NAME' can only be called while compiling` there, and still emits the call, so the IR stays well formed.
  - `ir_print` writes `compiler proc @NAME(...)`.
- **Driver:** after `ir_lower`, when `d.error_count` grew, the diagnostics are sorted and printed, nothing is lowered further, and rig build exits 1 with no object written. Interpret mode does the same.
- **`modules/Compiler/module.rig`:**
  - A header in Basic's style: the Compiler module, what a build program is, `#import "Compiler";`, the procedures done by the compiler while compiling (spec 7.9), and how a program gets it.
  - Then the issue's declarations exactly: `Workspace`, `Build_Options` with its three commented fields, and the four `#compiler` procedures, each with a comment.
  - `README.md` lists them, as Basic's does.

- [ ] **Step 1: Write the failing tests.**
  - `test_parser.cpp`, `parse_compiler_procedures`:
    - `make :: (name: string) -> Workspace #compiler;\n` gives `(proc make (params (name string)) (returns Workspace) (compiler))\n`, with `is_compiler`, `compiler_pos` 1:37 and a null body.
    - `f :: () #c_call #compiler;\n` gives `1:9: a #compiler procedure is the compiler's: it takes no '#c_call'`.
    - `f :: () #compiler {}\n` gives `1:19: expected ';', found '{'`.
  - `test_load.cpp`, `files_inside_the_places_are_modules`: with fixture places, `main.rig` imports Alpha and `#load`s `first/Alpha/other.rig` by its relative path. `in_module` is clear for main, and set for both module files.
  - `test_checker.cpp`. A helper `check_module_source(a, text)` is `check_source` with `files[0].in_module` set first.
    - `compiler_procedures_only_in_modules`: `check_source` of `f :: () #compiler;\nmain :: () {}\n` gives `1:9: #compiler is only for the modules that ship with rig\n`.
    - `compiler_procedures_take_structs_and_strings`: `check_module_source` gives `""` for:
      ```
      Workspace :: struct { id: s64; }
      Options :: struct { name: string; on: bool; }
      make :: (name: string) -> Workspace #compiler;
      get :: (w: Workspace) -> Options #compiler;
      set :: (w: Workspace, o: Options) #compiler;
      main :: () {}
      ```
    - `compiler_procedures_arent_values`: the same declarations, with `M :: make;` and `main :: () { f := make; }`. It gives exactly one `'make' can't be a value: the compiler does it` at `M`'s value, and one at `f`'s.
    - `compiler_main_needs_a_body`: `check_module_source` of `main :: () #compiler;\n` gives `1:1: 'main' must have a body: it's where the program starts\n`.
  - `test_ir.cpp`, `lowering_refuses_compiler_calls`:
    - A helper `lower_messages(a, text)`: `src()`, then `in_module` set, parse, a clean check, `ir_lower`, `diag_sort`, `messages`.
    - Program:
      ```
      1 Workspace :: struct { id: s64; }
      2 create_workspace :: (name: string) -> Workspace #compiler;
      3 helper :: () { w := create_workspace("never called"); }
      4 main :: () { w := create_workspace("game"); }
      ```
    - It gives exactly `3:21: 'create_workspace' can only be called while compiling\n4:19: 'create_workspace' can only be called while compiling\n`.
    - The same declarations with only `main :: () {}` lower with no messages, and `ir_print` of the module holds `compiler proc @create_workspace(ptr, ptr)`. `declare_proc` declares every procedure, so the declaration is there without a call.
  - `tests/errors`:
    - `compiler_outside_module`: a program declaring `make :: () #compiler;`, with `main`.
    - `compiler_as_value`: `#import "Compiler";` and `main :: () { f := create_workspace; }`.
    - `compiler_called_at_run_time`: `#import "Compiler";` and a `main` that calls `create_workspace("game")`. Its `.check.err` is empty.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe compiler` and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement** as in the interfaces.
  - `parser.h`'s grammar comment and `checker.h`'s rules comment say what `#compiler` is.
  - `llvm_backend.h`'s header says `#compiler` procedures aren't in the module's functions.
- [ ] **Step 4: Run all tests** on both hosts, and `check.js`. `rig check modules/Compiler/module.rig` exits 1 with only the main error, until Task 5 makes it a build program.
- [ ] **Step 5: Commit** `Declare procedures the compiler implements with #compiler`.

### Task 4: The interpreter's compiler hook and the Compiler module's procedures

**Files:**
- Modify: `src/interp.h` (the typedef, `interp_set_compiler`, and the header's paragraph on calls), `src/interp.cpp` (`Interp`'s two fields, and the `IR_CALL` branch before the foreign one)
- Modify: `src/run.h`, `src/run.cpp` (`compile_time_set_compiler`, and `make_parts` handing the hook on)
- Create: `src/compiler_module.h`, `src/compiler_module.cpp` (in `all.cpp` after `run.cpp`)
- Modify: `src/driver.cpp` (`read_and_check` installs a `Build_State` with `is_build_program` false: Task 5 sets it), `tests/unit/interpret_mode.cpp` (the same)
- Test: `tests/unit/test_run.cpp`, and a new `tests/unit/test_compiler_module.cpp` (at the end of `unit.cpp`'s list)
- Create: `tests/errors/main_creates_workspace.rig`, `.err`, and `.check.err` (empty)

**Interfaces:**
```cpp
// interp.h:
// A #compiler procedure's call (spec 7.9, 10.8): the compiler does it in
// C++, in place of a body. args are the call's IR arguments, canonical: an
// aggregate's is its address in native memory, the hidden result pointers
// first. Scalar results go to results[0..]. `at` is the call's place (its
// name). Returns {} when it worked; otherwise the failure's message, in
// `a`, which fails the run with the caller's frames, as a runtime error.
typedef String Interp_Compiler_Call(void *context, Ir_Proc *proc, Array<u64> args, u64 *results, Position at,
                                    Arena *a);
// The hook for in's #compiler calls; without one, each fails: "'NAME' can
// only be called while compiling".
void interp_set_compiler(Interp *in, Interp_Compiler_Call *call, void *context);

// run.h: the hook the world's interpreter gets (interp_set_compiler); set
// before the first run.
void compile_time_set_compiler(Compile_Time *ct, Interp_Compiler_Call *call, void *context);

// compiler_module.h:
// A workspace a build program made (create_workspace): one program to
// build, as a compilation of its own (spec 7.9).
struct Build_Workspace {
    String name;
    Position made_at;       // the create_workspace call's name, in the build program's files
    Array<String> files;    // add_build_file's, resolved against the build program's directory
                            // (load_path), in the order added
    String output_name;     // Build_Options.output_name; empty: the first file's name
    bool optimize;          // false at first
    bool bounds_checks;     // true at first
};
// What one compilation's calls of the Compiler module work on.
struct Build_State {
    String program_path;    // the compilation's main file as rig names it: add_build_file's paths are
                            // relative to its directory
    bool is_build_program;  // ast_is_build_program; otherwise create_workspace fails
    Array<Build_Workspace> workspaces;  // in creation order; a Workspace's id is its index + 1
    Arena arena;            // names, paths and output names, copied out of the interpreter's memory
};
void build_state_free(Build_State *s);
// interp_set_compiler's hook for the Compiler module; context is a Build_State.
String compiler_module_call(void *context, Ir_Proc *proc, Array<u64> args, u64 *results, Position at, Arena *a);
```
- **The interpreter's branch.** For `IR_CALL` of an `is_compiler` callee, it gathers `v[args[k]]` and calls the hook. Then:
  - a non-empty message goes to `interp_fail(in, frame, block, index, a, result, message)`, and the loop returns;
  - otherwise the scalar results are written to `v[instr->result + k]`.
- **What each procedure does** (the layouts are in Facts 9; `Workspace` is an `s64` at 0, and `Build_Options` has `output_name` at 0, `optimize` at 16 and `bounds_checks` at 17):
  - `create_workspace(name)`:
    - When not `is_build_program`, it fails with the main message.
    - A name already used fails with `there's already a workspace 'NAME'`.
    - Otherwise it appends a workspace with a copy of the name, `made_at = at` and the defaults, and writes its id through `args[0]`.
  - `get_build_options(w)` writes `output_name` (its `data` pointing into the state's arena), `optimize` and `bounds_checks` through `args[0]`.
  - `set_build_options(w, options)` checks decision 11's output rule, then copies all three in.
  - `add_build_file(w, path)`:
    - a path not ending in `.rig` is decision 11's message;
    - otherwise `load_path(program_path, path)`, and a path that doesn't exist (`os_file_exists`) is `there's no file 'PATH'`, PATH as given;
    - otherwise the resolved path is appended.
  - Each procedure first checks `w.id` in `1..count`, else `there's no workspace with id N`.

- [ ] **Step 1: Write the failing tests.**
  - `test_run.cpp`, `compiler_procedures_without_a_compiler_fail`:
    - `check_module_source` with a world of its own and no hook:
      ```
      1 twice :: (n: int) -> int #compiler;
      2 N :: #run twice(1);
      3 main :: () {}
      ```
    - It gives `2:6: #run failed: runtime error: 'twice' can only be called while compiling\n    in #run (test.rig:2:11)\n`.
  - `test_compiler_module.cpp`:
    - A helper, `run_build_program(Arena *a, const char *name, const char *text, Build_State *state, const Module_Places *places = nullptr, Ast_File **tree = nullptr)`:
      - it writes `text` to `.build/tests/compiler/NAME/build.rig` (made fresh), sets `state->program_path` to that path, and loads it with `places` (null: rig's own);
      - it makes a world with `compile_time_set_compiler(world, compiler_module_call, state)`, checks, and runs the top-level runs when the check is clean;
      - it returns the sorted `messages`, with every occurrence of the path replaced by `PATH`, and the tree (in `a`) through `tree` when given.
      - A test writes any other files it needs into the directory first.
      - The state's `is_build_program` is the test's to set, so the helper doesn't need the program to be a build program. Until Task 5 skips the `main` rule for build programs, a program without `main` gets `a procedure called 'main' is needed`, and its top-level runs never run. So every program of these tests ends with a last line `main :: () {}` (after the lines shown, so no position below moves).
    - `compiler_procedures_make_workspaces`:
      - `is_build_program` is set, and `src/main.rig` and `src/more.rig` exist. Program:
        ```
        1 #import "Compiler";
        2 #run {
        3     w := create_workspace("game");
        4     o := get_build_options(w);
        5     o.output_name = "game";
        6     o.optimize = true;
        7     o.bounds_checks = false;
        8     set_build_options(w, o);
        9     add_build_file(w, "src/main.rig");
        10    add_build_file(w, "src/more.rig");
        11    create_workspace("tools");
        12 }
        ```
      - No messages.
      - Two workspaces. The first is `game`, with `made_at` 3:10, files `.build/tests/compiler/NAME/src/main.rig` and `.../src/more.rig` (with the host's `load_path` result), `output_name` `game`, `optimize` true and `bounds_checks` false.
      - The second is `tools`, with no files, `output_name` empty, `optimize` false and `bounds_checks` true.
    - `build_options_start_as_rig_build_has_them`:
      - Program: `#import "Compiler";`, then a procedure `defaults :: () -> int` returning `o.output_name.count * 100`, plus 10 if `o.optimize`, plus 1 if `o.bounds_checks`, of a new workspace's options, then `D :: #run defaults();`.
      - `D` is the constant 1 (read from the tree the helper gives back).
    - `build_options_keep_their_own_copy_of_the_name`:
      - Program: a top-level `#run { }` that fills a local `bytes : [4]u8` with `game` and views it as a string (`name.data = *bytes[0]; name.count = 4;`).
      - It creates workspace `name`, sets `output_name = name`, then overwrites `bytes[0] = 120`.
      - The workspace's name and its `output_name` are both `game`.
    - `compiler_procedures_fail_like_runtime_errors`. One program each, `is_build_program` set; the messages exactly:
      - `#import "Compiler";\n#run { w : Workspace; w.id = 7; add_build_file(w, "x.rig"); }\nmain :: () {}\n` gives `2:1: #run failed: runtime error: there's no workspace with id 7\n    in #run (PATH:2:33)\n`;
      - two `create_workspace("game");` statements in one `#run { }` give `there's already a workspace 'game'`, its frame at the second call's name;
      - `add_build_file(w, "src/missing.rig")` gives `there's no file 'src/missing.rig'`;
      - `add_build_file(w, "notes.txt")` gives `'notes.txt' isn't a .rig file`;
      - an `output_name` of `game.rig` gives `the output 'game.rig' ends in .rig; is it the source file?`;
      - a failure inside a helper `make :: () { create_workspace("a"); create_workspace("a"); }` has two frames, `in make (PATH:L:C)` at the second call's name, then `in #run (...)`.
    - `create_workspace_needs_a_build_program`: `is_build_program` clear, and `#run { create_workspace("x"); }` gives `#run failed: runtime error: a program with 'main' can't create workspaces`.
    - `compiler_procedures_unknown_to_the_compiler_fail`:
      - a fixture place holds `Fake/module.rig` with `nothing :: () #compiler;`;
      - a program `#import "Fake"; #run nothing(); main :: () {}`, loaded with those places, gives `the compiler has no procedure 'nothing'`. (Those places hold no Compiler module, so it is never a build program: without `main` it would get the `main` error, and its run wouldn't run, after Task 5 too.)
  - `tests/errors/main_creates_workspace`:
    - Header: a program with `main` can't make workspaces, even while compiling (spec 7.9).
    - `#import "Compiler";`, `#run { create_workspace("tools"); }` and `main :: () {}`.
    - Its `.err` is the `#run failed: runtime error: a program with 'main' can't create workspaces` line, with the source line and caret, then `    in #run (tests/errors/main_creates_workspace.rig:L:C)`, at the call's name.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe compiler` and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement** as in the interfaces.
  - `interp.h`'s header says how a `#compiler` procedure is called: in C++, not through the thunks, its struct parameters and results in the interpreter's memory.
  - `compiler_module.h`'s header says what the module's procedures do and what they leave behind.
- [ ] **Step 4: Run all tests** on both hosts, and `check.js`.
- [ ] **Step 5: Commit** `Do the Compiler module's procedures in the compiler`.

### Task 5: Build programs: recognised, refused options, and the first failures

**Files:**
- Modify: `src/ast.h` (`Ast_File.reaches_compiler`, `compiler_pos`, `ast_is_build_program`), `src/ast.cpp`
- Modify: `src/load.cpp` (sets them, decision 7), `src/load.h` (the rules comment)
- Modify: `src/checker.cpp` (the `main` rule skips a build program), `src/checker.h` (its comment)
- Modify: `src/driver.cpp`:
  - `read_and_check` becomes the `Compilation` steps, which `command_check` uses too;
  - `build()` runs a build program;
  - `command_build`'s no-file rule;
  - the header comment.
- Modify: `src/main.cpp` (the usage), `tests/unit/interpret_mode.cpp` (it calls the `Compilation` steps, and refuses a build program)
- Modify: `tests/runner.cpp` (`error_test` and decision 18, `COMMAND_CASES`, `CHECK_CASES`' usage start, the interpret-mode fixed case, the header)
- Modify: `CONTRIBUTING.md` (the tests list: build programs in `tests/errors`)
- Test: `tests/unit/test_load.cpp`
- Create, each `.rig` and `.err`:
  - `tests/errors/build_no_workspace` and `build_empty_workspace`, each with an empty `.check.err`;
  - `build_no_such_file` and `build_two_workspaces_one_name`, each with an empty `.check.err`;
  - `build_type_error`, with no `.check.err`.

**Interfaces:**
```cpp
// ast.h, Ast_File:
bool reaches_compiler;  // the loader added the Compiler module's module.rig (by #import or #load)
Position compiler_pos;  // the #import or #load that first named it; the main file's 1:1 when it is that file
// Whether the program is a build program (spec 7.9): it reaches the Compiler
// module and has no procedure called main, a broken one included.
bool ast_is_build_program(Ast_File *file);

// driver.cpp:
// One compilation, from its files to its diagnostics: rig build's (an
// ordinary program's, a build program's, or one of its workspaces'), rig
// check's, and interpret mode's. Its steps, in order, each false when
// there are errors so far (said, or to be printed by compilation_print).
struct Compilation {
    Arena *arena;
    Diagnostics d;
    Ast_File *tree;
    Build_State build;    // its calls of the Compiler module (compiler_module_call)
    Compile_Time *world;  // from compilation_check until compilation_end_runs
};
// Reads the main file (or stdin), and stdin_for's text, and loads: as
// read_and_check did, its messages included. False when nothing could be
// loaded (said).
static bool compilation_load(Compilation *c, String path, bool from_stdin, String stdin_for);
// diag_print_at_exit(&d), the world with compiler_module_call on c->build
// (is_build_program from the tree), check().
static bool compilation_check(Compilation *c);
static bool compilation_run(Compilation *c);       // the top-level #runs
static void compilation_end_runs(Compilation *c);  // diag_print_at_exit(nullptr), the world freed
// ir_lower; null when it reported errors (Task 3's refusal).
static Ir_Module *compilation_lower(Compilation *c, bool bounds_checks);
// Sorts and prints the diagnostics, then empties the list (error_count stays).
static void compilation_print(Compilation *c);
static void compilation_free(Compilation *c);
```
- **`build(a, path, output, bounds_checks)`**, `output` being `{}` when there was no `-o`:
  1. Load, then check. Errors are printed, and the status is 1.
  2. For a build program: with `-o`, its message, status 2. Then with `--no-bounds-checks`, decision 12's message, status 2. In both cases nothing runs and nothing more is printed.
  3. Run the top-level runs. Errors are printed, and the status is 1.
  4. An ordinary program is lowered, emitted and linked as today.
  5. A build program with no workspace gets `the build program created no workspace` at `compiler_pos`, printed, status 1.
  6. Workspaces with no files get decision 15's errors.
  7. Until Task 6, workspaces with files aren't built.
- **`command_build`:** with no file, it uses `build.rig` when `os_file_exists("build.rig")`. Otherwise it prints `rig: error: no file given and no build.rig in the current directory` and returns 1.
- **Usage:** `usage: rig build [FILE.rig] [-o OUTPUT] [--no-bounds-checks]`, the rest as today.
- **rig check** uses `compilation_load`, `compilation_check` and `compilation_end_runs`, and prints as today.

- [ ] **Step 1: Write the failing tests.**
  - `test_load.cpp`, `the_compiler_module_makes_a_build_program`. A fixture place holds `Compiler/module.rig` (a comment line alone). For each case, `reaches_compiler`, `compiler_pos` and `ast_is_build_program`:
    - `#import "Compiler";\n`: set, 1:1, true;
    - `#import "Compiler";\nmain :: () {}\n`: set, 1:1, false;
    - `#import "Compiler";\nmain := 1;\n`: set, 1:1, true;
    - `main :: () {\n` (broken) with the import first: false;
    - `#load "helper.rig";\n`, whose `helper.rig` is `#import "Compiler";\n`: set, at `helper.rig`'s 1:1 (file 1), true;
    - `#load "first/Compiler/module.rig";\n`, by its relative path: set, at 1:1 of the main file, true;
    - `#import "Basic";\n`, with a `Basic/module.rig` fixture: clear, false.
  - `test_load.cpp`, `build_programs_need_no_main`: the first case above, loaded and then checked (`check`), has no messages. The same program with `x := 1;` and no import still gets `a procedure called 'main' is needed`.
  - `tests/errors`, each with a header comment naming spec 7.9:
    - `build_no_workspace.rig`: `#import "Compiler";` and a top-level `#run { }`. The `.err` is `tests/errors/build_no_workspace.rig:L:1: error: the build program created no workspace`, at the `#import`.
    - `build_empty_workspace.rig`: `#run { create_workspace("game"); }`. The `.err` is `workspace 'game' has no files`, at the call's name.
    - `build_no_such_file.rig`: `add_build_file(w, "src/missing.rig")`. The `.err` is `#run failed: runtime error: there's no file 'src/missing.rig'` and its frame.
    - `build_two_workspaces_one_name.rig`: `#run failed: runtime error: there's already a workspace 'game'` and its frame.
    - `build_type_error.rig`: a build program with `x : int = "text";`. The `.err` is the checker's message, which rig check prints too.
  - `COMMAND_CASES`:
    - the two other usage starts (`no command`, `an unknown command`) become `usage: rig build [FILE.rig] [-o OUTPUT] [--no-bounds-checks]\n`, and so does the first line of `CHECK_CASES`' `rig check with no file` (`runner.cpp:1463-1466`), whose start is both lines of the usage;
    - `rig build with no file` becomes `{"build"}, 1, "rig: error: no file given and no build.rig in the current directory\n"`;
    - `--no-bounds-checks with no file` becomes status 1 with the same message;
    - new: `-o with a build program`, `{"build", "tests/errors/build_no_workspace.rig", "-o", ".build/tests/output"}`, 2, with the `-o` message whole;
    - new: `--no-bounds-checks with a build program`, `{"build", "tests/errors/build_no_workspace.rig", "--no-bounds-checks"}`, 2, with decision 12's message whole;
    - new: `-o with a build program that has errors`, `{"build", "tests/errors/build_type_error.rig", "-o", ".build/tests/output"}`, 1, starting `tests/errors/build_type_error.rig:`.
  - A runner fixed case, `interpret mode refuses a build program`: `bin/unit-tests --interpret tests/errors/build_no_workspace.rig` exits 1, with stderr exactly `unit-tests: error: a build program can't be interpreted\n` and nothing on stdout.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe build_program` and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement** as in the interfaces.
  - `error_test` leaves `-o` out for `build_*` programs (decision 18). The runner's header and CONTRIBUTING say so.
  - `driver.cpp`'s header comment describes build programs and the `Compilation` steps.
  - `interpret_mode.cpp`'s comment no longer says it copies `read_and_check`.
- [ ] **Step 4: Run all tests** on both hosts, and `check.js`. `rig check modules/Compiler/module.rig` now exits 0.
- [ ] **Step 5: Commit** `Run a build program, and refuse what its build options decide`.

### Task 6: Building the workspaces, and the runner's `tests/build`

**Files:**
- Modify: `src/load.h`, `src/load.cpp` (`Load_Options.more_files` and `more_texts`)
- Modify: `src/driver.cpp`:
  - `compilation_load` takes the further files;
  - a new `build_workspace`;
  - `build()`'s workspace phase (decision 15);
  - the header.
- Modify: `tests/runner.cpp`:
  - `copy_tree`, `remove_tree`, `build_directory_test` and its pass in `main`;
  - `build_program_elsewhere_test`;
  - the header.
- Modify: `editors/vscode/test/check.js` (the grammar test walks `tests/build` too), `CONTRIBUTING.md` (the tests list: `tests/build`)
- Test: `tests/unit/test_load.cpp`
- Create: `tests/build/two_workspaces/`, `tests/build/load_across_files/`, `tests/build/options/`, `tests/build/workspace_errors/`, `tests/build/exit_fails/`
- Create: `tests/errors/build_nested.rig`, `.err` and `.check.err` (empty), and `tests/errors/load/build_nested_inner.rig`

**Interfaces:**
```cpp
// load.h, Load_Options:
Array<String> more_files;  // a workspace's further files (spec 7.9), loaded right after the main file,
                           // before its #loads, in this order, as if it #loaded each first; their paths
                           // as given
Array<String> more_texts;  // their texts, read by the caller, one for each

// driver.cpp: Task 5's compilation_load, with a workspace's further files,
// each read as the main file is ("rig: error: cannot read ...": false).
static bool compilation_load(Compilation *c, String path, bool from_stdin, String stdin_for,
                             Array<String> more_files = {});
// Workspace w of the build program in `build` (its diagnostics, for the
// nested-build-program error), compiled and linked as a compilation of its
// own (decisions 13 and 14), in an arena of its own; true when it built.
static bool build_workspace(Compilation *build, Build_Workspace *w);

// runner.cpp:
static bool copy_tree(Arena *a, String from, String to);  // to made; files copied at any depth
static void remove_tree(Arena *a, String dir);            // dir and everything in it, if it's there
static String build_directory_test(Arena *a, String rig, String name);
static String build_program_elsewhere_test(Arena *a, String rig);
```
- **`build_workspace`:**
  1. Read the files and load them.
  2. When the tree is a build program, add the nested error to `build->d` at `w->made_at`, print it with `compilation_print(build)`, and fail.
  3. Check, run, lower with `w->bounds_checks`, and emit with `w->optimize` to decision 14's object.
  4. Link to decision 14's executable. The `rig: error:` messages are as today's.
  - Each compilation's own diagnostics are printed at its end.
- **Paths in messages** are as the workspace's files are named: `src/broken.rig:2:14: error: ...` when the build program is `build.rig` in the current directory.

- [ ] **Step 1: Write the failing tests.**
  - `test_load.cpp`, `more_files_load_after_the_main_file`:
    - `main.rig` `#load`s `c.rig`. The further files are `a.rig` and `b.rig`, and `b.rig` names `main.rig` again.
    - The files are, in order: `main.rig`, `a.rig`, `b.rig`, `c.rig`. No messages. `main.rig` is read once.
  - `tests/build/two_workspaces/` (Review Focus 1):
    - `build.rig`:
      - workspace `greeter`, `output_name` `hello`, file `greeter.rig`;
      - then workspace `counter`, file `src/counter.rig`, with the default name.
    - Both programs declare `label := "..."`, a top-level `#run puts("compiling the greeter");` (or `the counter`), and a `main` that prints `hello from the greeter` (or `1 2 3`) with `printf`, using `label`.
    - `expected.build.out`: `compiling the greeter\ncompiling the counter\n`.
    - `expected.programs`: `hello`, `counter`.
    - `hello.out` and `counter.out`.
  - `tests/build/load_across_files/`:
    - `build.rig`: workspace `game`, `output_name` `game`, files `src/main.rig` and `src/extra.rig`.
    - `src/main.rig` `#load`s `player.rig` (`src/player.rig`). Its `main` calls `player_say()` and `extra_say()`, each printing a line.
    - `expected.programs`: `game`. `game.out` holds the two lines.
  - `tests/build/options/`:
    - `build.rig` makes three workspaces:
      - `plain`, from `sums.rig`, `output_name` `plain`;
      - `fast`, from `sums.rig`, `output_name` `fast`, `optimize` true;
      - `unchecked`, from `past_the_end.rig`, `bounds_checks` false, with the default name.
    - `sums.rig` computes and prints, with `printf`: loops over an array, a struct passed and returned, a recursive procedure, a float sum printed `%.3f`, and integer wrap-around. `plain.out` and `fast.out` are the same text.
    - `past_the_end.rig` is `tests/run/no_bounds_checks.rig`'s program, so `past_the_end.out` is `42\n`.
    - `expected.programs`: `plain`, `fast`, `past_the_end`.
  - `tests/build/workspace_errors/`:
    - workspaces `broken` (`src/broken.rig`, whose `main` calls `putz("hi");`) and `fine` (`fine.rig`, printing `fine`);
    - `expected.exit`: `1`;
    - `expected.err`: `src/broken.rig:L:C: error: unknown identifier 'putz'`, with its source line and caret;
    - `expected.programs`: `fine`. `fine.out` is `fine\n`.
  - `tests/build/exit_fails/`:
    - `build.rig` declares `exit` and `puts`, and has a top-level `#run { }` that makes workspace `never` with `main.rig`, prints `the assets are missing` with `puts`, and calls `exit(1)`;
    - `main.rig` is a hello program;
    - `expected.exit`: `1`. `expected.build.out`: `the assets are missing\n`. There is no `expected.programs`, so no `.build` and no `never` may appear.
  - `tests/errors/build_nested.rig`:
    - Its workspace `inner` adds `load/build_nested_inner.rig`, a build program (`#import "Compiler";` and `#run { }`).
    - `.err`: `workspace 'inner' is a build program; a workspace builds an ordinary program`, at the call's name.
  - The runner's fixed case `rig build sub/build.rig from its parent` (Review Focus 2):
    - it copies `tests/build/load_across_files` to `.build/tests/elsewhere/sub/` and runs `RIG build sub/build.rig` in `.build/tests/elsewhere`;
    - it expects exit 0, and that `sub/game[.exe]` exists and prints `game.out`;
    - `sub/.build/main.obj|.o` exists, and neither `game[.exe]` nor `.build` exists in `.build/tests/elsewhere`.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe more_files` and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement** as in the interfaces.
  - The runner's header and CONTRIBUTING describe `tests/build`:
    - decision 10's files;
    - the copy;
    - `rig build` with no file;
    - the programs' runs;
    - that none of it is interpreted.
- [ ] **Step 4: Run all tests** on both hosts, debug and release, and `check.js`.
- [ ] **Step 5: Commit** `Build each workspace as a compilation of its own`.

### Task 7: Packaging, the installers and CI

**Files:**
- Modify: `scripts/package_linux.sh`, `scripts/package_windows.bat`:
  - the header comments;
  - `modules/` copied beside rig and packed;
  - the try-out, below.
- Modify: `site/public/install.sh` (step 3), `site/public/install.ps1` (step 3), both per decision 20
- Modify: `scripts/test_install.sh` (the stand-in release holds `modules/Basic/module.rig`, and its upgrade check)
- Modify: `CONTRIBUTING.md` ("Releasing": the archives' contents gain `modules/`, and the release checklist checks an installed `modules/Basic/module.rig`), `site/install.md` (`30`, `81`, and the manual steps: `modules` stays beside rig)
- Modify: `.github/workflows/ci.yml` only if a step needs to change. The tests find `modules/` from `bin/`, so none should.

- **The try-out** (decision 1), after today's hello:
  1. Copy `tests/build/load_across_files` into the stage's `check/try/`. Run the packed rig as `rig build try/build.rig`, then `try/game`, and compare its output with `tests/build/load_across_files/game.out`.
  2. Build `$root/tests/run/basic_string.rig`, which `#import`s Basic and so finds the archive's `modules/`, with `-o basic`. Run it and compare with `basic_string.out`.
  - Each failure says which part failed, as the hello part does.
- **The installers:** both replace the old `modules/` whole on an upgrade. Each script's comment says why: a file a module no longer has mustn't stay behind, and `mv` and `Copy-Item` can't replace a directory.

- [ ] **Step 1: Write the failing test.** `test_install.sh`'s stand-in release packs `modules/Basic/module.rig`. After installing it twice (the second time over a `modules/Basic/stale.rig` added by hand), `$HOME/.local/share/rig/modules/Basic/module.rig` exists and `stale.rig` doesn't.
- [ ] **Step 2: Run** `scripts/test_install.sh` in WSL. The new check fails on today's `mv`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run, and see each pass:**
  - `cmd /c scripts\package_windows.bat` on Windows;
  - `scripts/package_linux.sh` in WSL;
  - `scripts/test_install.sh` and `shellcheck site/public/install.sh scripts/test_install.sh` in WSL;
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test_install.ps1` on Windows. It installs published releases, which have no `modules/`, so it checks that nothing else changed. The commit body says the Windows upgrade path is tested once 0.3.0 is published.
  - All tests on both hosts.
- [ ] **Step 5: Commit** `Ship modules/ in the release archives`.

### Task 8: The specification, README and CONTRIBUTING

**Files:**
- Modify: `docs/rig-spec.md`
- Modify: `README.md` (Features; an example `build.rig` after the quick start; the usage line)
- Modify: `CONTRIBUTING.md` ("Using rig": the usage, `rig build` with no file, and the same example)
- Modify: `src/main.cpp` and the code comments, if any section number they cite moved

- [ ] **Step 1: Write the examples first.** Each is run by `bin/run-tests`.
  - 2.14: a `rig` program that does `#import "Basic";` and prints with `print_line`. Its output block is what it prints.
  - 7.9: the issue's `build.rig` as a `rig fragment`. The runner adds `main`, so `rig check` checks it as an ordinary program, which runs no top-level `#run`.
  - The Compiler module's declarations are a ```` ```text ```` block, since `#compiler` is refused outside a module's file.
  - Run `.\bin\run-tests.exe`. The examples pass.
- [ ] **Step 2: Write 2.14, Modules and `#import`:**
  - the declaration, and that it is a top-level one;
  - the two places, in order, and what each is for;
  - names, and their case;
  - the messages;
  - an imported file named by its absolute path, and loaded once with `#load`'s identity;
  - declarations joining the program's one scope, for now (milestone 6 brings module scopes);
  - the modules that ship (Basic, Compiler);
  - what makes a file a module's (`#compiler`, 7.9).
- [ ] **Step 3: Write 7.9, Build programs and the Compiler module:**
  - what a build program is, known before anything runs;
  - the module's declarations, and `Build_Options`' fields and defaults;
  - `#compiler`: only in a module's file, never a value, refused by lowering at run time;
  - what rig build does with a build program: its runs, then each workspace, a separate compilation in creation order;
  - several files in a workspace;
  - paths relative to the build program's directory: files, outputs, `.build`, and `#load` inside a workspace's file;
  - every failure and its message, the runtime ones with frames;
  - `exit(n)`;
  - `rig check` of a build program;
  - the `rig fragment` example.
- [ ] **Step 4: Update the other sections:**
  - 2.7: seven directives. `#import` is top-level, like `#load` (its misplaced message), and `#compiler` comes after a signature.
  - 2.13: a file holds `#import`s; `ImportDecl`; the load order with imports (one queue); `main` is needed unless the program is a build program (7.9).
  - 4.6: `#import`s declare no name.
  - 7.1: the `ProcDecl` EBNF gains `| "#compiler" ";"`, with a sentence and a reference to 7.9.
  - 7.4: the build-program exception.
  - 7.8: a build program's top-level runs come before its workspaces (7.9).
  - 9.2: an `#import` starting a line is where the recovery resumes.
  - 9.3: rows for every new message, each with its section.
  - 10.1:
    - the usage `rig build [FILE.rig] ...`;
    - no file means `build.rig`, or the new message;
    - build programs;
    - the refusals (status 2), after errors (status 1);
    - the exit status paragraph: no file is no longer status 2 for build.
  - 10.2:
    - stage 3 (the loader) imports;
    - stage 6: unoptimised by default, `optimize` running `default<O2>` at the optimising codegen level, debug information still made;
    - with `optimize`, every stack slot is zeroed before the procedure's body runs, so a `---` local read before it's assigned is still an unspecified value (8.6), zero, not a read the optimiser takes as undefined;
    - a build program isn't lowered.
  - 10.6: `modules/` beside rig in an archive, as the linker is.
  - 10.7: the guarantees about variables and stack slots (every local, parameter and iterator shown with its value; a parameter in its own slot, as clang's -O0 code has it, so a value a debugger writes is the one the program goes on with) hold for unoptimised code: `rig build` of a file, or a workspace without `optimize`. With `optimize`, the debug information is still written, and marked optimised, but a debugger may show a variable as optimised out, and writing to one may have no effect.
  - 10.8: `#compiler` calls are C++ in the compiler, not through the thunks, their arguments in the interpreter's memory, and their failures runtime errors with frames.
  - Appendix A: A.2's `TopLevel` gains `ImportDecl`; `ImportDecl = "#import" string_lit ";" .`; `ProcDecl` as 7.1; the top-level-only rule and the recovery rule name `#import`.
  - Appendix B: `| #import | a module that ships with rig | 2.14 |` and `| #compiler | a procedure the compiler does | 7.9 |`.
- [ ] **Step 5: README and CONTRIBUTING:**
  - README's Features gains "Modules" (`#import "Basic";`) and "Build programs" (a sentence, and the example `build.rig` with how to run it: `rig build` with no file);
  - the usage lines in both;
  - CONTRIBUTING's "Using rig" gets the same example and the build-program bullets.
- [ ] **Step 6: Run all tests** on both hosts. Every spec example passes.
- [ ] **Step 7: Commit** `Specify modules, #import and build programs`. Then send the independent review, fix its findings as the work flow says, and mark the PR ready.

## Not in this plan

- **Modules' own scopes**, and `#import` of a program's own or fetched modules (milestone 6).
- **More build options:** an output directory, defines, and enums for the options (milestone 5).
- **A command-line flag for optimisation.** Optimised builds come from a build file.
- **Interpreting a build program** (interpret mode refuses it), and `tests/build` in the interpreted pass.
- **A Windows stand-in release in `test_install.ps1`**, to test the installer's `modules/` upgrade before a release is published.
- **The milestone's close:** closing milestone 3, the Release workflow, and the roadmap are the maintainer's steps after the merge.
