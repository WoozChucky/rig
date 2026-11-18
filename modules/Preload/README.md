# Preload

Preload holds what the Rig language itself refers to, written in Rig, as Jai's Preload does (issue #151). Every program imports it without asking: an ordinary program, a build program and each of its workspaces, `rig check` (with `--stdin` and `--stdin-for` too), a program the test mode runs interpreted (`bin/unit-tests --interpret`), and the compile-time world where `#run` runs (spec 2.14, 7.8, 10.8).

## How every program gets it

rig finds `Preload/module.rig` where it finds every module that ships with it (spec 2.14): in the `modules` directory beside it, or in the one above it, the repository's. It reads the file right after the program's main file, and places it first in load order, before the main file (spec 2.13), so Preload's declarations come before the program's.

Writing the import is allowed, and reads nothing more, just as a `#load` of `module.rig` by its path reads nothing more: a file is read once.

```rig
#import "Preload";
```

## Its names

What Preload declares joins the program's one top level (spec 2.14, 4.6), so a program can't declare a name Preload declares. Preload comes first in load order, so the error is at the program's declaration, naming Preload's: `'NAME' is already declared at .../modules/Preload/module.rig:L:C`.

Every name Preload declares for its own use starts with two underscores (`__malloc`, say), which leaves the plain names to programs. Only the names this README lists are for programs. The underscores are a convention: the compiler doesn't enforce it.

## What it declares

### The allocator

An allocator is passed to whatever needs memory, as Jai has it: an `Allocator` holds a procedure and the data it works on, and the procedure is called with a mode, `ALLOCATE`, `RESIZE`, `FREE` or `FREE_ALL`. Preload holds the shape, the modes and the heap, so every program has them; Basic (`modules/Basic`) adds the helpers `alloc`, `resize`, `free` and `free_all`, an arena and a pool, all over these.

- `ALLOCATE`, `RESIZE`, `FREE`, `FREE_ALL`: the modes, 0 to 3.
- `Allocator`: `proc: (mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void` and `data: *void`. The procedure is called as `proc(mode, size, old_size, old_memory, data)`: `ALLOCATE` gives `size` bytes, or null; `RESIZE` gives `old_memory`, which holds `old_size` bytes, made `size` bytes long, moved or not, its first bytes kept, or null with `old_memory` as it was; `FREE` gives `old_memory` back and `FREE_ALL` everything the allocator gave, each returning null.
- `heap_allocator_proc`, `HEAP_ALLOCATOR`: C's heap, through Preload's own `__malloc`, `__realloc` and `__free`. Its memory is aligned for any type, as C's `malloc` aligns it. `FREE_ALL` does nothing and gives null: the heap doesn't know what it gave, so each piece goes back with `FREE`.

Calling an allocator's procedure directly asks it for exactly what it's given: the heap's passes a size of 0, or a null to resize or free, on to C, whose answer is C's business (what `malloc` and `realloc` give for 0 is left to each C library). Basic's `alloc`, `resize`, `free` and `free_all` settle those edges, so an allocator is only ever asked for more than 0 bytes and only ever given memory that isn't null to resize or free.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    a := HEAP_ALLOCATOR;
    p := a.proc(ALLOCATE, 64, 0, null, a.data);
    if p != null { puts("64 bytes from the heap"); }
    a.proc(FREE, 0, 0, p, a.data);
}
```

`__malloc`, `__realloc` and `__free` are C's `malloc`, `realloc` and `free` (`#foreign libc "NAME"`, spec 7.6) under Preload's own names, not for programs: they leave C's names to the program, which may declare C's `malloc`, `realloc` or `free` itself, the same C functions.

## Waiting on the language

- No type aliases, so the allocator's procedure type is written out, in `Allocator` and in each allocator procedure.
- No enums, so the allocator's modes are constants.
