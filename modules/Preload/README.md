# Preload

Preload holds what the Rig language itself refers to, written in Rig, as Jai's Preload does (issue #151). Every program imports it without asking: an ordinary program, a build program and each of its workspaces, `rig check` (with `--stdin` and `--stdin-for` too), and the compile-time world where `#run` runs (spec 2.14, 7.8).

## How every program gets it

rig finds `Preload/module.rig` where it finds every module that ships with it (spec 2.14): in the `modules` directory beside it, or in the one above it, the repository's. It reads the file right after the program's main file, and places it first in load order, before the main file (spec 2.13), so Preload's declarations come before the program's.

Writing the import is allowed, and reads nothing more, as a `#load` of `module.rig` by its path doesn't: a file is read once.

```rig
#import "Preload";
```

## Its names

What Preload declares joins the program's one top level (spec 2.13, 4.6), so a program can't declare a name Preload declares. Preload comes first in load order, so the error is at the program's declaration, naming Preload's: `'NAME' is already declared at .../modules/Preload/module.rig:L:C`.

Every name Preload declares for its own use starts with two underscores (`__malloc`, say), which leaves the plain names to programs. Only the names this README lists are for programs, none for now. The underscores are a convention: the compiler doesn't enforce it.
