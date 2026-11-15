# Compiler

Compiler is what a build program uses to build other programs (spec 7.9, issue #116). A build program is an ordinary Rig program that imports Compiler and declares no procedure called `main`, written in place of a build script. `rig build` runs its top-level `#run`s, which make workspaces, and then builds each workspace, in the order they were made, as a program of its own, with its own globals and its own `#run`s. A build program is never an executable itself.

## Importing it

Compiler ships with rig, so a program imports it by its name (spec 2.14). By default a build program is `build.rig`, which `rig build` with no file runs:

```rig
#import "Compiler";

build :: () {
    w := create_workspace("game");
    o := get_build_options(w);
    o.output_name = "game";
    o.optimize    = true;
    set_build_options(w, o);
    add_build_file(w, "src/main.rig");
}

#run build();
```

An imported file's declarations join the program's one top level (spec 2.13), so every name listed below is the program's too.

The paths a build program gives, its files and its output's name, are relative to the build program's own directory, as the `.build` directory it builds in is, not to the directory rig runs in.

## `#compiler` procedures

Compiler's procedures are declared with `#compiler` after the signature and no body: the compiler does them itself, while compiling, so a build program calls them from code its `#run`s reach. Only the modules that ship with rig may declare one.

- A call that a program's executable would make is an error, `'create_workspace' can only be called while compiling`, at the procedure's name.
- None of them can be a value: `'create_workspace' can't be a value: the compiler does it`.
- A program with `main` that creates a workspace fails there, as a runtime error does: `a program with 'main' can't create workspaces`. A program builds others, or is built, never both.

## Its declarations

- `Workspace`: `id: s64`, one program to build. The id is the compiler's; a zeroed `Workspace` names none.
- `Build_Options`: how a workspace's program is built.
  - `output_name: string`: the executable's name; `""` means the first file's name without `.rig`, as `rig build` does.
  - `optimize: bool`: `false`, unoptimised, as `rig build` of a file is; `true`, LLVM's `-O2` pipeline.
  - `bounds_checks: bool`: `true` by default.
- `create_workspace(name: string) -> Workspace`: a new workspace named `name`, with no files and the default options. Two workspaces can't share a name.
- `get_build_options(w: Workspace) -> Build_Options`: the options workspace `w`'s program will be built with.
- `set_build_options(w: Workspace, options: Build_Options)`: builds workspace `w`'s program with `options`. The output's name can't end in `.rig`, since that names a source file.
- `add_build_file(w: Workspace, path: string)`: adds the `.rig` file at `path` to workspace `w`'s program, which must exist when it's added. The first file added is the program's main file, and the others are loaded right after it, in the order they were added, before the files it `#load`s.
