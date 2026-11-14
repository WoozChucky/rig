# Basic

Basic is Rig's first library, written in Rig over a thin layer of C. It holds what most programs need first. For now that is the class and case of a byte, min, max, clamp and abs, copying, setting and comparing bytes in memory, numbers to text and back, views of strings, allocators (the heap, an arena and a pool), a string builder, a growable array, hash tables, and printing to standard output (issue #134).

## Loading it

Rig has no `#import` yet, so a program loads Basic with `#load`, by the path from the program's own directory to `module.rig`. From `tests/run`:

```rig
#load "../../modules/Basic/module.rig";
```

`module.rig` loads every part. A loaded file's declarations join the program's one top level (spec 2.13), so every name listed below is the program's too: a program that loads Basic can't declare a procedure, struct or constant of the same name.

## Its C layer

`libc.rig` declares the only C functions Basic calls, each under a name of its own that starts with `c_` (`#foreign libc "NAME"`, spec 7.6). So a program that loads Basic keeps C's own names: it may declare C's `putchar`, `malloc` and `realloc` itself. `free` is the exception, since Basic's allocators take that name for their own helper and `c_free` is taken too, so such a program declares C's `free` under another name, as in `libc_free :: (p: *void) #foreign libc "free";`.

- `c_putchar(c: s32) -> s32`: C's `putchar`, the one call that writes a byte to standard output.
- `c_malloc(size: u64) -> *void`, `c_realloc(memory: *void, size: u64) -> *void`, `c_free(memory: *void)`: C's `malloc`, `realloc` and `free`, the heap allocator's memory. Basic never calls them with a size of 0.

## Characters (`char.rig`)

A char is a `u8`, one byte of a string, and the classes are ASCII's, as C's `<ctype.h>` has them in the "C" locale. Every byte from 128 up is in none of them, and its case doesn't change, so UTF-8 text passes through unharmed.

- `is_digit(c: u8) -> bool`: `0` to `9`.
- `is_alpha(c: u8) -> bool`: `a` to `z` and `A` to `Z`.
- `is_alnum(c: u8) -> bool`: a letter or a digit; `_` isn't.
- `is_space(c: u8) -> bool`: a space, tab, line feed, vertical tab (11), form feed (12) or carriage return.
- `is_upper(c: u8) -> bool`: `A` to `Z`.
- `is_lower(c: u8) -> bool`: `a` to `z`.
- `to_upper(c: u8) -> u8`: the uppercase of a lowercase letter; any other byte unchanged.
- `to_lower(c: u8) -> u8`: the lowercase of an uppercase letter; any other byte unchanged.

## Math (`math.rig`)

One procedure per type, named by it: `int` and `u64` (every other integer type widens to one of them), `float32` and `float64`. `min` and `max` return their first operand unless the second is strictly smaller or larger, so a NaN first operand comes back from both and a NaN second one never does.

- `min_int`, `min_u64`, `min_float32`, `min_float64` `(a: T, b: T) -> T`: the smaller of `a` and `b`; `a` when they're equal.
- `max_int`, `max_u64`, `max_float32`, `max_float64` `(a: T, b: T) -> T`: the larger of `a` and `b`; `a` when they're equal.
- `clamp_int`, `clamp_u64`, `clamp_float32`, `clamp_float64` `(value: T, low: T, high: T) -> T`: `low` for a value below it, `high` for one above it, otherwise `value`.
- `abs_int(value: int) -> int`: the magnitude; the most negative `int` is itself, since its negation wraps.
- `abs_float32(value: float32) -> float32`, `abs_float64(value: float64) -> float64`: the value with its sign bit cleared, so `-0.0` gives `0.0` and NaN stays NaN.

## Memory (`memory.rig`)

C's `memmove`, `memset` and `memcmp`, written as byte loops in Rig rather than called from C. Memory is passed as `*void`, so any pointer converts to it. A count of 0 or less does nothing and reads nothing.

- `copy_memory(destination: *void, source: *void, count: int)`: copies `count` bytes from `source` to `destination`; the two may overlap, and the bytes copied are the ones `source` held before the copy.
- `set_memory(destination: *void, value: u8, count: int)`: sets `count` bytes from `destination` on to `value`.
- `compare_memory(a: *void, b: *void, count: int) -> int`: -1, 0 or 1 as the first of `count` bytes that differs is smaller or larger in `a` than in `b`, the bytes compared unsigned; 0 when none differs.

## Numbers (`numbers.rig`)

Numbers are written into a buffer the caller gives, and the text comes back as a string viewing the buffer's first bytes, so nothing allocates. A buffer of `INT_TEXT_MAX` or `FLOAT_TEXT_MAX` bytes is always enough; one too short stops at the language's bounds check (unless built with `--no-bounds-checks`). Parsing takes the whole string or nothing: no spaces, prefixes or `_`.

- `INT_TEXT_MAX`: 65, the most bytes `format_int` writes (64 binary digits and a sign).
- `FLOAT_TEXT_MAX`: 320, the most bytes `format_float` writes (a sign, 309 digits, a point and 9 decimals).
- `digit_value(c: u8) -> int`: 0-9 for `0`-`9`, 10-35 for `a`-`z` and `A`-`Z`, -1 for any other byte.
- `format_int(buffer: []u8, value: int, base: int) -> string`: writes `value` in a base from 2 to 36, lowercase, with `-` before a negative value; another base writes nothing and gives `""`.
- `parse_int(s: string, base: int) -> int, bool`: reads an optional `+` or `-` and one digit or more of the base, either case; `0, false` for anything else, a value outside `int` or a base outside 2 to 36.
- `format_float(buffer: []u8, value: float64, decimals: int) -> string`: writes `value` with 0 to 9 decimals (others are clamped), the last rounded half away from zero on the `float64`'s own value, though a value within about a unit in the last place of a tie can round the other way (`1.805` with 2 decimals is `1.81`); `nan`, `inf`, `-inf`, and `-` before any negative value, `-0.0` included. From 1e18 up, 18 leading digits are written, of which only about the first 15 or 16 are the value's, and the rest are zeros.
- `parse_float(s: string) -> float64, bool`: reads `[+-] digits [. digits] [(e|E) [+-] digits]`, with a digit before or after the point; `0, false` for anything else. A value too large is an infinity. Correctly rounded when the significant digits fit in 2^53 and the power of ten is within ±22, otherwise within a few units in the last place.

## Strings (`string.rig`)

Every string returned is a view into the string given: nothing is copied or allocated. Indexes and counts are in bytes, and bytes are compared unsigned, with no knowledge of UTF-8. A zeroed string (count 0, data null) is the empty string everywhere, and no byte is read through its null data.

- `strings_equal(a: string, b: string) -> bool`: `a == b`, as a procedure a procedure value can name.
- `compare_strings(a: string, b: string) -> int`: -1, 0 or 1 by the first byte that differs, compared unsigned; a prefix sorts first.
- `find_byte(s: string, byte: u8) -> int`: the index of the first `byte` in `s`, or -1.
- `find_byte_from_end(s: string, byte: u8) -> int`: the index of the last `byte` in `s`, or -1.
- `find_string(s: string, needle: string) -> int`: the index of the first `needle` in `s`, or -1; the empty needle is found at 0.
- `find_string_from_end(s: string, needle: string) -> int`: the index of the last `needle` in `s`, or -1; the empty needle is found at `s.count`.
- `starts_with(s: string, prefix: string) -> bool`: whether `s` begins with `prefix`; every string begins with `""`.
- `ends_with(s: string, suffix: string) -> bool`: whether `s` ends with `suffix`; every string ends with `""`.
- `trim_left(s: string) -> string`: `s` without the `is_space` bytes at its start.
- `trim_right(s: string) -> string`: `s` without the `is_space` bytes at its end.
- `trim(s: string) -> string`: `s` without the `is_space` bytes at either end.
- `slice(s: string, start: int, count: int) -> string`: `count` bytes from `start`, clamped to stay in `s`: `start` to `0..s.count`, then `count` to `0..s.count - start`.
- `advance(s: string, amount: int) -> string`: `s` without its first `amount` bytes, clamped as `slice` is.
- `split(s: string, separator: u8, parts: []string) -> int`: writes the pieces of `s` between separators into `parts` and returns how many; empty pieces count, `""` is one empty piece, the last part holds the unsplit rest when `parts` runs out, and an empty `parts` gets 0.
- `split_from_left(s: string, separator: u8) -> bool, string, string`: whether `separator` is in `s`, and the bytes before and after its first occurrence; without one, `false`, `s` and an empty view at the end of `s`.

## Allocators (`allocators.rig`)

An allocator is passed to whatever needs memory, as Jai has it: an `Allocator` holds a procedure and the data it works on, and the procedure is called with a mode, `ALLOCATE`, `RESIZE`, `FREE` or `FREE_ALL`. A program calls the four helpers rather than the procedure. They settle the edges, so an allocator is only ever asked for more than 0 bytes and only ever given memory that isn't null to resize or free. An allocator that can't give memory returns null. The heap's memory is aligned as C's `malloc` aligns it, and an arena's or a pool's to 8 bytes when its parent's is, as the heap's is.

- `ALLOCATE`, `RESIZE`, `FREE`, `FREE_ALL`: the modes, 0 to 3.
- `Allocator`: `proc: (mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void` and `data: *void`.
- `alloc(size: int, allocator: Allocator) -> *void`: `size` bytes, or null; null for a size of 0 or less, without asking the allocator.
- `resize(memory: *void, old_size: int, new_size: int, allocator: Allocator) -> *void`: `memory` made `new_size` bytes long, perhaps moved, its first bytes kept; null when the allocator can't, and `memory` is as it was. Resizing null is `alloc`, and resizing to 0 or less is `free` and gives null.
- `free(memory: *void, allocator: Allocator)`: gives `memory` back; freeing null does nothing.
- `free_all(allocator: Allocator)`: gives back everything the allocator gave, for an arena or a pool; the heap's does nothing.
- `heap_allocator_proc`, `HEAP_ALLOCATOR`: C's heap, through `c_malloc`, `c_realloc` and `c_free`.
- `Arena_Block`: an arena block's header, `previous: *Arena_Block` and `size: int`, 16 bytes, with its `size` bytes after it.
- `Arena`: `parent: Allocator`, `block_size: int`, `block: *Arena_Block` (the newest) and `used: int` (its bytes handed out).
- `arena_init(arena: *Arena, block_size: int, parent: Allocator)`: an empty arena whose blocks of `block_size` bytes (4096 for 0 or less) come from `parent`.
- `arena_allocator_proc`, `arena_allocator(arena: *Arena) -> Allocator`: allocations one after another in the newest block, each at a multiple of 8, and a new block of `block_size` bytes, or the allocation's size when larger, when one doesn't fit. A resize allocates anew and copies, and a free does nothing; `free_all` gives every block back.
- `arena_reset(arena: *Arena)`: gives back every block but the newest, and hands that one out again from its start.
- `Pool_Slot`: a free slot, `next: *Pool_Slot`, held in the slot's own first bytes.
- `Pool`: `parent: Allocator`, `slot_size: int`, `slot_count: int`, `memory: *u8` and `free_list: *Pool_Slot`.
- `pool_init(pool: *Pool, slot_size: int, slot_count: int, parent: Allocator)`: an empty pool of `slot_count` slots of `slot_size` bytes, raised to at least 8 and up to a multiple of 8, whose memory comes from `parent` at the first allocation.
- `pool_allocator_proc`, `pool_allocator(pool: *Pool) -> Allocator`: a free slot for up to `slot_size` bytes, in address order at first, and null for more or when none is free. A resize within a slot keeps it, and one past it gives null; a free puts the slot back, to be handed out next, and `free_all` gives the memory back to `parent`, after which the pool can be used again.

## String builder (`builder.rig`)

Text put together a piece at a time, in memory of the builder's own from the allocator it is given, and copied out as one string. When an append doesn't fit, the capacity becomes the larger of 64 and twice the old one, doubled again until it fits, and the memory moves with `resize`. Numbers are written on the stack first, so nothing but the builder's memory is allocated. An allocator that has no memory to give stops the program at the next write, with the language's `null pointer dereference`.

- `String_Builder`: `allocator: Allocator`, `data: *u8`, `count: int` (the bytes built) and `capacity: int` (the bytes `data` has room for).
- `builder_init(builder: *String_Builder, allocator: Allocator)`: an empty builder whose memory comes from `allocator`, taken at the first append that needs it.
- `append_string(builder: *String_Builder, s: string)`: appends the bytes of `s`; an empty or zeroed string appends nothing and takes no memory. `s` may be a view of the builder's own text: when the append grows the builder, the bytes are copied from where they moved to.
- `append_byte(builder: *String_Builder, byte: u8)`: appends the byte itself, not its number.
- `append_int(builder: *String_Builder, value: int, base: int)`: appends `value` as `format_int` writes it.
- `append_float(builder: *String_Builder, value: float64, decimals: int)`: appends `value` as `format_float` writes it.
- `append_bool(builder: *String_Builder, value: bool)`: appends `true` or `false`.
- `builder_to_string(builder: *String_Builder, allocator: Allocator) -> string`: a copy of the text in memory from `allocator`, which the caller gives back with `free(s.data, allocator)`; an empty builder gives the zeroed string (count 0, data null) and allocates nothing.
- `builder_reset(builder: *String_Builder)`: empties the builder, keeping its memory.
- `builder_free(builder: *String_Builder)`: gives the memory back; the builder is empty, with no memory, and keeps its allocator.

## Growable array (`array.rig`)

Elements of one size, one after another in memory from the allocator the array is given, growing as they are added. The array is untyped: it knows only its element's size, copies elements in and out by that many bytes, and hands out an element's address as a `*void` for the caller to cast, valid until the array next grows. When the elements fill the capacity, it becomes the larger of 8 and twice the old one, and the memory moves with `resize`. Elements of 0 bytes take no memory, and their address is null. An index outside the elements stops the program with the language's own bounds check (unless built with `--no-bounds-checks`), and an allocator that has no memory to give stops it at the next element reached, with the language's `null pointer dereference`.

- `check_index(index: int, count: int)`: stops the program with the language's `index I is out of bounds (count C)` unless `index` is at least 0 and less than `count`.
- `Array`: `element_size: int`, `count: int` (the elements held), `capacity: int` (the elements `data` has room for), `data: *u8` and `allocator: Allocator`.
- `array_init(array: *Array, element_size: int, allocator: Allocator)`: an empty array of elements of `element_size` bytes, whose memory comes from `allocator`, taken at the first add. A negative `element_size` is taken as 0.
- `array_add(array: *Array, item: *void) -> *void`: adds a copy of the bytes at `item` as the last element, or a zeroed element for null, and gives its address.
- `array_get(array: Array, index: int) -> *void`: the address of element `index`.
- `array_pop(array: *Array, item: *void)`: copies the last element into `item`, unless null, and removes it; an empty array stops the program.
- `array_insert(array: *Array, index: int, item: *void) -> *void`: inserts a copy of the bytes at `item`, or a zeroed element for null, as element `index`, from 0 to the count, moving the later elements up, and gives its address. `item` may be one of the array's own elements: it is copied from where the move and any growing put it.
- `array_remove_ordered(array: *Array, index: int)`: removes element `index`, moving the later elements down, in order.
- `array_remove_unordered(array: *Array, index: int)`: removes element `index` by moving the last element into its place.
- `array_reset(array: *Array)`: empties the array, keeping its memory.
- `array_free(array: *Array)`: gives the memory back; the array is empty, with no memory, and keeps its element size and allocator.

## Hash tables (`table.rig`)

Values found by their key, a string (`Table`) or an int (`Int_Table`). Like the array, a table is untyped: it knows only its value's size, copies values in by that many bytes, and hands out a value's address as a `*void` for the caller to cast, valid until the table next grows. A string key is copied into memory from the table's allocator when it is set, and given back when it is removed.

The entries are one run of memory, a power of two of them, each a header (the key's 64-bit FNV-1a hash, the entry's state and the key) and then the value, at a multiple of 8, so every value is aligned to 8. Keys are found by open addressing with linear probing; a removed key's entry is marked removed, so keys further along its chain are still found, and a new key takes the first removed entry its probe passed. Before a set, when one more key would leave more than three quarters of the entries used or removed, the table is rehashed: into 16 entries the first time, twice as many when the keys would fill more than half, and otherwise as many, which only clears the removed entries. A table with no entries, before its first set or after it is freed, is found in without touching any. An allocator that has no memory to give stops the program at the next write, with the language's `null pointer dereference`.

- `hash_bytes(data: *u8, count: int) -> u64`: the 64-bit FNV-1a hash of `count` bytes; the offset basis, 14695981039346656037, for 0 or less.
- `hash_string(s: string) -> u64`: the hash of the bytes of `s`.
- `hash_int(key: int) -> u64`: the hash of the 8 bytes of `key`, in the order memory holds them.
- `TABLE_EMPTY`, `TABLE_USED`, `TABLE_REMOVED`: an entry's state, 0 to 2.
- `Table_Entry_Header`: `hash: u64` and `state: int`, what both kinds of entry start with.
- `Table_Entry`: a string-keyed entry's header, `hash: u64`, `state: int` and `key: string` (the table's copy).
- `Int_Table_Entry`: an int-keyed entry's header, `hash: u64`, `state: int` and `key: int`.
- `TABLE_VALUE_OFFSET`, `INT_TABLE_VALUE_OFFSET`: where the value starts in each kind's entry, 32 and 24.
- `Table`, `Int_Table`: `value_size: int`, `entry_size: int`, `count: int` (the keys held), `removed: int` (the entries marked removed), `capacity: int` (the entries), `entries: *u8` and `allocator: Allocator`.
- `table_entry_size(value_offset: int, value_size: int) -> int`: an entry's bytes, the value's size rounded up to 8 after its offset.
- `table_probe(entries: *u8, capacity: int, entry_size: int, hash: u64, key: *void, same_key: (entry: *u8, key: *void) -> bool) -> int, int`: the probe both kinds share; the index of the key's entry or -1, and where the key would go, or -1 with no entries.
- `table_make_room(entries: *u8, capacity: int, count: int, removed: int, entry_size: int, allocator: Allocator) -> *u8, int, int`: the growing both kinds share; the entries, capacity and removed count, rehashed when one more key needs it. The old entries are the caller's to give back.
- `table_same_key(entry: *u8, key: *void) -> bool`, `int_table_same_key(entry: *u8, key: *void) -> bool`: whether an entry holds the key at `key`, for `table_probe`.
- `table_init(table: *Table, value_size: int, allocator: Allocator)`, `int_table_init(table: *Int_Table, value_size: int, allocator: Allocator)`: an empty table of values of `value_size` bytes, whose memory comes from `allocator`, taken at the first set. A negative `value_size` is taken as 0.
- `table_set(table: *Table, key: string, value: *void) -> *void`, `int_table_set(table: *Int_Table, key: int, value: *void) -> *void`: sets `key`'s value to a copy of the bytes at `value`, or zeroed bytes for null, adding the key if it isn't there, and gives the value's address. `value` may be one of the table's own values.
- `table_find(table: Table, key: string) -> *void`, `int_table_find(table: Int_Table, key: int) -> *void`: the address of `key`'s value, or null.
- `table_remove(table: *Table, key: string) -> bool`, `int_table_remove(table: *Int_Table, key: int) -> bool`: removes `key`, giving a string key's copy back, and says whether it was there.
- `table_entry(table: Table, index: int) -> bool, string, *void`, `int_table_entry(table: Int_Table, index: int) -> bool, int, *void`: entry `index`, from 0 to the capacity less one: whether it holds a key, the key and the value's address; false, the zeroed string or 0, and null for one that holds none. Any other index stops the program.
- `table_free(table: *Table)`, `int_table_free(table: *Int_Table)`: gives every key's copy and the entries back; the table is empty, with no entries, and keeps its value size and allocator.

## Print (`print.rig`)

Everything goes to standard output through C's `putchar`, so it comes out in order with what a program writes with C's `printf`, compiled and interpreted. Bytes are written as they are, so UTF-8 text prints as UTF-8.

- `print_string(s: string)`: writes the bytes of `s`; an empty or zeroed string writes nothing.
- `print_byte(byte: u8)`: writes the byte itself, not its number.
- `print_bool(value: bool)`: writes `true` or `false`.
- `print_int(value: int)`: writes `value` in base 10.
- `print_float(value: float64, decimals: int)`: writes `value` with `decimals` digits after the point, as `format_float` does.
- `print_line(s: string)`: writes `s` and a line feed.

## Waiting on the language

What Basic would do differently once Rig has it:

- No type aliases, so the allocator's procedure type, and the key comparison the tables' probe takes, are written out wherever they're used.
- No overloading or generics, so procedures are named by type (`min_int`, `min_float64`), the collections are untyped, handing out `*void`, and there is one kind of table per kind of key, `Table` and `Int_Table`, with the same fields.
- No way for a struct to take in another's fields, as Jai's `using` does, so both kinds of table entry repeat the fields of `Table_Entry_Header`, and the code they share reads an entry's start through it.
- No enums, so an allocator's modes and a table entry's states are constants.
- No portable way to reach standard error and no conditional compilation, so Basic writes only through `c_putchar`, and an index out of bounds is reported by the language's own bounds check.
- No `defer`, so what is allocated is freed by hand on every path.
- No `#import`, so a program loads Basic with a `#load` of its relative path.
