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
- `Type_Info_Array` (`TYPE_INFO_ARRAY`, for an array `[N]T`, a view `[]T` and a resizable array `[..]T` alike):
  - `info: Type_Info`.
  - `element_type: *Type_Info`: `T`'s description.
  - `array_kind: int`: `ARRAY_FIXED` (0) for an array, `ARRAY_VIEW` (1) for a view, `ARRAY_RESIZABLE` (2) for a resizable array (spec 3.17), in Jai's order.
  - `count: int`: the array's `N`, or -1 for a view or a resizable array, whose count is its value's. A string isn't described as a view: it has its own tag.

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

The context is what code deep in a call chain needs without every caller handing it down, as in Jai (issue #156, spec 7.10): the allocator code takes its memory from when it's handed none, the temporary storage, and the place an allocation is asked from (issue #194). Every Rig procedure but `main` and a `#c_call` one is given its caller's context, a `*Context` it isn't given in the source, and `context` names it (`main` starts with `default_context`); `push_context` runs a block with another (spec 6.13).

- `Context`:
  - `allocator: Allocator`: where code takes its memory from when it's handed no allocator.
  - `temporary_storage: *Temporary_Storage`: the temporary storage.
  - `allocation_location: Source_Code_Location` (Caller locations, below): the place the allocation being asked for was asked from, while an allocator is called. Basic's `alloc`, `resize`, `free` and `free_all` set it to their caller's place, or the `loc` they're given, around their call of the allocator, and put the one before back after ([`../Basic/README.md`](../Basic/README.md)); the resizable arrays' built-in procedures set it to their own call's place the same way, through their helpers (Resizable arrays, below). So an allocator procedure reads it to know where it is asked from, as a memory debugger records each allocation's place, though an allocator procedure's shape, Jai's, has no parameter for it. It is an ordinary field, which a program may set too: `default_context` starts with it zeroed, no place, each of these procedures puts back the value it found, whatever that was, and a direct call of an allocator's procedure leaves it as it is, so the allocator reads whatever the context holds.
- `default_context: Context`: the context `main` and each `#run` start with, and the one a `#c_call` procedure, which C calls with none, pushes for itself when it has no other (`push_context default_context { ... }`). Its allocator is the heap, its temporary storage Preload's one, and its `allocation_location` zeroed.
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

`__save_command_line(argc: s32, argv: **u8)` saves the program's command-line arguments (issue #152, spec 7.4, 10.5) into `__command_line_arguments: []string`: the name it was started by first, then each argument, each string viewing the bytes of one of the `argv` strings it is given, so a zero byte its count doesn't include follows it. `main` takes C's `argc` and `argv`, though its Rig signature doesn't say so, and calls it right after `__runtime_init`, before its first line. A null `argv` keeps what was saved, as a Rig call of `main` passes 0 and null; an `argc` of 0 or less saves the empty view. The elements come from one `__malloc`, and are never freed, nor are the strings. The `argv` given is saved as it is, on both systems: on Windows, where the C runtime's own `argv` is in the ANSI code page, Rig's start-up gives `main` the C runtime's wide arguments converted to UTF-8 in its place (issue #125, spec 10.6), so `main`'s arguments are UTF-8 there too. Until it runs, and in the compile-time world, where no startup runs (spec 7.8), the view is empty. It's `#c_call`, as `__runtime_init` is, and like it has no debug information, so a debugger steps over `main`'s call of it. Both are Preload's own: programs read the arguments through Basic.

`__runtime_error(text: string, message_start: int) #compiler` ends the program with a runtime error whose whole text is `text` (issue #157, spec 8.1, 10.5), as a failing check ends it: compiled, the C library's streams flushed, `text` written to standard error (nothing when its count is below 0 or its data null) and exit status 101; interpreted, the run fails with the message from `text`'s byte `message_start` on, its final newline dropped (all of `text` when `message_start` is outside it), and a `#run` that calls it fails as one whose check failed does (spec 7.8), its innermost call the procedure that called `__runtime_error`, at that call's name. `text` is the whole line, `PATH:L:C: runtime error: MESSAGE` and a line feed, and `message_start` where `MESSAGE` starts. It's the one `#compiler` procedure a program's own code may call: the compiler makes each call of it the panic a failing check ends in, in the executable and while compiling alike, so none is refused as a `#compiler` call in the program is (spec 7.9). It's Preload's own, for Basic's `assert` ([`../Basic/README.md`](../Basic/README.md)), which builds the line and calls it.

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

`__memmove` is C's `memmove` under Preload's own name, which the temporary allocator's `RESIZE` copies with, and so do the resizable arrays' helpers that move or copy elements (below); it's not for programs.

### Resizable arrays

A resizable array, `[..]T` (issue #158, spec 3.17), is laid out as Preload's `Resizable_Array`, Jai's layout, and the compiler needs that struct for the type: a program without Preload has no `[..]T`.

- `Resizable_Array`:
  - `count: int`, `data: *void`: a view's count and data, where a view has them, so whatever reads a view's reads a resizable array's.
  - `allocated: int`: how many elements `data`'s memory holds. The array owns that memory when `allocated` is above 0 and its elements take bytes; at 0 it owns none, though a program may have set `data` and `count` by hand, and an array of elements of 0 bytes owns none either, whatever its `allocated`, since they take no memory.
  - `allocator: Allocator`: where that memory came from, and where it goes back.

It's the untyped form, `data` a `*void`, which every `[..]T` has whatever its `T`.

The built-in procedures that grow and shrink a `[..]T` (`array_add`, `array_insert`, `array_pop`, `array_remove_ordered`, `array_remove_unordered`, `array_reserve`, `array_resize`, `array_reset`, `array_free`; spec 3.17) call Preload's helpers of the same names with two underscores before them, `__array_add` and so on, written in Rig so they work the same compiled, interpreted and while compiling. Each takes the array as a `*Resizable_Array` and its element's size in bytes, which only the caller's `T` says, then what the built-in takes after the array: `__array_add` and `__array_insert` take the item by the address of a copy of its own, and `__array_pop` takes the address its last element is copied to, the built-in's result. The five that can reach the allocator, `__array_add`, `__array_insert`, `__array_reserve`, `__array_resize` and `__array_free`, take last the built-in's place, `loc: Source_Code_Location`, its name in its call, which the compiler makes as it makes a default `#caller_location` (spec 7.2). The compiler checks the pointer and `array_resize`'s count before the call, at the built-in's name, and the index and `array_pop`'s count too unless bounds checks are off (`#no_abc`, `--no-bounds-checks`; spec 7.7), when an index outside the array is a memory error no check catches (spec 8.6). For every built-in but `array_reset` and `array_free`, which use no element, it checks too, bounds checks on or off, what the helpers trust the array to say of itself:

- a count of 0 or more, but in `array_reserve`, which places no element from it: `'array_add' was given an array with a negative count`, where no bounds check failed it first;
- `data` not null while the count or `allocated` is above 0: such a count says elements are there and such an `allocated` says room is, which no memory holds, so the built-in stops as indexing would, with `null pointer dereference` (spec 8.4), even where its helper would touch nothing;
- in `array_pop` and the removes, the count not past `allocated` when the array owns memory, since they read and move its last elements: `'array_pop' was given an array with more elements than its memory holds`. The others grow before they write past `allocated`.

It checks the result of those that grow. The helpers check none of these. They're Preload's own, not for programs, and so are `__array_grow`, which asks for the memory, `__array_no_bytes` and `__memset`, C's `memset`, which `__array_resize` zeroes new elements with.

- **The allocator.** A zeroed array's allocator has a null procedure. When the array first grows, it takes the caller's `context.allocator` and keeps it, even when that gives no memory, so it grows and goes back to that allocator wherever it's used later. Setting `.allocator` before the first growth chooses another; changing it while the array holds memory is a memory error no check catches (spec 8.6).
- **Growth.** Adding or inserting into a full array, its count at least `allocated`, grows it to the larger of 8 and twice `allocated`, so `n` elements added one at a time are moved about `n` times in all; past half of `int`'s largest, to `int`'s largest. `array_reserve` and `array_resize` grow to exactly the count they're given, or to the array's count when that is more, as for an array whose `data` and `count` a program set by hand. Room whose bytes pass `int`'s largest asks for `int`'s largest bytes, which no allocator gives.
- **Asking for memory.** An array that owns none is given it with `ALLOCATE`, copying the elements whose `data` and `count` a program set by hand, which stay where they were; one that owns some has it resized with `RESIZE`, from `allocated` elements' bytes. An allocator's null leaves the array as it was, and the built-in stops the program with `the array's allocator has no memory to give`.
- **Giving back.** `array_free` gives the memory with `FREE` and its size, `allocated` elements' bytes, so an allocator that records sizes gets them. The array is then empty, `data` null and `allocated` 0, and keeps its allocator. It gives nothing back for an array that owns no memory.
- **The place.** While `__array_grow` and `__array_free` call the allocator, `context.allocation_location` is the place they were given, and the one before is put back after, as Basic's `alloc` and `free` do: an allocator reads the program's line where the built-in was called, not Preload's (spec 7.10). The place has no default, since the compiler always passes it; a program that calls a helper by hand passes one too, `.{}` for no place.
- **Elements of no size** (`[..]Empty`, for a struct with no fields) take no memory: growing asks for nothing, and `data` becomes `__array_no_bytes`'s address, so indexing and `for` find it not null. It's an `int`, for its 8-byte alignment, which an element of 0 bytes can still need (a struct holding a `[0]int`).

### Caller locations

A parameter whose default value is `#caller_location` (issue #194, spec 7.2), as Jai has it, is given the place of each call that leaves it out: `loc: Source_Code_Location = #caller_location`. The compiler needs Preload's struct for it: a program without Preload has no `#caller_location`.

- `Source_Code_Location`, a place in the program's source:
  - `file: string`: the path messages name the call's file by, as a runtime error there would (spec 8.1): the main file's path as `rig build` was given it, a loaded file's as its `#load` made it, a module's file by its absolute path.
  - `line: int`, `column: int`: where the callee's name is in the call, counted from 1 (spec 2.2), so two calls on one line have two places.

A zeroed `Source_Code_Location`, `file` empty and `line` 0, is no place. The context's `allocation_location` is one (The context, above).

## Waiting on the language

- No type aliases, so the allocator's procedure type is written out, in `Allocator` and in each allocator procedure.
- No enums, so the allocator's modes and the type information's tags are constants.
- No `using`, so each kind's type information struct holds its `Type_Info` as a field, `info`, and reads it as `t.info.size`.
