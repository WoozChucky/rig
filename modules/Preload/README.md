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

### Type information and Any

`type_info(T)` (spec 5.19) gives a `*Type_Info`, the address of a description of type `T` laid out as the structs below: Jai's type information, trimmed to Rig's types (issue #153). Every value converts implicitly to an `Any` (spec 3.16), which pairs the value's address with its type's description, so code can take values of any type and find out what each is.

There is one description per type, so `a.type == type_info(int)` tells types apart by address. `int` and `s64` are two types with two descriptions, the same in all but their addresses; `float` is `float32`. The compiler writes the descriptions in the program's read-only data, only for the types the program uses and those they reach, and in the interpreter's memory for `#run`. Writing one is a memory error no check catches (spec 8.6).

A `Type_Info`'s `tag` is one of these tags, which are numbered as Jai's are:

- `TYPE_INFO_INTEGER` 0, `TYPE_INFO_FLOAT` 1, `TYPE_INFO_BOOL` 2, `TYPE_INFO_STRING` 3, `TYPE_INFO_POINTER` 4, `TYPE_INFO_PROCEDURE` 5, `TYPE_INFO_VOID` 6, `TYPE_INFO_STRUCT` 7, `TYPE_INFO_ARRAY` 8.

Every description starts with a `Type_Info`. Each kind's struct holds it as its first field, `info`, so a `*Type_Info` whose tag says the kind is cast to the kind's struct. Floats, `bool`, `string` and `void` have the plain `Type_Info`.

- `Type_Info`:
  - `tag: int`: the kind, one of the tags.
  - `size: int`, `alignment: int`: the type's size and alignment in bytes (spec 3.11); `void`'s are 0 and 1. A float's size says which float it is.
- `Type_Info_Integer` (`TYPE_INFO_INTEGER`, for `s8` to `u64` and `int`):
  - `info: Type_Info`.
  - `signed: bool`: whether the integer type is signed.
- `Type_Info_Pointer` (`TYPE_INFO_POINTER`):
  - `info: Type_Info`.
  - `pointer_to: *Type_Info`: what it points at; `void`'s description for a `*void`.
- `Type_Info_Procedure` (`TYPE_INFO_PROCEDURE`):
  - `info: Type_Info`.
  - `parameter_types: []*Type_Info`, `result_types: []*Type_Info`: the parameters' types and the results', in order.
  - `is_c_call: bool`: whether it is called as C calls (`#c_call`).
  - `is_variadic: bool`: whether its last parameter is a `..T`, whose type is then described as `[]T`.
- `Type_Info_Struct` (`TYPE_INFO_STRUCT`):
  - `info: Type_Info`.
  - `name: string`: the struct's declared name.
  - `members: []Type_Info_Struct_Member`: its fields, in order; none for a struct with none. A struct that holds a pointer to itself points back at its own description.
- `Type_Info_Struct_Member`, one field of a struct:
  - `name: string`: the field's name.
  - `type: *Type_Info`: its type's description.
  - `offset: int`: where it is, in bytes from the struct's start (spec 3.9).
- `Type_Info_Array` (`TYPE_INFO_ARRAY`, for an array `[N]T` and a view `[]T` alike):
  - `info: Type_Info`.
  - `element_type: *Type_Info`: `T`'s description.
  - `array_kind: int`: `ARRAY_FIXED` (0) for an array, `ARRAY_VIEW` (1) for a view.
  - `count: int`: the array's `N`, or -1 for a view. A string isn't described as a view: it has its own tag.

`Any` is an ordinary struct, so it has a description of its own, a struct named `Any`:

- `type: *Type_Info`: the value's type's description.
- `value_pointer: *void`: its address. A place (spec 5.1) converts by its own address, so the `Any` sees later changes to it and writing through `value_pointer` changes it; any other value is copied into a temporary, which lives until the end of the statement holding the conversion. An `Any` converts to `Any` as itself.

Reading one: compare `a.type` with `type_info(T)` for a type known in advance, or read its `tag`, then cast: `cast(*int) a.value_pointer` for the value, `cast(*Type_Info_Struct) a.type` for the struct's description when the tag says `TYPE_INFO_STRUCT`. Nothing converts from `Any` implicitly.

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

Vec2 :: struct { x: float; y: float; }

describe :: (a: Any) {
    if a.type == type_info(int) {
        printf("an int: %lld\n", (cast(*int) a.value_pointer).*);
    } else if a.type.tag == TYPE_INFO_STRUCT {
        s := cast(*Type_Info_Struct) a.type;
        printf("%.*s with %lld fields\n", cast(s32) s.name.count, s.name.data, s.members.count);
    }
}

main :: () {
    n := 42;
    describe(n);             // n's own address
    describe(Vec2.{1, 2});   // a temporary's
}
```

It prints:

```text
an int: 42
Vec2 with 2 fields
```

### The context

The context is what code deep in a call chain needs without every caller handing it down, as in Jai (issue #156, spec 7.10): the allocator code takes its memory from when it's handed none, and the temporary storage. Every Rig procedure but `main` and a `#c_call` one is given its caller's context, a `*Context` it isn't given in the source, and `context` names it (`main` starts with `default_context`); `push_context` runs a block with another (spec 6.13).

- `Context`:
  - `allocator: Allocator`: where code takes its memory from when it's handed no allocator.
  - `temporary_storage: *Temporary_Storage`: the temporary storage.
- `default_context: Context`: the context `main` and each `#run` start with, and the one a `#c_call` procedure, which C calls with none, pushes for itself when it has no other (`push_context default_context { ... }`). Its allocator is the heap, and its temporary storage Preload's one.
- `Temporary_Storage`, memory given out from one end and given back all at once:
  - `data: *u8`, `size: int`: its first block, `TEMPORARY_STORAGE_SIZE` bytes of Preload's.
  - `occupied: int`: how far into the storage's count allocations reach, from the first block's start through the pages after it, counting the padding that aligns them and the unused tails of regions a page replaced.
  - `high_water_mark: int`: the largest `occupied` has been.
  - `overflow_allocator: Allocator`: where its pages come from when the current region, the first block or the newest page, can't hold an allocation; the heap.
  - `overflow_pages: *Temporary_Storage_Page`: those pages, newest first.
- `Temporary_Storage_Page`, a page's header, before its bytes:
  - `previous: *Temporary_Storage_Page`: the page before it, or null.
  - `size: int`: its bytes after the header.
  - `start: int`: where they start in the storage's count (`occupied`).
- `TEMPORARY_STORAGE_SIZE`: 32768, the bytes of Preload's storage's first block, and of the smallest page any storage asks for, its header included.

`__runtime_init` sets `default_context` up: its allocator, and the pointers to Preload's storage and its first block (`__temporary_storage`, `__temporary_storage_data`), which a global's value can't hold, since it can't hold another global's address (spec 4.3). It runs before `main`'s first line and before each `#run`'s code, and only the first call does anything. It's `#c_call`, so it uses no context itself. It and the two globals are Preload's own, not for programs.

#### The temporary allocator

- `temporary_allocator_proc`, `TEMPORARY_ALLOCATOR`: memory from the current context's temporary storage, `context.temporary_storage`, handed out one allocation after another and given back all at once. The procedure finds the storage through the context, so the allocator's `data` is unused, and a `push_context` with another storage makes it allocate from that one. A program that imports Basic reaches it through Basic's `talloc`, `tprint`, `reset_temporary_storage` and the storage's marks ([`../Basic/README.md`](../Basic/README.md)) rather than through its procedure.

It hands out the current region: the newest page's bytes after its header, or, with no page, the first block. A page starts in the storage's count where the region before it ended, rounded up to a multiple of 8, so `occupied` keeps counting through the pages and the newest page's own offset is `occupied - start`, found without walking the pages.

- `ALLOCATE` starts at the first multiple of 8 at or past the region's offset, so every allocation is aligned to 8 when the first block and `overflow_allocator`'s memory are, as the heap's is: a page's 24-byte header is a multiple of 8. When the rest of the region can't hold `size` bytes, a page is asked of `overflow_allocator`: `TEMPORARY_STORAGE_SIZE` bytes, or `size` with the 24-byte header when that is larger. The rest of the region before it goes unused but counts as occupied. When `overflow_allocator` has none to give, or `size` with the header would be past `int`'s largest, the result is null and the storage is as it was.
- `RESIZE` grows or shrinks the last allocation in place when the region holds `size` bytes from where that allocation starts. Otherwise it allocates anew and copies the old bytes that fit, or gives null with the memory as it was.
- `FREE` does nothing: the memory goes back with everything else at the next reset.
- `FREE_ALL` is a reset: every page goes back to `overflow_allocator`, newest first, `overflow_pages` is null and `occupied` 0, so the first block is handed out again from its start. `high_water_mark` stays: the largest `occupied` has been, counting the unused tails of regions a page replaced. A page's start, rounded up, keeps its offsets as aligned as the count's, so the mark is at least the size of a first block that would have held everything.

Memory the temporary allocator gave, used after a reset, is a memory error no check catches (spec 8.6): the bytes are handed out again, or their page is back with `overflow_allocator`. A program's own `Temporary_Storage` needs `data`, `size` and `overflow_allocator` set, and its `data` and `overflow_allocator`'s memory aligned to 8 for its allocations to be.

While compiling, every `#run` starts with the compile-time world's `default_context`, so the runs share the world's temporary storage, which nothing resets but their own code.

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

main :: () {
    t := TEMPORARY_ALLOCATOR;
    a := t.proc(ALLOCATE, 10, 0, null, t.data);
    b := t.proc(ALLOCATE, 100, 0, null, t.data);
    printf("%lld bytes occupied\n", context.temporary_storage.occupied);
    t.proc(FREE_ALL, 0, 0, null, t.data);
    printf("%lld after the reset, at most %lld\n", context.temporary_storage.occupied, context.temporary_storage.high_water_mark);
}
```

It prints:

```text
116 bytes occupied
0 after the reset, at most 116
```

`__memmove` is C's `memmove` under Preload's own name, which the temporary allocator's `RESIZE` copies with; it's not for programs.

## Waiting on the language

- No type aliases, so the allocator's procedure type is written out, in `Allocator` and in each allocator procedure.
- No enums, so the allocator's modes and the type information's tags are constants.
- No `using`, so each kind's type information struct holds its `Type_Info` as a field, `info`, and reads it as `t.info.size`.
