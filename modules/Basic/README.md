# Basic

Basic is Rig's first library, written in Rig over a thin layer of C. It holds what most programs need first. For now that is the class and case of a byte, min, max, clamp and abs, π and C's math functions (roots, trigonometry, exponentials and logarithms, powers, rounding), copying, setting and comparing bytes in memory, numbers to text and back, views and copies of strings and their UTF-8, allocators (an arena and a pool, over Preload's heap), the context's temporary storage, a string builder, hash tables, sorting and searching, paths, files, directories, the clock, the calendar and sleeping, random numbers, the environment's variables, commands run by the system's shell, with or without their output, and the command-line arguments, formatted printing to standard output and standard error, to a string builder and to a string, `assert`, and a memory debugger, an allocator that records what it gives and reports what isn't freed (issues #134, #145, #148, #152, #154, #156, #157, #159 and #209). Whatever hands back memory it allocates takes it from the context's allocator unless it is given another (Allocators, below), so a caller decides where the memory of everything it calls comes from without passing an allocator down; `talloc` and `tprint` take theirs from the context's temporary storage instead. `sprint`'s, `print_to_builder`'s and `print_error`'s scratch builders, the file and directory procedures' copies of a path for the system, `list_directory`'s gathered names and sorting scratch, `run_command_with_output`'s gathered output, and on Windows `get_environment`'s UTF-16 buffer for the value and the commands' builder of the command line and copy of the shell's path come from the heap, and are freed before the call returns; the memory debugger's records come from the heap too, and stay until `memory_debugger_free`, and its report's sorted copy of them and the scratch it gives `sort` are the heap's, freed before it returns. A failing `assert`'s line is the one allocation from the heap Basic never gives back: the program, or the `#run`, ends there. `sort`'s scratch memory and the environment's copies of a name or a command for the system (UTF-16 on Windows, ended by a zero on Linux) come from the allocator the call is given, the context's unless another is. The command-line arguments are Preload's, saved before `main`'s first line, and never freed. A resizable array, `[..]T`, grown by `array_add` and the language's other built-in procedures, is the language's, not Basic's (spec 3.17): Basic's procedures that take a view, such as `join` and `split`, take one as the view of its elements, and `print` writes one as it writes a view.

## Importing it

Basic ships with rig, so a program imports it by its name (spec 2.14):

```rig
#import "Basic";
```

rig finds `Basic/module.rig` in the `modules` directory beside it, or in the one above it, the repository's. A `#load` of `module.rig` by its path from the program's own directory still works (from `tests/run`, `#load "../../modules/Basic/module.rig";`), and a program that does both reads the files once.

`module.rig` loads every part. An imported file's declarations join the program's one top level (spec 2.13), so every name listed below is the program's too: a program that imports Basic can't declare a procedure, struct or constant of the same name.

## Its C layer

`libc.rig` holds the C functions Basic declares, each under a name of its own that starts with `c_` (`#foreign libc "NAME"`, spec 7.6). So a program that loads Basic keeps C's own names: it may declare C's `putchar`, `malloc`, `realloc`, `fopen`, `getenv` and the rest itself. `free` is the exception, since Basic's allocators take that name for their own helper and `c_free` is taken too, so such a program declares C's `free` under another name, as in `libc_free :: (p: *void) #foreign libc "free";`.

Each of them but the per-system ones below links and runs on Windows (the UCRT) and Linux (glibc's `libc`, and its `libm` for `<math.h>`, which every program links too, spec 10.6), compiled and interpreted, with one declaration that serves both: where C's types differ (`fseek`'s `long` offset, `fpos_t`, `struct timespec`), the declaration and its comment say how Basic stays within what both agree on. Basic's own files use C's file functions, its environment C's `getenv`, and its commands C's `system`, only on Linux; on Windows they go through `kernel32`'s wide calls (below, Files and The environment), and C's stay declared there for a program to call. `libc.rig`'s header says which C functions Basic leaves out, and why: `ftell` (C's `long`, 32 bits on Windows), `_ftelli64` and `ftello` (one system each, and on Windows, interpreted, `_ftelli64` is a second C runtime's, which refuses Basic's `FILE *`), `time` (whole seconds) and `clock` (a different clock on each system).

- `c_putchar(c: s32) -> s32`: C's `putchar`, the one call that writes a byte to standard output.
- `c_malloc(size: u64) -> *void`, `c_realloc(memory: *void, size: u64) -> *void`, `c_free(memory: *void)`: C's `malloc`, `realloc` and `free`, the C heap for a program that imports Basic. Basic itself takes heap memory through Preload's `HEAP_ALLOCATOR`, and Preload declares the three under names of its own, so Basic doesn't call these. What C gives for a size of 0 is left to each C library, and Basic's allocator helpers never ask for it.
- `C_Fpos`: C's `fpos_t`, `position: s64` (the offset) and `reserved: u64`, 16 bytes, which holds the UCRT's 8 and glibc's 16.
- `C_SEEK_SET`, `C_SEEK_END`: `fseek`'s origins, 0 and 2.
- `c_fopen(path: *u8, mode: *u8) -> *void`, `c_fclose(file: *void) -> s32`: C's `fopen` and `fclose`; a `FILE *`, or null when the file can't be opened. Basic's files open with them on Linux.
- `c_fread(buffer: *void, size: u64, count: u64, file: *void) -> u64`, `c_fwrite(data: *void, size: u64, count: u64, file: *void) -> u64`: C's `fread` and `fwrite`, giving the items read or written.
- `c_fseek(file: *void, offset: s64, origin: s32) -> s32`: C's `fseek`. The offset is C's `long`, 32 bits on Windows, so Basic passes only 0.
- `c_fgetpos(file: *void, position: *C_Fpos) -> s32`, `c_fsetpos(file: *void, position: *C_Fpos) -> s32`: C's `fgetpos` and `fsetpos`, the position in a file in 64 bits on both systems.
- `c_ferror(file: *void) -> s32`: C's `ferror`, not 0 when a read or write has failed.
- `c_fflush(file: *void) -> s32`: C's `fflush`; null flushes every stream.
- `c_remove(path: *u8) -> s32`: C's `remove`, 0 when the file is deleted. On Linux it deletes an empty directory too.
- `c_getenv(name: *u8) -> *u8`: C's `getenv`, a variable's value, or null. Basic's `get_environment` calls it on Linux.
- `c_system(command: *u8) -> s32`: C's `system`, which runs a command through the system's shell and gives its status; null asks whether there is a shell. Basic's `run_command` calls it on Linux.
- `C_Timespec`: C's `struct timespec`, `seconds: s64`, `nanoseconds: s32` and `padding: s32`, 16 bytes, which holds the UCRT's (a 4-byte `long` of nanoseconds and padding) and glibc's (an 8-byte `long`, whose low 4 bytes hold them). Basic passes one to Linux's `nanosleep` with `padding` 0, so that glibc's whole 8-byte `long` is the nanoseconds.
- `C_TIME_UTC`: `timespec_get`'s base for UTC, 1.
- `c_timespec_get(time: *C_Timespec, base: s32) -> s32`: C's `timespec_get`, the wall clock's seconds and nanoseconds since 1970; the base, or 0 when it can't. On Windows the compiler links it to the UCRT's `_timespec64_get`, as a C program's `<time.h>` does (issue #161, spec 10.6).
- `c_sqrt`, `c_cbrt`, `c_sin`, `c_cos`, `c_tan`, `c_asin`, `c_acos`, `c_atan`, `c_exp`, `c_exp2`, `c_log`, `c_log2`, `c_log10`, `c_floor`, `c_ceil`, `c_round`, `c_trunc` `(x: float64) -> float64`, `c_atan2(y: float64, x: float64) -> float64`, `c_pow(x: float64, y: float64) -> float64`, `c_hypot(x: float64, y: float64) -> float64`, `c_fmod(x: float64, y: float64) -> float64`: C's `<math.h>` functions of a `double`, libm's on Linux.
- `c_sqrtf`, `c_cbrtf`, `c_sinf`, `c_cosf`, `c_tanf`, `c_asinf`, `c_acosf`, `c_atanf`, `c_expf`, `c_exp2f`, `c_logf`, `c_log2f`, `c_log10f`, `c_floorf`, `c_ceilf`, `c_roundf`, `c_truncf` `(x: float32) -> float32`, `c_atan2f(y: float32, x: float32) -> float32`, `c_powf(x: float32, y: float32) -> float32`, `c_hypotf(x: float32, y: float32) -> float32`, `c_fmodf(x: float32, y: float32) -> float32`: C's `<math.h>` functions of a `float`, libm's on Linux. On Windows the compiler links `c_hypotf` to the UCRT's `_hypotf`, as a C program's `<math.h>` does (spec 10.6).

The per-system part, at the end of `libc.rig`, is a top-level `#if OS == OS_WINDOWS { ... } else { ... }` (spec 4.9) that declares each system's own functions under one `c_` name, with that system's signature: a function only one system has, or one whose types differ between them. The procedures that call them choose with a statement `#if`. On Windows the part holds `kernel32`'s own functions too (`Sleep`, and the wide calls of the files, the directories, the environment and the commands), which every program links and the interpreter looks up in `kernel32.dll` (spec 10.6, 10.8), so compiled and interpreted programs call the same ones. On Windows, interpreted, the one whose name starts with `_`, `_write`, comes from `ucrtbase.dll`, a second C runtime (spec 10.8), so Basic gives it only standard error's descriptor, 2, which both runtimes have, never a `FILE *` from the calls above.

- `c_write`: writes bytes to a file descriptor at once, with no buffer of C's, and gives how many it wrote, or -1; Basic writes standard error (descriptor 2) with it, as the runtime writes a runtime error (spec 10.5). Windows: `c_write(descriptor: s32, data: *void, count: u32) -> s32`, the UCRT's `_write`, which writes each `\n` as `\r\n` (text mode). Linux: `c_write(descriptor: s32, data: *void, count: u64) -> s64`, `write`.
- `c_sleep(milliseconds: u32)`, Windows only: `kernel32`'s `Sleep`, which works in the system's clock ticks, about 15.6 ms apart, so it may wake up to a tick early, or later; its largest count, `0xFFFFFFFF`, is `INFINITE`, a sleep that never ends.
- `c_nanosleep(request: *C_Timespec, remaining: *C_Timespec) -> s32`, Linux only: `nanosleep`, which sleeps for at least `request`'s time and gives 0, or -1 when a signal woke it first, having written the time left to `remaining`.
- Windows only, for the files and directories, `kernel32`'s wide calls, which take a path as UTF-16:
  - `C_Find_Data`: `WIN32_FIND_DATAW`, 592 bytes: `attributes: u32`, `times: [6]u32` (its three `FILETIME`s), `size_high: u32`, `size_low: u32`, `reserved0: u32`, `reserved1: u32`, `name: [260]u16` (at offset 44) and `alternate_name: [14]u16`.
  - `C_INVALID_FILE_ATTRIBUTES` (4294967295), `C_FILE_ATTRIBUTE_DIRECTORY` (16), `C_ERROR_FILE_NOT_FOUND` (2), `C_ERROR_NO_MORE_FILES` (18), `C_CP_UTF8` (65001), `C_MB_ERR_INVALID_CHARS` (8).
  - `c_multi_byte_to_wide_char(code_page: u32, flags: u32, bytes: *u8, byte_count: s32, wide: *u16, wide_count: s32) -> s32`: `MultiByteToWideChar`, bytes to UTF-16; with `C_MB_ERR_INVALID_CHARS`, 0 for bytes that aren't valid.
  - `c_wide_char_to_multi_byte(code_page: u32, flags: u32, wide: *u16, wide_count: s32, bytes: *u8, byte_count: s32, default_char: *u8, used_default: *s32) -> s32`: `WideCharToMultiByte`, UTF-16 to bytes; a `wide_count` of -1 converts up to the zero, and writes it.
  - `c_get_file_attributes_w(path: *u16) -> u32`: `GetFileAttributesW`, `C_INVALID_FILE_ATTRIBUTES` when nothing is there.
  - `c_create_directory_w(path: *u16, security: *void) -> s32`, `c_remove_directory_w(path: *u16) -> s32`: `CreateDirectoryW` and `RemoveDirectoryW`, not 0 when they did.
  - `c_find_first_file_w(pattern: *u16, data: *C_Find_Data) -> *void`, `c_find_next_file_w(handle: *void, data: *C_Find_Data) -> s32`, `c_find_close(handle: *void) -> s32`: `FindFirstFileW`, `FindNextFileW` and `FindClose`, a listing an entry at a time; the first gives `INVALID_HANDLE_VALUE`, -1, when it can't.
  - `c_get_last_error() -> u32`: `GetLastError`, the error of the last call that failed, read right after it.
  - `C_GENERIC_READ` (2147483648), `C_GENERIC_WRITE` (1073741824), `C_FILE_APPEND_DATA` (4): `CreateFileW`'s access, the last writing only at the end; `C_FILE_SHARE_READ_WRITE` (3): others may read and write the file while it is open, but not delete it; `C_CREATE_ALWAYS` (2), `C_OPEN_EXISTING` (3), `C_OPEN_ALWAYS` (4): its dispositions; `C_FILE_ATTRIBUTE_NORMAL` (128).
  - `c_create_file_w(path: *u16, access: u32, share: u32, security: *void, disposition: u32, flags: u32, template: *void) -> *void`: `CreateFileW`, a file's handle, or `INVALID_HANDLE_VALUE`, -1, when it can't open it; it opens no directory in Basic's modes.
  - `c_read_file(handle: *void, buffer: *void, count: u32, read: *u32, overlapped: *void) -> s32`, `c_write_file(handle: *void, data: *void, count: u32, written: *u32, overlapped: *void) -> s32`: `ReadFile` and `WriteFile`, not 0 when they could, with how many bytes they read (0 at the end) or wrote in `read` or `written`.
  - `c_close_handle(handle: *void) -> s32`: `CloseHandle`, not 0 when it did.
  - `c_get_file_size_ex(handle: *void, size: *s64) -> s32`: `GetFileSizeEx`, the open file's size in 64 bits, not 0 when it could.
  - `c_delete_file_w(path: *u16) -> s32`: `DeleteFileW`, not 0 when it did; 0 for a directory, and for a file open without `FILE_SHARE_DELETE`.
- Windows only, for the environment, `kernel32`'s wide call, which takes a name and gives a value as UTF-16:
  - `c_get_environment_variable_w(name: *u16, buffer: *u16, size: u32) -> u32`: `GetEnvironmentVariableW`, the value written to `buffer` followed by a zero, and the units written, the zero not counted; given too small a `size` (0 included), the units needed, the zero counted, so 1 for a variable set to `""`; 0 when the variable isn't set. A name is found whatever its case.
- Windows only, for the commands, `kernel32`'s calls that start the shell, with a command line as UTF-16, and make the pipe a captured output comes through, read with `c_read_file`:
  - `C_Security_Attributes`: `SECURITY_ATTRIBUTES`, 24 bytes: `length: u32`, `padding: u32`, `descriptor: *void`, `inherit: s32` and `padding2: u32`.
  - `C_Startup_Info`: `STARTUPINFOW`, 104 bytes: `size: u32`, `padding: u32`, `reserved: *void`, `desktop: *u16`, `title: *u16`, `x`, `y`, `x_size`, `y_size`, `x_count_chars`, `y_count_chars`, `fill_attribute` and `flags` (`u32`s), `show_window: u16`, `reserved2_count: u16`, `padding2: u32`, `reserved2: *u8`, and `standard_input`, `standard_output` and `standard_error` (`*void`s, at offsets 80, 88 and 96).
  - `C_Process_Information`: `PROCESS_INFORMATION`, 24 bytes: `process: *void`, `thread: *void`, `process_id: u32` and `thread_id: u32`.
  - `C_STARTF_USESTDHANDLES` (256): the new process starts with the three handles `C_Startup_Info` holds; `C_HANDLE_FLAG_INHERIT` (1); `C_INFINITE` (4294967295), a wait that ends only when the process does; `C_STD_INPUT_HANDLE` (4294967286), `C_STD_OUTPUT_HANDLE` (4294967285), `C_STD_ERROR_HANDLE` (4294967284): -10, -11 and -12 as a `u32`.
  - `c_create_pipe(read: **void, write: **void, security: *C_Security_Attributes, size: u32) -> s32`: `CreatePipe`, a pipe's two ends, not 0 when it could.
  - `c_set_handle_information(handle: *void, mask: u32, flags: u32) -> s32`: `SetHandleInformation`, which Basic uses to make the pipe's end it keeps not inheritable.
  - `c_get_std_handle(which: u32) -> *void`: `GetStdHandle`, the program's standard input, output or error handle.
  - `c_create_process_w(application: *u16, command_line: *u16, process_security: *void, thread_security: *void, inherit: s32, flags: u32, environment: *void, directory: *u16, startup: *C_Startup_Info, information: *C_Process_Information) -> s32`: `CreateProcessW`, which starts a process running `command_line`, a buffer the call may change, not a literal; not 0 when it could, with `information` filled.
  - `c_wait_for_single_object(handle: *void, milliseconds: u32) -> u32`: `WaitForSingleObject`, 0 when the process ended.
  - `c_get_exit_code_process(handle: *void, code: *u32) -> s32`: `GetExitCodeProcess`, the process's exit code, 32 bits with no sign, so `exit -1` is 4294967295.
- Linux only, for the directories, POSIX's calls, which take a path as bytes, and `errno`, which the directories, `print_error_string` and `run_command_with_output` read:
  - `C_Stat`: glibc's x86-64 `struct stat`, 144 bytes: `device: u64`, `inode: u64`, `links: u64`, `mode: u32` (`st_mode`, at offset 24) and `rest: [116]u8`.
  - `C_S_IFMT` (61440) and `C_S_IFDIR` (16384), `st_mode`'s kind bits and a directory's; `C_DIRENT_NAME_OFFSET` (19), where `d_name` is in glibc's x86-64 `struct dirent`.
  - `c_stat(path: *u8, buffer: *C_Stat) -> s32`: `stat`, 0 when it could, following a symbolic link; `libc.so.6` exports it from glibc 2.33 on.
  - `c_mkdir(path: *u8, mode: u32) -> s32`, `c_rmdir(path: *u8) -> s32`: `mkdir` and `rmdir`, 0 when they did.
  - `c_opendir(path: *u8) -> *void`, `c_readdir(directory: *void) -> *u8`, `c_closedir(directory: *void) -> s32`: `opendir`, `readdir` and `closedir`, a listing an entry at a time; `readdir` gives the entry's address, or null at the end and on an error, which only `errno` tells apart.
  - `c_errno_location() -> *s32`: `__errno_location`, the address of the thread's `errno`, which glibc's `<errno.h>` reads through it.
  - `C_EINTR` (4): `errno` when a signal stopped a call before it did anything, as a handler installed without `SA_RESTART` does; `print_error_string`'s writes and `run_command_with_output`'s reads make such a call again.
- Linux only, for `run_command_with_output`, a command whose standard output comes through a pipe, read from its file descriptor:
  - `c_popen(command: *u8, mode: *u8) -> *void`: `popen`, which runs a command through sh with its standard output into a pipe, and gives the pipe's end to read as a `FILE *`, or null. It takes only `r`, `w` and `e` in its mode, so Basic opens it `"r"`; Linux's pipes have no text mode.
  - `c_pclose(stream: *void) -> s32`: `pclose`, which waits for the command to end and gives its status as `c_system` does.
  - `c_fileno(stream: *void) -> s32`: `fileno`, the file descriptor under the pipe's `FILE *`.
  - `c_read(descriptor: s32, buffer: *void, count: u64) -> s64`: `read`, which reads up to `count` bytes from a file descriptor, giving how many it read, which may be fewer, 0 at the end, or -1.

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

`PI` and `TAU` are Rig's own constants, and the procedures after them are C's `<math.h>`, through `libc.rig`, each procedure only the call, so `-0.0`, the infinities and NaN go to C and come back as C gives them. The results are the C library's, so the last bit can differ between systems, and on Windows between the compiled and the interpreted program. A domain error gives NaN (`sqrt_float64(-1)`), a pole an infinity (`log_float64(0)` is `-inf`), and C's `errno` isn't exposed. A NaN's sign is the C library's too, and `print_float` writes any NaN as `nan`.

- `min_int`, `min_u64`, `min_float32`, `min_float64` `(a: T, b: T) -> T`: the smaller of `a` and `b`; `a` when they're equal.
- `max_int`, `max_u64`, `max_float32`, `max_float64` `(a: T, b: T) -> T`: the larger of `a` and `b`; `a` when they're equal.
- `clamp_int`, `clamp_u64`, `clamp_float32`, `clamp_float64` `(value: T, low: T, high: T) -> T`: `low` for a value below it, `high` for one above it, otherwise `value`.
- `abs_int(value: int) -> int`: the magnitude; the most negative `int` is itself, since its negation wraps.
- `abs_float32(value: float32) -> float32`, `abs_float64(value: float64) -> float64`: the value with its sign bit cleared, so `-0.0` gives `0.0` and NaN stays NaN.
- `PI`, `TAU`: π, 3.141592653589793, and 2π, 6.283185307179586; untyped float constants (spec 3.12), so each takes either float type, and `x := PI;` makes a `float32`.
- `sqrt_float64`, `sqrt_float32` `(x: T) -> T`: the square root; `-0.0` for `-0.0`, NaN below 0.
- `cbrt_float64`, `cbrt_float32` `(x: T) -> T`: the cube root, negative for a negative `x`.
- `sin_float64`, `sin_float32` `(x: T) -> T`: the sine of an angle in radians; NaN for an infinity.
- `cos_float64`, `cos_float32` `(x: T) -> T`: the cosine of an angle in radians; NaN for an infinity.
- `tan_float64`, `tan_float32` `(x: T) -> T`: the tangent of an angle in radians; NaN for an infinity.
- `asin_float64`, `asin_float32`, `acos_float64`, `acos_float32` `(x: T) -> T`: the angle whose sine or cosine is `x`, in radians, `-π/2` to `π/2` and 0 to π; NaN outside -1 to 1.
- `atan_float64`, `atan_float32` `(x: T) -> T`: the angle whose tangent is `x`, in radians, `-π/2` to `π/2`.
- `atan2_float64`, `atan2_float32` `(y: T, x: T) -> T`: the angle of the point (`x`, `y`) from the positive x axis, `-π` to `π`, in the point's quadrant, `y` first as in C; on the axes the zeros' signs choose: `(0, -0.0)` is π and `(-0.0, -0.0)` is `-π`.
- `exp_float64`, `exp_float32`, `exp2_float64`, `exp2_float32` `(x: T) -> T`: e and 2 to the power `x`; 0 for `-inf`.
- `log_float64`, `log_float32`, `log2_float64`, `log2_float32`, `log10_float64`, `log10_float32` `(x: T) -> T`: the logarithm to base e, 2 and 10; `-inf` for 0, NaN below 0.
- `pow_float64`, `pow_float32` `(base: T, exponent: T) -> T`: `base` to the power `exponent`; NaN for a finite negative base and a finite exponent that isn't whole; 1 for an exponent of 0 or a base of 1, even with a NaN; `inf` or `-inf` for 0 or `-0.0` to a negative odd power.
- `hypot_float64`, `hypot_float32` `(x: T, y: T) -> T`: the square root of `x² + y²`, without overflow or underflow on the way; `inf` when either is infinite, even with a NaN.
- `floor_float64`, `floor_float32`, `ceil_float64`, `ceil_float32` `(x: T) -> T`: the nearest whole number below and above; the sign of `-0.0` kept, and `ceil` of a value between -1 and 0 is `-0.0`.
- `round_float64`, `round_float32` `(x: T) -> T`: the nearest whole number, halves away from zero, as C's `round` (2.5 is 3, -2.5 is -3, -0.4 is `-0.0`).
- `trunc_float64`, `trunc_float32` `(x: T) -> T`: toward zero.
- `mod_float64`, `mod_float32` `(a: T, b: T) -> T`: C's `fmod`, the remainder of `a / b` with `a`'s sign (`mod_float64(-5.5, 2)` is -1.5); NaN for a `b` of 0 or an infinite `a`, and `a` for an infinite `b`.

## Memory (`memory.rig`)

C's `memmove`, `memset` and `memcmp`, written as byte loops in Rig rather than called from C. Memory is passed as `*void`, so any pointer converts to it. A count of 0 or less does nothing and reads nothing, `int`'s most negative value included.

- `copy_memory(destination: *void, source: *void, count: int)`: copies `count` bytes from `source` to `destination`; the two may overlap, and the bytes copied are the ones `source` held before the copy.
- `set_memory(destination: *void, value: u8, count: int)`: sets each of the `count` bytes that start at `destination` to `value`.
- `compare_memory(a: *void, b: *void, count: int) -> int`: -1, 0 or 1 as the first of `count` bytes that differs is smaller or larger in `a` than in `b`, the bytes compared unsigned; 0 when none differs.

## Numbers (`numbers.rig`)

Numbers are written into a buffer the caller gives, and the text comes back as a string viewing the buffer's first bytes, so nothing allocates. A buffer of `INT_TEXT_MAX` or `FLOAT_TEXT_MAX` bytes is always enough; one too short stops at the language's bounds check (unless built with `--no-bounds-checks`). Parsing takes the whole string or nothing: no spaces, prefixes or `_`.

- `INT_TEXT_MAX`: 65, the most bytes `format_int` writes (64 binary digits and a sign); `format_u64` writes 64 at most.
- `FLOAT_TEXT_MAX`: 320, the most bytes `format_float` writes (a sign, 309 digits, a point and 9 decimals).
- `FLOAT64_LARGEST`: `1.7976931348623157e308`, `float64`'s largest finite value.
- `digit_value(c: u8) -> int`: 0-9 for `0`-`9`, 10-35 for `a`-`z` and `A`-`Z`, -1 for any other byte.
- `format_int(buffer: []u8, value: int, base: int) -> string`: writes `value` in a base from 2 to 36, lowercase, with `-` before a negative value; another base writes nothing and gives `""`.
- `format_u64(buffer: []u8, value: u64, base: int) -> string`: writes `value` as `format_int` does, for a `u64`, which may be above `int`'s largest; another base writes nothing and gives `""`.
- `format_u64_after(buffer: []u8, count: int, value: u64, base: int) -> string`: writes `value` as `format_u64` does, after the `count` bytes already in `buffer`, and gives the text written, those bytes included; `format_int`'s digits, after its `-` or not. Another base writes nothing and gives `""`.
- `parse_int(s: string, base: int) -> int, bool`: reads an optional `+` or `-` and one digit or more of the base, either case; `0, false` for anything else, a value outside `int` or a base outside 2 to 36.
- `format_float(buffer: []u8, value: float64, decimals: int) -> string`: writes `value` with 0 to 9 decimals (others are clamped), the last rounded half away from zero on the `float64`'s own value, though a value within about a unit in the last place of a tie can round the other way (`1.805` with 2 decimals is `1.81`); `nan`, `inf`, `-inf`, and `-` before any negative value, `-0.0` included. From 1e18 up, 18 leading digits are written, of which only about the first 15 or 16 are the value's, and the rest are zeros.
- `format_float_word(buffer: []u8, count: int, word: string) -> string`: writes `word` after the `count` bytes already in `buffer` and gives the text written; `format_float`'s `nan` and `inf`.
- `parse_float(s: string) -> float64, bool`: reads `[+-] digits [. digits] [(e|E) [+-] digits]`, with a digit before or after the point; `0, false` for anything else. A value too large is an infinity, and one too small is zero. Correctly rounded when the significant digits fit in 2^53 and the power of ten is within ±22, otherwise within a few units in the last place.

## Strings (`string.rig`)

The procedures that cut strings up and search them, from `strings_equal` to `split_from_left`, and `contains` and `count_of`, never allocate: every string they return is a view into the string given. The ones that copy, `to_c_string`, `join`, `replace`, `repeat`, `to_upper_copy` and `to_lower_copy`, take their memory from the allocator they are given, the context's unless another is, and the caller gives it back there. The last five always copy, even when nothing changed, so the caller can always free `result.data` with `free(result.data, allocator)`; each works out its size first and allocates once, and an empty result is the zeroed string and allocates nothing, as `builder_to_string`'s does. An allocator with no memory, or a size past `int`'s largest, gives the zeroed string too. Indexes and counts are in bytes (but `utf8_count`'s, which counts code points), and bytes are compared unsigned. Only the three `utf8_` procedures know UTF-8, reading and writing its sequences as the Unicode Standard has them (Table 3-7: no overlong forms, no surrogates, nothing past U+10FFFF); every other string procedure works on bytes. A zeroed string (count 0, data null) is the empty string everywhere, and no byte is read through its null data.

- `strings_equal(a: string, b: string) -> bool`: `a == b`, as a procedure a procedure value can name.
- `compare_strings(a: string, b: string) -> int`: -1, 0 or 1 by the first byte that differs, compared unsigned; a prefix sorts first. It takes the strings; `compare_string`, for `binary_search`, takes pointers to them.
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
- `to_c_string(s: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> *u8`: a C string, `s.count + 1` bytes from `allocator` holding the bytes of `s` and a zero, which the caller frees with `free(p, allocator)`; null when the allocator has none. A zeroed string gives a lone zero. A zero inside `s` is copied as it is, so C sees the string end there.
- `contains(s: string, sub: string) -> bool`: whether `sub` occurs in `s`; every string contains `""`.
- `count_of(s: string, sub: string) -> int`: how many times `sub` occurs in `s`, counted from the left without overlap (`"aa"` in `"aaaa"` is 2); `""` matches nothing, so 0.
- `join(parts: []string, separator: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string`: the parts with `separator` between each two, in memory from `allocator`, which the caller frees with `free(result.data, allocator)`; empty parts keep their separators, and no parts give `""`.
- `replace(s: string, old: string, new: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string`: a copy of `s` with each `old` replaced by `new`, from the left without overlap, what replaces one never searched again; an empty `old` matches nothing, so the result is a copy of `s`. The caller frees `result.data`.
- `repeat(s: string, n: int, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string`: `s` `n` times over; `""` for an `n` of 0 or less. The caller frees `result.data`.
- `to_upper_copy(s: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string`, `to_lower_copy(s: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string`: a copy of `s` with its ASCII letters changed as `to_upper` and `to_lower` change them, every other byte as it is. The caller frees `result.data`.
- `UTF8_REPLACEMENT`: U+FFFD, 65533, the replacement character.
- `UTF8_MAX_BYTES`: 4, the most bytes a code point takes, so a buffer of that many is always enough for `utf8_encode`.
- `utf8_decode(s: string, index: int) -> int, int, bool`: the code point whose sequence starts at byte `index`, its byte count, and whether it is well-formed; `UTF8_REPLACEMENT`, 1 and `false` for a byte that doesn't start a well-formed sequence, one cut short by the string's end included, so a loop moving on by the count goes on at the next byte; 0, 0 and `false`, reading nothing, for an index outside `s`.
- `utf8_count(s: string) -> int, bool`: how many code points `s` holds, each byte of an ill-formed sequence counted as one, and whether all of `s` is well-formed; `""` is 0 and `true`.
- `utf8_encode(code_point: int, buffer: []u8) -> int`: writes the code point's sequence at the start of `buffer` and gives its byte count, 1 to 4; a negative value, a surrogate (U+D800 to U+DFFF) or one past U+10FFFF writes `UTF8_REPLACEMENT`'s 3 bytes. A buffer shorter than the sequence stops at the language's bounds check (unless built with `--no-bounds-checks`).

## Allocators (`allocators.rig`)

Whatever needs memory takes an allocator, as Jai has it: an `Allocator` holds a procedure and the data it works on, and the procedure is called with a mode, `ALLOCATE`, `RESIZE`, `FREE` or `FREE_ALL`. Each procedure of Basic's that takes one, the allocators' helpers and initializers included, but `table_make_room`, the tables' own, which they always give their table's, and `memory_debugger_gives_back_all`, which asks about an allocator rather than allocating from it, and `wide.rig`'s `to_wide_string` and `from_wide_string`, whose caller always chooses it (Basic's own give the heap's), has it last, or just before `loc` in one that takes a place too (below), defaulting to the context's, `context.allocator` (spec 7.10), so memory comes from the context's allocator unless one is given; a default value is worked out in the caller (spec 7.2), so it is the caller's context. Calls that give one work as before. Memory goes back to the allocator that gave it: freeing with the current context's allocator what another gave, because the context changed in between, is the program's mistake, as in Jai, and no check catches it, though the memory debugger (below) reports it in the memory it watches. So `free(p)` is right only where `p` came from the context's allocator, the same one; a builder, a table, an arena and a pool keep the allocator they were made with, and give their memory back to it whatever the context is by then, as a resizable array keeps the one it first grew from (spec 3.17). `Allocator`, the modes and the heap allocator, `heap_allocator_proc` and `HEAP_ALLOCATOR`, are Preload's, which every program imports without asking, Basic or not: [`../Preload/README.md`](../Preload/README.md) lists them. Basic adds the helpers, an arena, a pool, and the temporary storage's own helpers, `talloc`, its marks and its reset (Temporary storage, below). Its allocators, `arena_allocator_proc` and `pool_allocator_proc`, and the memory debugger's `memory_debugger_allocator_proc` (below), are each of an allocator procedure's type, `(mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void`, as the heap's is. A program calls the four helpers rather than an allocator's procedure. They settle the edges, so an allocator is only ever asked for more than 0 bytes and only ever given memory that isn't null to resize or free. An allocator that can't give memory returns null. The heap's memory is aligned as C's `malloc` aligns it, and an arena's or a pool's to 8 bytes when its parent's is, as the heap's is.

The helpers also tell the allocator where it is asked from (issue #194). Each takes `loc: Source_Code_Location = #caller_location` last, the place of the call, the callee's name in it, unless one is given (spec 7.2; Preload's `Source_Code_Location`, [`../Preload/README.md`](../Preload/README.md)). While it calls the allocator, it puts `loc` in `context.allocation_location`, and puts the one before back once the allocator returns (spec 7.10), so an allocator procedure, whose shape has no parameter for it, reads the place from the context: a memory debugger records it with each allocation. A direct call of an allocator's procedure leaves the field as it is, so the allocator reads whatever the context holds: zeroed, unless the program set it. An arena and a pool ask their parent for blocks and slots, and give them back, with the place they are asked from, so the parent sees the request that needed them; `arena_reset` gives its blocks back at its own line.

Every other procedure of Basic's that allocates or frees through the allocator it is given, or the context's, but those the next paragraph names, takes `loc` too, last, after its allocator when it has one, and passes it down to the helper it calls, so the place an allocator sees is the program's own call, not a line of Basic's: the appends, `builder_to_string` and `builder_free`; `print_to_builder` and `sprint`, whose `loc` comes after their `..Any` arguments, so only its default ever fills it (spec 7.2); the string copies, `to_c_string`, `join`, `replace`, `repeat`, `to_upper_copy` and `to_lower_copy`; `wide.rig`'s conversions, `to_wide_string` and `from_wide_string`; `path_join`, `read_entire_file`, `list_directory`, `get_environment`, `run_command`, `run_command_with_output` and `sort`; and the tables' sets, `table_remove` and frees (`table_make_room` takes the place its set passes, with no default). Left out, `loc` is the caller's place; given, it is passed on as it is. A procedure of the program's own that allocates for its caller can take one the same way and pass it on, so the place is its caller's line rather than its own:

```rig
// A copy of s with a line feed after it, which the caller frees.
with_line_feed :: (s: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string {
    parts := string.[s, "\n"];
    return join(parts, "", allocator, loc);
}
```

The procedures under `print_to_builder` and `sprint` that write into a builder they are given, `print_formatted`, `print_item`, `print_quoted` and `print_or_append`, take none: each appends through `print_or_append`'s `append_string`, so the builder's allocator sees that line of `print.rig`. `talloc` and `tprint`, whose memory is the temporary storage's, given back all at once, take none either, nor do the storage's reset and marks and `arena_reset`, which give back pages and blocks at their own lines. Nor do the heap's short-lived copies, which are freed before the call returns: the file procedures' copies of a path for the system, UTF-16 on Windows, taken at a line of `wide.rig` (`to_wide_path`'s), and ended by a zero on Linux (`file_open`'s, and so those of `read_entire_file`, whose contents' memory has the caller's `loc`, `write_entire_file`, `append_to_file` and `file_exists`, which open through it, and `delete_file`'s), the directory procedures' copies of a path for the system (UTF-16 on Windows, ended by a zero on Linux, and `list_directory`'s pattern on Windows), `list_directory`'s builder of the names it gathers and the scratch it gives `sort`, `get_environment`'s UTF-16 buffer for the value on Windows, `run_command`'s and `run_command_with_output`'s builder of the command line and copy of the shell's path on Windows, `run_command_with_output`'s builder of the output it gathers, and `print_to_builder`'s, `sprint`'s and `print_error`'s scratch builders. Nor does the memory debugger's own memory, which is the heap's whatever it wraps: its records, which its procedure grows and `memory_debugger_free` gives back, and `memory_debugger_report`'s sorted copy and the scratch it gives `sort`. A failing `assert` builds its line in a builder of the heap's, at its own lines, not its `loc`'s, which is the line's place, and never gives it back: the program, or the `#run`, ends there. The heap sees lines of `wide.rig`, `file.rig`, `directory.rig`, `environment.rig`, `print.rig`, `assert.rig` and `memory_debugger.rig`.

- `alloc(size: int, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> *void`: `size` bytes, or null; null for a size of 0 or less, without asking the allocator.
- `resize(memory: *void, old_size: int, new_size: int, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> *void`: `memory` made `new_size` bytes long, perhaps moved, its first bytes kept; null when the allocator can't, and `memory` is as it was. Resizing null is `alloc`, and resizing to 0 or less is `free` and gives null, each with the same `loc`.
- `free(memory: *void, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location)`: gives `memory` back; freeing null does nothing.
- `free_all(allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location)`: gives back everything the allocator gave, for an arena, a pool or `TEMPORARY_ALLOCATOR` (whose `FREE_ALL` is `reset_temporary_storage`'s reset); the heap's does nothing.
- `Arena_Block`: an arena block's header, `previous: *Arena_Block` and `size: int`, 16 bytes, with its `size` bytes after it.
- `Arena`: `parent: Allocator`, `block_size: int`, `block: *Arena_Block` (the newest) and `used: int` (its bytes handed out).
- `arena_init(arena: *Arena, block_size: int, parent: Allocator = context.allocator)`: an empty arena whose blocks of `block_size` bytes (4096 for 0 or less) come from `parent`.
- `arena_allocator_proc`, `arena_allocator(arena: *Arena) -> Allocator`: allocations one after another in the newest block, each at a multiple of 8, and a new block of `block_size` bytes, or the allocation's size when larger, when one doesn't fit. A resize allocates anew and copies, and a free does nothing; `free_all` gives every block back.
- `arena_reset(arena: *Arena)`: gives back every block but the newest, and hands that one out again from its start.
- `Pool_Slot`: a free slot, `next: *Pool_Slot`, held in the slot's own first bytes.
- `Pool`: `parent: Allocator`, `slot_size: int`, `slot_count: int`, `memory: *u8` and `free_list: *Pool_Slot`.
- `pool_init(pool: *Pool, slot_size: int, slot_count: int, parent: Allocator = context.allocator)`: an empty pool of `slot_count` slots of `slot_size` bytes, raised to at least 8 and up to a multiple of 8, whose memory comes from `parent` at the first allocation.
- `pool_allocator_proc`, `pool_allocator(pool: *Pool) -> Allocator`: a free slot for up to `slot_size` bytes, in address order at first, and null for more or when none is free. A resize within a slot keeps it, and one past it gives null; a free puts the slot back, to be handed out next, and `free_all` gives the memory back to `parent`, after which the pool can be used again.

A stretch of code takes its memory from another allocator in either of two ways: `push_context` with a copy of the context whose allocator is the other one (spec 6.13), or `context.allocator` set and put back by a `defer`. Either way every call that leaves its allocator out, in the stretch and in what it calls, takes its memory from there, without being passed it. The push is undone when its block is left; the assignment changes the context that is current, which the caller shares, so the `defer` puts it back:

```rig
arena : Arena;
arena_init(*arena, 4096);
defer free_all(arena_allocator(*arena));

with_arena := context;
with_arena.allocator = arena_allocator(*arena);
push_context with_arena {
    names := join(string.["a", "b"], ", ");    // from the arena
}

{
    old := context.allocator;
    context.allocator = arena_allocator(*arena);
    defer context.allocator = old;
    twice := repeat("ab", 2);    // from the arena
}
```

### Temporary storage

The context's temporary storage (Preload's, spec 7.10; [`../Preload/README.md`](../Preload/README.md) has its rules) hands out memory one allocation after another, each a multiple of 8 bytes into its region (so aligned to 8 in Preload's storage), and takes it all back at once: nothing is given back one allocation at a time. So taking memory from it is quick, and nothing has to be freed, which suits memory that is done with by a point the program knows: a frame's end, a request's, a command's. The program resets the storage there; until then, everything it gave stays. Its first 32 KB lie in the program, and past them it asks its `overflow_allocator`, the heap's in Preload's storage, for pages, which a reset gives back to it. `TEMPORARY_ALLOCATOR` is it as an allocator, to give a procedure that takes one; `tprint` (`print.rig`) formats into it.

Memory from it used after a reset, or after going back to a mark taken before it was allocated, is a memory error no check catches (spec 8.6): the bytes are handed out again, or their page is back with `overflow_allocator`. A mark whose page the storage no longer has is wrong too: one of another storage, one taken before a reset since, or one taken after a mark the storage has gone back to since. Going back to it is a memory error no check catches: when no page made since has the gone page's address, every page goes back and the program stops at a null pointer dereference, but the heap often hands that address out again, and then the walk stops there silently.

- `talloc(size: int) -> *void`: `size` bytes from the context's temporary storage, or null, as `alloc(size, TEMPORARY_ALLOCATOR)` gives them.
- `reset_temporary_storage()`: empties the context's temporary storage: its pages go back to its `overflow_allocator`, and its first block is handed out again from its start. Its `high_water_mark`, the largest `occupied` has been, stays, so a program can see how much a frame took at its most. It counts the padding that aligns allocations and the unused ends of regions a page replaced, so it is at least the most the storage held.
- `Temporary_Storage_Mark`: where the storage reached, `page: *Temporary_Storage_Page` (its newest page, null while it has none) and `occupied: int`.
- `get_temporary_storage_mark() -> Temporary_Storage_Mark`: a mark of where the context's temporary storage is now.
- `set_temporary_storage_mark(mark: Temporary_Storage_Mark)`: takes the context's temporary storage back to `mark`, giving back every page made since, newest first, so what was allocated after the mark is handed out again and what came before it stays.

```rig
frame :: (n: int) {
    mark := get_temporary_storage_mark();
    defer set_temporary_storage_mark(mark);
    title := tprint("frame {}", n);    // gone at the end of frame
    print_line(title);
}

main :: () {
    for 1..3 { frame(it); }
    names := tprint("{} and {}", "Ada", "Grace");
    print_line(names);
    reset_temporary_storage();          // names is gone
}
```

## String builder (`builder.rig`)

Text put together a piece at a time, in memory of the builder's own from the allocator it is given, the context's unless another is, and copied out as one string. `print_to_builder` (`print.rig`) appends formatted text to one. When an append doesn't fit, the capacity becomes the larger of 64 and twice the old one, doubled again until it fits, and the memory moves with `resize`. Numbers are written on the stack first, so the appends allocate nothing but the builder's memory (`print_to_builder` also takes some of the heap's for a moment). An allocator that has no memory to give stops the program at the next write, with the language's `null pointer dereference`.

- `String_Builder`: `allocator: Allocator`, `data: *u8`, `count: int` (the bytes built) and `capacity: int` (the bytes `data` has room for).
- `builder_init(builder: *String_Builder, allocator: Allocator = context.allocator)`: an empty builder whose memory comes from `allocator`, taken at the first append that needs it.
- `append_string(builder: *String_Builder, s: string, loc: Source_Code_Location = #caller_location)`: appends the bytes of `s`; an empty or zeroed string appends nothing and takes no memory. `s` may be a view of the builder's own text: when the append grows the builder, the bytes are copied from where they moved to.
- `append_byte(builder: *String_Builder, byte: u8, loc: Source_Code_Location = #caller_location)`: appends the byte itself, not its number.
- `append_int(builder: *String_Builder, value: int, base: int, loc: Source_Code_Location = #caller_location)`: appends `value` as `format_int` writes it.
- `append_u64(builder: *String_Builder, value: u64, base: int, loc: Source_Code_Location = #caller_location)`: appends `value` as `format_u64` writes it.
- `append_float(builder: *String_Builder, value: float64, decimals: int, loc: Source_Code_Location = #caller_location)`: appends `value` as `format_float` writes it.
- `append_bool(builder: *String_Builder, value: bool, loc: Source_Code_Location = #caller_location)`: appends `true` or `false`.
- `builder_to_string(builder: *String_Builder, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string`: a copy of the text in memory from `allocator`, which the caller gives back with `free(s.data, allocator)`; an empty builder gives the zeroed string (count 0, data null) and allocates nothing.
- `builder_reset(builder: *String_Builder)`: empties the builder, keeping its memory.
- `builder_free(builder: *String_Builder, loc: Source_Code_Location = #caller_location)`: gives the memory back; the builder is empty, with no memory, and keeps its allocator.

## Bounds checks (`array.rig`)

Basic's bounds check, which the hash tables use to stop a program given an entry index outside their entries with the language's own message. Basic's untyped growable array is gone: resizable arrays are the language's, `[..]T` (spec 3.17), grown and shrunk by its built-in procedures.

- `check_index(index: int, count: int)`: stops the program with the language's `index I is out of bounds (count C)` unless `index` is at least 0 and less than `count`, at its line in `array.rig`. Built with `--no-bounds-checks`, it checks nothing.

## Hash tables (`table.rig`)

Values found by their key, a string (`Table`) or an int (`Int_Table`). A table is untyped: it knows only its value's size, copies values in by that many bytes, and hands out a value's address as a `*void` for the caller to cast, valid until the table's next set or free: a set may rehash the table, which moves every value even when the capacity stays the same. A table's memory comes from its allocator, the one `table_init` or `int_table_init` is given, the context's unless another is; a string key is copied into memory from it when it is set, and given back when it is removed.

The entries are one run of memory, a power of two of them, each a header (the key's 64-bit FNV-1a hash, the entry's state and the key) and then the value, at a multiple of 8, so every value is aligned to 8 when the allocator's memory is, as the heap's is. Keys are found by open addressing with linear probing; a removed key's entry is marked removed, so keys further along its chain are still found, and a new key takes the first removed entry its probe passed. Before a set, when one more key would leave more than three quarters of the entries used or removed, the table is rehashed: into 16 entries the first time, twice as many when the keys would fill more than half, and otherwise as many, which only clears the removed entries. A table with no entries, before its first set or after it is freed, is found in without touching any. An allocator that has no memory to give stops the program at the next write, with the language's `null pointer dereference`.

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
- `table_make_room(entries: *u8, capacity: int, count: int, removed: int, entry_size: int, allocator: Allocator, loc: Source_Code_Location) -> *u8, int, int`: the growing both kinds share; the entries, capacity and removed count, rehashed when one more key needs it, the new entries asked for with `loc`, the place of the set. The old entries are the caller's to give back.
- `table_same_key(entry: *u8, key: *void) -> bool`, `int_table_same_key(entry: *u8, key: *void) -> bool`: whether an entry holds the key at `key`, for `table_probe`.
- `table_init(table: *Table, value_size: int, allocator: Allocator = context.allocator)`, `int_table_init(table: *Int_Table, value_size: int, allocator: Allocator = context.allocator)`: an empty table of values of `value_size` bytes, whose memory comes from `allocator`, taken at the first set. A negative `value_size` is taken as 0.
- `table_set(table: *Table, key: string, value: *void, loc: Source_Code_Location = #caller_location) -> *void`, `int_table_set(table: *Int_Table, key: int, value: *void, loc: Source_Code_Location = #caller_location) -> *void`: sets `key`'s value to a copy of the bytes at `value`, or zeroed bytes for null, adding the key if it isn't there, and gives the value's address. `value` may be one of the table's own values.
- `table_find(table: Table, key: string) -> *void`, `int_table_find(table: Int_Table, key: int) -> *void`: the address of `key`'s value, or null.
- `table_remove(table: *Table, key: string, loc: Source_Code_Location = #caller_location) -> bool`, `int_table_remove(table: *Int_Table, key: int) -> bool`: removes `key`, giving a string key's copy back, and says whether it was there; an int key has no copy, so `int_table_remove` frees nothing and takes no `loc`.
- `table_entry(table: Table, index: int) -> bool, string, *void`, `int_table_entry(table: Int_Table, index: int) -> bool, int, *void`: entry `index`, from 0 to the capacity less one: whether it holds a key, the key and the value's address; false, the zeroed string or 0, and null for one that holds none. Any other index stops the program with the language's own bounds check, through `check_index` (unless built with `--no-bounds-checks`).
- `table_free(table: *Table, loc: Source_Code_Location = #caller_location)`, `int_table_free(table: *Int_Table, loc: Source_Code_Location = #caller_location)`: gives every key's copy and the entries back; the table is empty, with no entries, and keeps its value size and allocator.

## Sorting and searching (`sort.rig`)

Elements of any type, given as a pointer to the first, a count and each element's size in bytes, as C's `qsort` and `bsearch` take them, and compared by a procedure value given pointers to two of them: `less_int` and its neighbours, or one of the program's own. The sort is a merge sort, and stable: elements that compare equal keep the order they had, so sorting by one key and then by another sorts by the second and, within it, by the first. It takes at most about n log2 n comparisons whatever the order the elements come in, and n for elements already sorted. Its scratch memory, half the elements' bytes, comes from the allocator it is given, the context's unless another is, and goes back before it returns; up to `SORT_INSERTION_MAX` elements are sorted by insertion without asking. When the allocator has no memory, the elements are sorted by insertion in place instead, about n^2 / 2 comparisons and swaps, stable all the same: sorting never fails. Elements are moved a byte at a time, and a comparison may be given a pointer to an element's copy in the scratch memory, so it must compare what the pointers point at, not the addresses.

- `SORT_INSERTION_MAX`: 4, the most elements sorted by insertion, without merging or memory.
- `sort_by_insertion(data: *void, count: int, element_size: int, less: (a: *void, b: *void) -> bool)`: sorts in place, stable, with no memory, swapping neighbours a byte at a time; for a few elements, and `sort`'s fallback. Nothing is compared or touched for a `count` of 1 or less or an `element_size` of 0 or less, so `data` may be null then.
- `sort_by_merging(data: *void, scratch: *void, count: int, element_size: int, less: (a: *void, b: *void) -> bool)`: sorts in place, stable, with `scratch` holding at least `count / 2` elements; what `sort` does once it has the scratch. Nothing is compared or touched for a `count` of 1 or less or an `element_size` of 0 or less, so `data` and `scratch` may be null then.
- `sort(data: *void, count: int, element_size: int, less: (a: *void, b: *void) -> bool, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location)`: sorts the elements in place so that none is less than one before it, stable. `less` says whether the element at its first pointer goes before the one at its second, and must be a strict weak order. Nothing is asked for, compared or touched for a `count` of 1 or less or an `element_size` of 0 or less, so `data` may be null then.
- `binary_search(data: *void, count: int, element_size: int, key: *void, compare: (key: *void, element: *void) -> int) -> int, bool`: in sorted elements, the first index whose element isn't below the key, as C++'s `lower_bound` gives it, and whether that element equals the key: the key's first occurrence and `true`, or where it would go and `false`, `count` and `false` past the last. `compare` is given the key first and gives a negative number, 0 or a positive one as the key goes before the element, equals it or goes after; the key may be of another type than the elements. No elements, or an `element_size` of 0 or less, give 0 and `false` without calling `compare`, so `data` may be null then.
- `less_int(a: *void, b: *void) -> bool`, `less_float64(a: *void, b: *void) -> bool`, `less_string(a: *void, b: *void) -> bool`: whether the `int`, `float64` or `string` at `a` goes before the one at `b`, for `sort`. NaN goes after every number and equals another NaN, so floats with NaNs still sort; strings go by `compare_strings`.
- `compare_int(key: *void, element: *void) -> int`, `compare_string(key: *void, element: *void) -> int`: -1, 0 or 1 as the `int` or `string` at `key` goes before, equals or goes after the one at `element`, for `binary_search`. `compare_string` takes pointers to two strings, where `compare_strings` takes the strings themselves.

## Paths (`path.rig`)

A path is a plain string, and its parts are views into it, cut out by `slice` and `advance`: nothing is copied, nothing asks the system whether a file is there, and no part reaches outside the path, so a zeroed path gives zeroed parts and a view of part of a string is read only as far as it goes. A missing extension or file name is an empty view at the path's end, and a missing directory is one at its start. Both `/` and `\` separate a path's parts, on every system, and a join writes `/`, which both systems accept. Nothing else is special: a drive letter is part of the first name (`"C:\x"`'s directory is `"C:"`), and `\\server\share` is a path from the root whose first separator is doubled.

- `is_path_separator(c: u8) -> bool`: `/` or `\`.
- `path_directory(path: string) -> string`: what is before the last separator, without the separators that end it (`"a//b"` gives `"a"`, `"a/b/"` gives `"a/b"`); the first separator when only separators are left, the root (`"/b"` gives `"/"`); `""` with no separator.
- `path_file_name(path: string) -> string`: what is after the last separator, all of `path` with none; `""` when `path` ends with one.
- `path_extension(path: string) -> string`: the file name's extension with its dot, from its last dot that isn't one of the dots the name starts with: `".gz"` for `"c.tar.gz"`, `"."` for `"b."`, none for `".bashrc"` or `".."`; none is an empty view at the path's end.
- `path_without_extension(path: string) -> string`: `path` without `path_extension(path)`.
- `path_join(a: string, b: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string`: `a`, a `/`, then `b`, in memory from `allocator`, which the caller frees with `free(s.data, allocator)`. The `/` is left out when either is empty, `a` ends with a separator or `b` starts with one; an absolute `b` is joined all the same. Always a copy, allocated once; two empty paths give the zeroed string (count 0, data null) and allocate nothing, and an allocator with no memory gives the zeroed string too.

## UTF-16 on Windows (`wide.rig`)

Windows' own calls, `kernel32`'s wide ones, take a path or a name as UTF-16, where every Rig string is UTF-8, so Basic's files, directories, environment and commands convert a path, a name or a command line before handing it to them, and convert what they give back. C's calls on Windows (`fopen`, `remove`, `system` and the rest) read a path or a command in the system's ANSI code page instead, so one with a character outside it names another file, or none; C's wide-character calls (`_wfopen`, `_wremove`) would take it right, but interpreted they are a second C runtime's (spec 10.8), whose `FILE *` the compiler's own copy of C refuses. `kernel32`'s calls are the same functions compiled and interpreted. Each copy comes from the allocator the caller gives, at `loc`, the place of the call unless given, and the caller gives it back; Basic's file and directory procedures give the heap's and free it as soon as the call that takes it returns, and `get_environment`, `run_command` and `run_command_with_output` give the allocator their caller gives, as for their C copy of a name or a command on Linux. Every procedure here is Windows only; on Linux `wide.rig` declares nothing.

- `to_wide_string(s: string, allocator: Allocator, loc: Source_Code_Location = #caller_location) -> *u16`: `s` in UTF-16, followed by a zero unit, in one allocation from `allocator`, which the caller frees with `free(wide, allocator)`; `""` is a lone zero unit. Null, with nothing allocated, for a string with a zero byte (which the system would read as a shorter one), one that isn't valid UTF-8 (which would have to change to be converted, and so would name something else), one longer than 2^31 - 1 bytes (a C `int`'s count), and when `allocator` has no memory.
- `to_wide_path(path: string) -> *u16`: `to_wide_string(path, HEAP_ALLOCATOR)`, which the caller frees with `free(wide, HEAP_ALLOCATOR)`, the copy the file procedures give `kernel32`; null for an empty path too.
- `from_wide_string(wide: *u16, units: int, allocator: Allocator, loc: Source_Code_Location = #caller_location) -> string, bool`: `units` UTF-16 units, with no zero after them needed, as UTF-8, in one allocation of the exact size from `allocator`, which the caller frees with `free(s.data, allocator)`, and `true`; an unpaired surrogate, which Windows allows, becomes U+FFFD. 0 units are the zeroed string and `true`, allocating nothing; fewer than 0, more than 2^31 - 1, a conversion that fails and no memory are the zeroed string and `false`, with nothing left allocated.

## Files (`file.rig`)

Files through `kernel32`'s wide calls on Windows (`CreateFileW`, `ReadFile`, `WriteFile`, `GetFileSizeEx`, `CloseHandle` and `DeleteFileW`) and C's stdio on Linux. A path is UTF-8 and names the file it says on both systems: Windows' calls take it as UTF-16, so it is converted first (`to_wide_path`, above), and a file named outside the system's ANSI code page, `café` or `日本`, is made, read, found and deleted by its UTF-8 name, as a directory is; a path that isn't valid UTF-8 (which would have to change to be converted) is refused there. Linux's C library takes a path as plain bytes, UTF-8 or not. The bytes read are the bytes written on every system: Windows' calls have no text mode, and Linux's files open in binary mode (`"rb"`, `"wb"`, `"ab"`), where C's text mode on Windows would turn a line feed into a carriage return and a line feed, and stop reading at Ctrl-Z. A directory is not a file, on either system: it isn't found, read, written or deleted (Linux's `remove` would delete an empty one). Windows' `CreateFileW` opens none, but Linux's C library opens one to read, so there Basic's `file_open`, once C has opened a file to read, reads a byte from it first, and a directory's read fails. A file's size is 64 bits on both systems, `GetFileSizeEx`'s on Windows and the position at its end from `fgetpos` on Linux, so a file past 2 GiB is measured right. A path goes to the system as a copy, UTF-16 on Windows and ended by a zero on Linux, from the heap, given back once the system is done with it: not from the context's allocator (`read_entire_file`'s is for the file's contents), which may be an arena's or the temporary storage's, where giving the copy back frees nothing, so memory a program never resets would grow with each call; a path with a zero byte would reach the system as a shorter name, another file's, so it is refused without calling it.

- `File`: `handle: *void`, `kernel32`'s file handle on Windows and C's `FILE *` on Linux, null when the file isn't open; a zeroed `File` isn't open.
- `FILE_READ`, `FILE_WRITE`, `FILE_APPEND`: `file_open`'s modes, 0 to 2: reading from the start, writing from empty, and writing at the end, the last two making the file when it isn't there.
- `file_open(path: string, mode: int) -> File, bool`: the open file and `true`; a `File` that isn't open and `false` for another mode, a path with a zero byte, an empty path, on Windows one that isn't valid UTF-8, a file the system can't open, and a directory. On Windows others may read and write the file while it is open, as C's `fopen` lets them there, but not delete it.
- `file_close(file: File)`: closes the file; one that isn't open is left alone.
- `file_read(file: File, buffer: []u8) -> int`: reads up to `buffer.count` bytes from where the file is, and gives how many; 0 at the end, on an error, or for a `File` that isn't open. On Windows one read asks for at most 1 GiB, so a larger buffer takes another read for the rest.
- `file_write(file: File, data: string) -> bool`: whether every byte of `data` was written; `""` writes nothing and is `true`, and a `File` that isn't open is `false`. On Windows the bytes go to the system at once, in pieces of at most 1 GiB. On Linux they may stay in C's buffer until the file is closed, and `file_close` doesn't say whether writing them out failed (a full disk, say); `write_entire_file` and `append_to_file` do.
- `file_size(file: File) -> int`: the file's size in bytes, its position left where it was; -1 when the system can't tell, or for a `File` that isn't open.
- `read_entire_file(path: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string, bool`: the whole file in memory from `allocator`, which the caller frees with `free(s.data, allocator)`, and `true`. An empty file is the zeroed string and `true`, allocating nothing; a missing file, a directory, a path `file_open` refuses, a file whose size the system can't tell, a short read or no memory is the zeroed string and `false`, with nothing left allocated.
- `write_entire_file(path: string, data: string) -> bool`: makes or empties the file and writes `data`; `true` when every byte was written and the file closed without an error; `false` for a path `file_open` refuses.
- `append_to_file(path: string, data: string) -> bool`: writes `data` at the end of the file, making it when it isn't there; `true` and `false` as for `write_entire_file`.
- `file_exists(path: string) -> bool`: whether there is a file at `path` that opens for reading; a directory isn't one, and a path `file_open` refuses is `false`.
- `delete_file(path: string) -> bool`: deletes a file that `file_exists` finds; `false` for a missing file, a directory, a path `file_open` refuses, or when the system can't, as Windows can't while the file is open without `FILE_SHARE_DELETE`, as Basic's and C's files are.

## Directories (`directory.rig`)

Directories through each system's own calls (issue #157), since C17 has none: `kernel32`'s wide calls on Windows (`GetFileAttributesW`, `CreateDirectoryW`, `RemoveDirectoryW`, `FindFirstFileW` and `FindNextFileW`), and POSIX's on Linux (`stat`, `mkdir`, `rmdir`, `opendir` and `readdir`). Windows' take a path as UTF-16, so the UTF-8 path is converted first (`to_wide_string`, above) and each name listed is converted back: a directory named outside the system's ANSI code page, `café` or `日本`, is made, found, listed and deleted by its UTF-8 name, as on Linux, whose calls take a path as bytes, and so is a file in it (Files, above). Every procedure refuses, with nothing allocated and nothing asked of the file system, an empty path, a path with a zero byte (which would reach the system cut short, another directory's name), and on Windows a path that isn't valid UTF-8 (which would have to change to be converted). The path's copy for the system, UTF-16 on Windows and ended by a zero on Linux, comes from the heap and is given back before the call returns, as the file procedures' copies are.

A listing holds every entry's name but `.` and `..`, files and directories alike, without the directory's path, sorted by bytes (`less_string`), so it comes in the same order on both systems and on every file system, whose own orders differ. It is one allocation: the views, then the names' bytes, each followed by a zero byte its count doesn't include, so a name's `data` can go to C as it is.

- `is_directory(path: string) -> bool`: whether there is a directory at `path`; a symbolic link to one is one, on both systems, and on Windows so is a directory's link or junction whose target is gone, which can't be listed (on Linux it isn't one). `false` for a file, a missing path, and the paths every procedure refuses.
- `make_directory(path: string) -> bool`: makes one directory, whose parent must be there; `false` when anything, a directory or a file, is already at `path`, when the parent is missing, and for the paths every procedure refuses. On Linux its permissions are `0o777` less the umask.
- `delete_directory(path: string) -> bool`: deletes an empty directory; `false` for one with entries, a file, a missing path, and the paths every procedure refuses. Given a symbolic link (or, on Windows, a junction) to a directory, Windows deletes the link itself, whatever its target holds, and leaves the target as it is; Linux's `rmdir` doesn't follow the link and gives `false`.
- `list_directory(path: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> []string, bool`: the names in the directory, sorted by bytes, in one allocation from `allocator`, which the caller frees with `free(names.data, allocator)`, and `true`; the names are gathered in a builder on the heap first, and sorted with the heap's scratch, both given back before it returns. An empty directory is the zeroed view and `true`, allocating nothing; a file, a missing path, an error while listing, no memory from `allocator`, and the paths every procedure refuses are the zeroed view and `false`, with nothing left allocated. A trailing separator lists the same directory, and on Windows a drive letter and a colon alone, `C:`, lists that drive's current directory, as the other procedures take it, not its root. On Windows an unpaired surrogate in a name, which Windows allows, becomes U+FFFD, so that name doesn't reach its entry again, and two such names may list alike; on Linux a name is its bytes as the system keeps them.
- `gather_directory_name(gathered: *String_Builder, name: string) -> bool`: `list_directory`'s step for each entry, on both systems: appends `name` and a zero byte after it to `gathered` and gives `true`, or appends nothing and gives `false` for `.` and `..`.

## Time (`time.rig`)

A time is a `Time`, an `int` of nanoseconds since 1970-01-01T00:00:00Z, negative before it, so every time is from 1677-09-21T00:12:43.145224192Z to 2262-04-11T23:47:16.854775807Z, and a difference of two is an `int` of nanoseconds too. The clock is C17's `timespec_get` with `TIME_UTC`, the wall clock, in nanoseconds on both systems. C17 has no monotonic clock, so a timer measures by the wall clock too: when the system's clock is set back while a timer runs, it gives less than the time that passed, even a negative time, and nothing clamps it. The calendar is UTC's, the proleptic Gregorian calendar of ISO 8601, with no time zones and no leap seconds (every day has 86400 seconds, as POSIX counts them), and weeks start on Sunday, 0, as C's `tm_wday` has them. `sleep_milliseconds` sleeps by each system's own call, `kernel32`'s `Sleep` on Windows and `nanosleep` on Linux, and not by C17's `thrd_sleep` (`<threads.h>`, optional): it is on both systems, but MSVC's `<threads.h>` defines it only inline, over the Visual C++ runtime's `_thrd_sleep64`, which the compiler doesn't link it to and the interpreter doesn't find.

- `NANOSECONDS_PER_SECOND`: 1000000000.
- `TIME_TEXT_MAX`: 20, the bytes `format_time` writes for every time `to_calendar` gives, `YYYY-MM-DDTHH:MM:SSZ`.
- `Time`: `nanoseconds: int`, since 1970-01-01T00:00:00Z; a zeroed `Time` is that instant.
- `Timer`: `start: Time`, when it was started.
- `Calendar`: `year: int`, `month: int` (1 to 12), `day: int` (1 to 31), `hour: int` (0 to 23), `minute: int`, `second: int` (0 to 59), `nanosecond: int` (0 to 999999999) and `weekday: int` (0 for Sunday to 6), in UTC.
- `time_now() -> Time`: the time now, by the wall clock; a zeroed `Time` when C can't give it.
- `timer_start() -> Timer`: a timer started now.
- `timer_elapsed_ns(timer: Timer) -> int`: the nanoseconds since the timer was started, by the wall clock.
- `timer_elapsed_seconds(timer: Timer) -> float64`: the same in seconds, the nanoseconds over 10^9.
- `to_calendar(time: Time) -> Calendar`: the time's fields in UTC, every field in its range for every `Time`, `int`'s extremes included (the years 1677 and 2262).
- `format_time(calendar: Calendar, buffer: []u8) -> string`: writes the calendar as ISO 8601, `YYYY-MM-DDTHH:MM:SSZ`, at the buffer's start and gives the text written, without the nanosecond or the weekday. Each field is written as `format_int` writes it, with zeros on its left to 4 digits for the year and 2 for the rest; one that needs more digits, or is negative, is written whole, `-` first and no zeros; when that is wider than the field's width, the text is longer than `TIME_TEXT_MAX`, and a buffer of that many bytes stops at the language's bounds check (unless built with `--no-bounds-checks`).
- `sleep_milliseconds(ms: int)`: sleeps for `ms` milliseconds: on Linux at least that long, and the system may wake the program later; on Windows about that long, give or take a clock tick of about 15.6 ms, since `Sleep` may wake it up to a tick early. An `ms` of 0 or less returns at once. On Windows a time past `Sleep`'s 2^31 - 1 ms is slept in pieces, so its `INFINITE` is never passed; on Linux a sleep a signal cuts short goes on for the time left.

## Random numbers (`random.rig`)

PCG32, Melissa O'Neill's permuted congruential generator (https://www.pcg-random.org), as her pcg-c-basic writes it: a 64-bit linear congruential step, and a permutation of the old state (a xorshift, then a rotation by its top bits) that gives 32 bits. The same seed and stream give the same numbers on every system, compiled and interpreted, and they are the reference's: `tests/run/basic_random.rig` reproduces the first round `pcg32-demo` prints. It is for simulations, games, tests and shuffles, not for keys, tokens or anything that must not be guessed: a few outputs give the state away. A zeroed `Random` is stream 0 from state 0, the same numbers in every program, so seed it first.

- `PCG_MULTIPLIER`: 6364136223846793005, the multiplier of the linear congruential step.
- `Random`: `state: u64` and `increment: u64`, the step's increment, odd once seeded; `random_u32` steps with its lowest bit set (it doesn't change the field), so a zeroed one draws as stream 0.
- `random_seed(r: *Random, seed: u64, stream: u64)`: seeds `r` as `pcg32_srandom_r` does; generators of different streams give different numbers from the same seed. The stream's top bit is unused.
- `random_u32(r: *Random) -> u32`: the next 32-bit number, as `pcg32_random_r` gives it.
- `random_u64(r: *Random) -> u64`: two 32-bit numbers, the first the high half.
- `random_int(r: *Random, low: int, high: int) -> int`: a number from `low` to `high`, both included, every one equally likely, with the bounds either way round. Draws that would favour some numbers are drawn again (`pcg32_boundedrand_r`'s rejection) rather than taken modulo the range, and it draws at least once, even for one value. Any range works, `int`'s whole range included.
- `random_float64(r: *Random) -> float64`: a number from 0 up to but not including 1, the top 53 bits of `random_u64` over 2^53.
- `random_bool(r: *Random) -> bool`: the lowest bit of `random_u32`.
- `random_shuffle(r: *Random, data: *void, count: int, element_size: int)`: puts the `count` elements of `element_size` bytes at `data` in a random order (Fisher-Yates, swapping a byte at a time, so it needs no memory); nothing is drawn for a `count` of 1 or less or an `element_size` of 0 or less.
- `random_from_time() -> Random`: a generator seeded from `time_now()`, its stream the address of a local. The seed differs from run to run, not from call to call: two made within one tick of the clock (100 ns on Windows) get the same seed, and from the same caller the same stream, so they give the same numbers, as two made one after the other often do on Windows. For several generators, seed one from the clock and the others from its numbers, or give each its own stream.

## The environment (`environment.rig`)

A variable of the environment the program was started with, through `kernel32`'s `GetEnvironmentVariableW` on Windows and C's `getenv` on Linux, a command run through the system's shell, through `kernel32`'s `CreateProcessW` on Windows and C's `system` on Linux, or with its standard output captured, through a pipe on Windows and `popen` on Linux, and the command-line arguments the program was started with, which Preload saves before `main`'s first line. A name or a command goes to the system as a copy, UTF-16 on Windows (`to_wide_string`, above) and ended by a zero on Linux, from the allocator it comes with, the context's unless another is, given back once the system is done with it; one with a zero byte would reach the system cut short, so it is refused without calling it. A variable's name and value, and a command, are UTF-8 on both systems: Linux's C library takes and gives them as bytes, and Windows' wide calls take the name and the command and give the value in UTF-16, converted from and to UTF-8, where C's `getenv`, `system` and `_popen` would use the system's ANSI code page, so `日本` is read, and a file named `日本.txt` found by a command, there too; a name or a command that isn't valid UTF-8 (which would have to change to be converted) is refused on Windows. What a command writes is its own, though: cmd's `echo` writes its text in the console's code page, not in UTF-8, where its `type` writes a file's bytes as they are. The systems differ, and Basic doesn't hide it: Windows' variable names are case-insensitive (`"Path"` finds `PATH`) and Linux's aren't; a command runs in cmd.exe on Windows and in sh on Linux, each with its own syntax; and its status means something else on each (`run_command`). What the program printed before a command comes out before what the command prints: every C stream is flushed before it runs.

- `get_environment(name: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string, bool`: a copy of the variable's value in memory from `allocator`, which the caller frees with `free(value.data, allocator)`, and `true`; on Linux copied at once, since C may change its own copy, and on Windows written by the system into a buffer Basic gives and converted from there. A variable set to `""` is the zeroed string (count 0, data null) and `true`, allocating nothing. A variable that isn't set, no memory, an empty name, one with `=` or a zero byte, and on Windows one that isn't valid UTF-8 are the zeroed string and `false`, with nothing left allocated. On Windows the value is read in UTF-16 into a buffer of the heap's, given back before it returns, and converted into one allocation from `allocator`, an unpaired surrogate in it, which Windows allows, becoming U+FFFD; its name's copy is given back before the value's is asked for, as on Linux.
- `run_command(command: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> int`: runs `command` through the shell, waits for it, and gives its status as C's `system` gives it: on Windows the command's exit code (`exit 3` gives 3) as a signed 32-bit number (`exit -1` gives -1), and 1 when cmd.exe can't find the command; on Linux a wait status, the exit code times 256 when the command ended by itself (`exit 3` gives 768) and 32512 (127 times 256) when sh can't find the command. 0 is success on both. An empty command runs the shell with nothing to do, which gives 0: on Linux it reaches C as a lone zero, never as the null that asks whether there is a shell. -1, with nothing run, for a command with a zero byte, on Windows one that isn't valid UTF-8, and no memory for its copy. -1 is also what it gives when the shell can't be started, and on Windows what a command that exits with -1 gives, so -1 alone doesn't say the command never ran. On Windows the shell is the one `%COMSPEC%` names, or `cmd.exe` when it is unset or empty, as for C's `system`, started by `CreateProcessW` with the command line `"SHELL" /c COMMAND` in UTF-16 and the program's standard input, output and error, so a command means what it meant through `system`; the command line is built on the heap, and the shell's path read into it, both given back before it returns.
- `run_command_with_output(command: string, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string, int`: runs `command` through the shell as `run_command` does, waits for it, and gives what it wrote to its standard output, in one allocation from `allocator` which the caller frees with `free(output.data, allocator)`, and its status as `run_command` gives it. The output is its bytes as the command wrote them, so cmd's `echo hello` gives `hello\r\n` and sh's gives `hello\n`. No output is the zeroed string, allocating nothing. Only standard output is captured: the command's standard error is the program's, as with `run_command` (write `2>&1` in the command to capture both), and so is its standard input. The zeroed string and -1, with nothing run, for every command `run_command` refuses, no memory for its copy, and when the shell can't be started; when there is no memory for the output, the command has still run, so the output is lost and its status still given. The output is gathered on the heap while it is read, then copied out once.
- `get_command_line_arguments() -> []string`: the program's command-line arguments (spec 7.4), the name it was started by first, then each argument as given, in UTF-8 on Windows: Windows' are converted from its wide (UTF-16) ones, a compiled program's by Preload and an interpreted one's by interpret mode, so, like the environment's variables, they don't depend on the ANSI code page, and Linux's are the bytes the system gave. The view is empty, not even a name, when the program was started with no `argv[0]` (`argc` 0, which Linux before 5.18 allowed), so check its `count` before reading the name. Each string is followed by a zero byte its count doesn't include, so its `data` can go to C as it is. It's the view `main`'s prologue saved into Preload's `__command_line_arguments`, the same one every call, its elements and bytes the program's for the whole run and never freed. A `#run` gets the empty view, not even a name: nothing is saved while compiling (spec 7.8).

## Print (`print.rig`)

What is written to standard output goes through C's `putchar`, so it comes out in order with what a program writes with C's `printf`, compiled and interpreted. `print_error` and `print_error_string` write to standard error instead, as a runtime error is written: C's streams flushed, then `c_write` on descriptor 2, so what was printed before comes out first, even when both streams go to one place. Bytes are written as they are, so UTF-8 text prints as UTF-8. A value of any type is written through its type info (`Any`, spec 3.16). `print_or_append`, `print_quoted`, `print_item` and `print_formatted` take a `builder` and append to it instead, or write to standard output when it is null: Rig has no closures or generics, so a pointer that may be null lets one walk write to either. `print_to_builder` stops on a null one. Written to standard output, nothing is allocated.

- `print_string(s: string)`: writes the bytes of `s`; an empty or zeroed string writes nothing.
- `print_byte(byte: u8)`: writes the byte itself, not its number.
- `print_bool(value: bool)`: writes `true` or `false`.
- `print_int(value: int)`: writes `value` in base 10.
- `print_float(value: float64, decimals: int)`: writes `value` with `decimals` digits after the point, as `format_float` does.
- `print_line(s: string)`: writes `s` and a line feed.
- `print_or_append(builder: *String_Builder, s: string)`: writes the bytes of `s` to `builder`, or to standard output when it is null.
- `print_quoted(builder: *String_Builder, s: string)`: writes `s` in double quotes, with the escapes below.
- `print_item(builder: *String_Builder, item: Any, nested: bool)`: writes the value `item` holds, as below; `nested` says it is inside a struct, an array, a view, a resizable array or `{!EXTRA: ...}`, where a string is quoted.
- `print_formatted(builder: *String_Builder, format: string, args: []Any)`: writes `format` with each `{}` replaced by the next of `args`, as below. `print` calls it with a null builder, and `print_to_builder`, `sprint` and `tprint` with a builder of their own.
- `print(format: string, args: ..Any)`: writes `format` to standard output with its `{}`s replaced by the arguments, in order: `print("{} is {}\n", name, age)`. A `[]Any` already made is forwarded with `..`, as in `print(format, ..args)`; passed without it, it is one argument, a view.
- `print_to_builder(builder: *String_Builder, format: string, args: ..Any, loc: Source_Code_Location = #caller_location)`: appends what `print` would write to `builder`, and writes nothing to standard output. A null `builder` stops the program with `null pointer dereference`, whatever the format. The format and the arguments may view the builder's own text, as `append_string`'s `s` may: the text is written into a builder of its own first, on the heap whatever `builder`'s allocator, and then appended at once, and the heap's memory is freed before it returns. So an arena's builder keeps nothing more, and a pool's needs no second slot. Its `loc`, after the `..Any` arguments, is always the caller's place (Allocators, above), which `builder`'s allocator sees.
- `sprint(format: string, args: ..Any, loc: Source_Code_Location = #caller_location) -> string`: what `print` would write, as a string in memory from `context.allocator`, which the caller gives back with `free(s.data)`: `sprint("{}-{}", "a", 7)` is `"a-7"`. It is written on the heap first, as `print_to_builder`'s text is, and copied out at once, so the context's allocator gives one allocation, the text's size; nothing written is the zeroed string (count 0, data null) and allocates nothing. Its `loc`, after the `..Any` arguments, is always the caller's place (Allocators, above), which the context's allocator sees.
- `tprint(format: string, args: ..Any) -> string`: what `print` would write, as a string in the context's temporary storage, gone at its next reset, or when it goes back to a mark taken before the `tprint` (Temporary storage, above): `print_line(tprint("{} + {} = {}", 1, 2, 3))`. It is written into a builder on the temporary storage and returned with no copy; nothing written is the zeroed string and allocates nothing.
- `print_error_string(s: string)`: writes the bytes of `s` to standard error, after flushing C's streams, so what was printed to standard output before comes out first. The bytes go in calls of at most 2^30 bytes, each going on from where the last stopped; when one writes nothing or fails, the rest is dropped, as there's nowhere left to say so. An empty or zeroed string writes nothing.
- `print_error(format: string, args: ..Any)`: writes to standard error what `print` would write to standard output: `print_error("can't read {}\n", path)`. The text is formatted into a builder of its own on the heap, then written by `print_error_string` whole, so a line goes out in one call; the heap's memory is freed before it returns. It never allocates from `context.allocator`, so it takes no `loc`.

The format, read left to right:

- `{}` takes the next argument and writes it; a string argument is its bytes, braces included, which are no format.
- `{{` writes `{`, and `}}` writes `}`. A `}` on its own is written as it is, so `}}}` is `}}`, and `{{}` is `{` and then a lone `}`.
- Any other `{` begins a placeholder this version doesn't know, as `{x}` or `{:x}`: the bytes from it to the next `}`, or to the end of the format when there's none, are written as `{!BAD}` and take no argument. So `{a{b}` is one `{!BAD}`, and format options are free to mean something later.
- A `{}` with no argument left writes `{!MISSING}`.
- Arguments left over are written after the text as ` {!EXTRA: 4, "x"}`, each as inside a struct, so a string is quoted, with `, ` between them.

The format is a value the compiler doesn't read, as C's isn't (spec 7.6), so a count that doesn't match writes a marker and never stops the program. What can't convert to `Any` (spec 3.16) can't be an argument: `print("{}\n", .["a", "b"])` is refused, since the literal's type isn't known. `print` works in a `#run` too, its output the compiler's (spec 7.8, 10.8).

How each value is written:

- **Integers:** in base 10, by their type's sign, `u64` across its whole range; a `u8` is a number, not a byte.
- **Floats:** a `float32` is widened to `float64`; then 6 decimals, as `format_float` writes them, with the trailing zeros removed down to one: `1.5`, `0.1`, `3.0`, `-0.0`, `100.0`; `nan`, `inf`, `-inf`. So a value below 0.0000005 in magnitude is `0.0` (or `-0.0`), and from 1e18 up only about the first 15 or 16 digits are the value's (`format_float`).
- **`bool`:** `true` or `false`.
- **Strings:** at the top, their bytes, as `print_string` writes them. Nested, in double quotes with Rig's escapes (spec 2.10: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`), any other byte below 32, and 127, as `\x` and two lowercase hex digits, as Rig's messages write C names (spec 7.6), and bytes from 128 as they are, so UTF-8 stays readable: `{name = "Ada\n"}`. A string with null data and a count above 0 stops the program with `null pointer dereference` where its first byte is read, after its opening `"` when it is nested.
- **Pointers:** `null`, or the address in lowercase hexadecimal after `0x`, unpadded. A pointer isn't followed, and a `*u8` is a pointer, not a C string.
- **Procedure values:** as pointers. Interpreted, a Rig procedure's address is the interpreter's stand-in (spec 10.8), so its text differs from the compiled program's, as every address does from run to run.
- **Structs:** `{x = 1, y = 2}`, fields in order, named; nested, `{from = {x = 1, y = 2}, to = {x = 3, y = 4}}`; a struct with no fields `{}`.
- **Arrays and views:** `[1, 2, 3]`; empty, or a view with a count of 0 or less, `[]`. A view with null data and a count above 0 stops the program with `null pointer dereference` before any of it is written, as indexing it would (spec 8.4). A resizable array `[..]T` (spec 3.17) is written as its view is: its `count` elements, `[]` for a zeroed one.
- **`Any`**, as a field or an element: the value it holds, written as that field or element would be. A zeroed `Any`, whose type is null, as a zeroed struct's `Any` field is, writes `null`.
- **`void`:** a description of `void`, which only an `Any` made by hand can hold, writes `void`.
- **Any other tag:** a description whose tag isn't one of the nine writes nothing.
- **Cycles:** a pointer isn't followed, but a view's data (a resizable array's too) and an `Any`'s `value_pointer` are, so a view or an `Any` that reaches a value it is part of makes a cycle: a struct whose view field views an array holding it (`nodes[0].kids = nodes;`), the struct an `Any` is a field of (`h.a = h;`), the array it is an element of (`arr[0] = arr;`), or anything that contains it deeper down. That one is written by recursing until the stack runs out, as recursion that never ends does: interpreted, `stack overflow`; compiled, the program crashes, and what C still buffers of its output is lost. It is the program's mistake, and isn't caught.

## Assertions (`assert.rig`)

A condition the program holds to be true, checked as it runs (issue #157). A false one stops the program with a runtime error (spec 8.1) at the place of the `assert`'s call, its caller's (`#caller_location`, spec 7.2): what was printed comes out first, then this line on standard error, and the exit status is 101, compiled and interpreted.

```text
main.rig:7:5: runtime error: assertion failed: n is positive
```

It ends through Preload's `__runtime_error` ([`../Preload/README.md`](../Preload/README.md)), not C's `exit`, so a failing `assert` in a `#run`'s code is an error at the `#run`, `#run failed: runtime error: assertion failed: MESSAGE` with the calls that led to it, as a failing check there is, and the build goes on to report the rest (spec 7.8); `exit` would end rig itself there. No deferred statement runs, as after any runtime error (spec 6.12). An optimised build checks every `assert` too: nothing turns them off.

- `assert(condition: bool, message: string = "", loc: Source_Code_Location = #caller_location)`: does nothing when `condition` is true, and allocates nothing. When it is false, it stops the program with the message `assertion failed`, followed by `: ` and `message` when `message` isn't empty: `assert(n > 0, "n is positive")`. `message` is written as it is, a `{}` in it included: it is no format, since a parameter with a default can't come before a `..` one. The line names `loc`'s place, or none when `loc`'s line is 0 or less, no place (a zeroed one's). It is built on the heap, never on `context.allocator`, and isn't given back: the program, or the `#run`, ends there.

## Memory debugger (`memory_debugger.rig`)

An allocator that wraps another and records every allocation it gives, so a program learns what it never freed: each allocation's size, its number, and the place it was asked for at (issue #159). A program checks itself by pushing the debugger's allocator in the context once, in `main` (spec 6.13), so that everything that takes its memory from the context's allocator, Basic's procedures and the program's own, takes it through the debugger, which passes each call on to the allocator it wraps. At the end, `memory_debugger_report` writes what isn't freed, and says whether all was well:

```rig
exit :: (status: s32) #foreign libc;

greeting :: (name: string) -> string {
    return join(string.["hello, ", name], "");
}

run :: () {
    a := greeting("Ada");
    print_line(a);
    free(a.data);
    g := greeting("Grace");      // never freed
    print_line(g);
}

main :: () {
    debugger : Memory_Debugger;
    memory_debugger_init(*debugger);                // wraps main's allocator, the heap
    debugged := context;
    debugged.allocator = memory_debugger_allocator(*debugger);
    push_context debugged {
        run();
    }
    clean := memory_debugger_report(*debugger);
    memory_debugger_free(*debugger);
    if !clean { exit(1); }
}
```

As `tests/run/memory_debugger_example.rig`, which imports Basic first, it prints this, and ends with exit status 1:

```text
hello, Ada
hello, Grace
tests/run/memory_debugger_example.rig:10:12: memory debugger: allocation 2 (12 bytes) isn't freed
memory debugger: 2 allocations, 1 not freed (12 bytes), 0 bad calls
```

Setting `context.allocator` works too, with `defer`s for the report and the free, which then run after the program's own code, the allocator put back first since `defer`s run last to first:

```rig
debugger : Memory_Debugger;
memory_debugger_init(*debugger);
defer memory_debugger_free(*debugger);
defer memory_debugger_report(*debugger);
old := context.allocator;
context.allocator = memory_debugger_allocator(*debugger);
defer context.allocator = old;
```

In a `#run`, push it rather than set `context.allocator`: every `#run` shares the compile-time world's default context (spec 7.8), so an assignment would stay for the runs after it, while a push is undone at the end of its block. Its memory there is the compile-time world's heap. An expression `#run` also runs in `rig check`, as often as an editor checks the program (spec 7.8), so one that reports gives the debugger a builder in `output` and hands back the text, as `tests/run/memory_debugger_run.rig` bakes its report as a string; a top-level `#run`, which only `rig build` runs, may print it.

The debugger keeps a record of each allocation, by its address: its number, from 1, in the order the debugger gave them; its size; whether it was given back; and the place it was first asked for at. A resize that the wrapped allocator does is the same allocation, its number and place kept, at the address it has now, so the report's sizes are the sizes now. A free marks the record freed.

The place is `context.allocation_location` when the debugger is called (Allocators, above): Basic's helpers set it to their caller's place, the built-in procedures of a resizable array to their own call's, and Basic's other procedures that allocate pass down the place of the program's call to them. So a leak names the program's line, not one of Basic's. A procedure of the program's own that allocates names its own line, the call inside it, unless it takes `loc: Source_Code_Location = #caller_location` and passes it on as Basic's do (`with_line_feed`, above): the example's `greeting` doesn't, so its leak names `greeting`'s `join`, line 4 above and line 10 of the test, not `run`'s call of it; `greeting :: (name: string, loc: Source_Code_Location = #caller_location) -> string`, passing `loc` to `join`, would name `run`'s line. The procedures that take no `loc` (Allocators, above) give a line of Basic's, whose file is named by its absolute path, as messages name a module's file (spec 8.1). A direct call of the debugger's procedure, from no helper, has whatever place the context holds: none, unless the program set the field. The debugger passes each call on by a direct call of the wrapped allocator's procedure, which leaves the place as it is, so an arena or a pool it wraps passes the program's place on to its parent.

The report writes a line for each allocation not freed, in number order, at its place, and then a line that counts them, which has no place. One of `tests/run/memory_debugger_leaks.rig`'s reports, six of its lines left out at the `...`:

```text
tests/run/memory_debugger_leaks.rig:69:10: memory debugger: allocation 1 (64 bytes) isn't freed
...
memory debugger: allocation 25 (8 bytes) isn't freed
memory debugger: 25 allocations, 8 not freed (151 bytes), 0 bad calls
```

A place is written first, as `PATH:LINE:COL: `, as a diagnostic or a runtime error starts (spec 8.1), and left out for none; a count of 1 takes the singular. The report is for the end of the program, after its own code, or for any point before as a checkpoint: it may be called any number of times, and the numbering goes on. It says whether every allocation is freed and no bad call was made; what to do with that, such as the exit status, is the program's choice.

A free or a resize of memory the debugger never gave out, or gave and has taken back, and a resize to more than 0 bytes whose `old_size` isn't the size the allocation has, are bad calls. Each is written as it happens, at the call's place, and counted, and the program goes on; the call isn't passed on to the wrapped allocator. A bad free does nothing. A bad resize gives null, as an allocator that can't resize does, and leaves the memory as it was: an allocation given the wrong `old_size` is still the caller's to free, and its own free is a good one; one already freed stays freed; and memory the debugger never gave out is the allocator's that gave it. Memory the debugger never gave out is named by `N`, the number the next allocation will have; memory it gave by its allocation's number, `K`, and size, `S`; `O` is the `old_size` the resize was given:

```text
PATH:LINE:COL: memory debugger: free of memory it never gave out, before allocation N
PATH:LINE:COL: memory debugger: resize of memory it never gave out, before allocation N
PATH:LINE:COL: memory debugger: free of allocation K (S bytes), which is already freed
PATH:LINE:COL: memory debugger: resize of allocation K (S bytes), which is already freed
PATH:LINE:COL: memory debugger: resize of allocation K (S bytes) given old_size O
```

Memory another allocator gave, freed or resized through the debugger because the context's allocator changed in between, is memory it never gave out: the program's mistake (spec 7.10), which the debugger refuses. An address the wrapped allocator hands out again after it was freed is the new allocation's, since the debugger knows memory only by its address: a second free of the old allocation then frees the new one, and the new one's own free is the bad call. A pool, whose freed slot is the next it hands out, does that at once. The address an allocation had before a resize moved it is taken back too: a free or a resize there names that allocation, at the size it had there, as already freed, though the allocation lives on at its new address and the report lists it there. The exception is a wrapped allocator that hands the old address out again while it resizes, to an allocation it makes through the context, so through the debugger: that allocation is then the address's, and live.

The place is only the innermost call that passed one down: for an allocation `sprint` makes in `make_name`, in `tests/debug/break_at_allocation.rig`, the line of `make_name`'s `sprint` call, not `main`'s call of `make_name`. A debugger attached shows every caller. Set `break_at` to an allocation's number, from a report, and run the program again under gdb (`break memory_debugger_break`, `run`, `bt`) or cdb (`bp PROGRAM!memory_debugger_break`, `g`, `k`): the debugger calls `memory_debugger_break`, which does nothing itself, when it makes that allocation, after recording it, so the stack there shows where it is made, the program's own lines among them. It calls it after writing each bad call too, so a debugger attached stops at every one. The numbers are the same from run to run, compiled and interpreted, on both systems, for a program whose allocations are, as they don't depend on addresses. A program built unoptimised, the default, keeps the call; interpreted, nothing stops. `tests/debug/break_at_allocation.rig` stops at an allocation `sprint` makes, at a second free and at a resize given the wrong `old_size`, under gdb and cdb.

`free_all` through the debugger is passed on to the allocator it wraps, and then takes every allocation the debugger gave as freed when that allocator gives back everything it gave, as the protocol says `FREE_ALL` does: an arena's, a pool's and the temporary storage's do, so the report after it lists none of theirs. The heap's does nothing, since the heap doesn't know what it gave, so a debugger over the heap keeps its records, and the report still lists each allocation not freed, as the heap still has it. A debugger over another debugger does what the one it wraps does: one over a debugger over the heap keeps its records too. An allocator of the program's own is taken at the protocol's word: one whose `FREE_ALL` gives nothing back has its allocations taken as freed all the same. What `FREE_ALL` gives back is what was given before it: an allocation the wrapped allocator makes through the context, so through the debugger when it is the context's allocator, while it frees all comes after, and stays live.

With the rest of Basic:

- **The temporary storage:** `talloc` and `tprint` take their memory from `TEMPORARY_ALLOCATOR` itself, not from the context's allocator, so a debugger in the context never sees them. A debugger that wraps `TEMPORARY_ALLOCATOR` sees each free do nothing, and `free_all` through it is the storage's reset, which takes everything as freed. A debugger that is a storage's `overflow_allocator` sees its pages, which the storage asks for and gives back by direct calls: each at the place of the call that needed one (`talloc`'s own line, in `allocators.rig`, for a `talloc`), given back at the reset's or the mark's line.
- **An arena or a pool whose parent is the debugger:** their blocks and their slots' memory are its allocations, each at the place of the request that needed it, which the arena and the pool pass on, and go back through it. A pool's memory goes back at its `free_all`'s place. An arena's `free_all` runs `arena_reset` first, which gives back every block but the newest at its own line in `allocators.rig`, as a reset on its own does, and then gives the newest back at the `free_all`'s place. What the arena or the pool hands out from them isn't the debugger's. **A debugger over an arena or a pool** sees each allocation they give, an arena's resize moving each one.
- **Nested:** a debugger may wrap another, in a push inside a push. Each records what it gave, so the inner one's allocations are the outer one's too, at the same places, as a debugger passes each call on by a direct call.
- **A resizable array** (spec 3.17) that first grows while the debugger is the context's allocator keeps the debugger as its allocator, so its growth and its `array_free` are the debugger's, at the place of the built-in's call. It must not outlive the debugger.

Memory the debugger gave goes back through it. Given back some other way, such as by the `free_all` of the arena it wraps, called on the arena and not through the debugger, its records stay, and the report says it isn't freed, until the wrapped allocator hands an address out again through the debugger, when the new allocation's record takes the old one's place, as it takes a freed record's.

It writes to standard output, through `print`, as all of Basic does, or into the string builder in its `output`, through `print_to_builder`, when that isn't null; that builder's allocator must not be the debugger. Its records are kept in an `Int_Table` on the heap, `HEAP_ALLOCATOR`, whatever it wraps, and every call of Basic's it makes that takes an allocator is given one, so the allocations it counts are the program's alone, though the context's allocator is the debugger itself while the program runs: an allocator procedure is given its caller's context (spec 7.10).

- `Memory_Debugger_Record`: `number: int`, `size: int` (its bytes now), `freed: bool` and `location: Source_Code_Location` (the place it was first asked for at, zeroed for none).
- `Memory_Debugger`: `wrapped: Allocator` (the allocator it passes calls on to), `records: Int_Table` (its records, by address), `allocations: int` (the allocations made, so the last one's number), `bad_calls: int` (the bad calls it was given), `output: *String_Builder` (where it writes, null for standard output) and `break_at: int` (the number of the allocation to call `memory_debugger_break` at, 0 for none).
- `memory_debugger_init(debugger: *Memory_Debugger, wrapped: Allocator = context.allocator)`: an empty debugger over `wrapped`, writing to standard output, breaking at no allocation. It doesn't give back records a debugger already had: `memory_debugger_free` does. A debugger can't wrap itself, which would call itself at its first allocation until the stack runs out: when `wrapped` is the debugger, or a debugger over it, followed down the debuggers each wraps, as when it is made ready again while it or one over it is the context's allocator and no allocator is given, it keeps the allocator it wraps already. So a debugger is made ready, over what it is to wrap, before it is the context's allocator.
- `memory_debugger_allocator_proc`, `memory_debugger_allocator(debugger: *Memory_Debugger) -> Allocator`: each call passed on to `wrapped`, settling the edges as the helpers do (Allocators, above), and each allocation it gives recorded; `wrapped`'s null is passed on and isn't counted. A bad call is written and counted, and isn't passed on. `FREE_ALL` takes every allocation given before it as freed when `wrapped` gives back everything it gave.
- `memory_debugger_report(debugger: *Memory_Debugger) -> bool`: writes what isn't freed and the counts, as above, and gives whether every allocation is freed and no bad call was made.
- `memory_debugger_free(debugger: *Memory_Debugger)`: gives the records back to the heap, and nothing the debugger gave, which is the wrapped allocator's; the debugger forgets every allocation, and `memory_debugger_init` makes it ready again.
- `memory_debugger_write(debugger: *Memory_Debugger, at: Source_Code_Location, format: string, args: ..Any)`: writes `at`'s place, when it has one, and then `format` with the arguments, as `print` does, where the debugger writes; every line of the debugger's goes through it.
- `memory_debugger_bad_call(debugger: *Memory_Debugger, at: Source_Code_Location, what: string, found: *Memory_Debugger_Record)`: counts a bad `"free"` or `"resize"` (`what`) and writes it at `at`: of memory the debugger never gave out when `found` is null, and otherwise of the allocation `found` is the record of, which is already freed; then calls `memory_debugger_break`.
- `memory_debugger_break(debugger: *Memory_Debugger)`: does nothing; the debugger calls it when it makes the allocation `break_at` names and after writing each bad call, so a breakpoint on it stops there with the whole stack.
- `memory_debugger_gives_back_all(allocator: Allocator) -> bool`: whether `allocator`'s `FREE_ALL` gives back everything it gave: false for the heap, the answer for its `wrapped` for a debugger, and true for any other.
- `memory_debugger_plural(n: int) -> string`: `""` for 1 and `"s"` for any other count.
- `memory_debugger_less(a: *void, b: *void) -> bool`: whether the record at `a` has a lower number than the one at `b`, for `sort`.

## Waiting on the language

What Basic would do differently once Rig has it:

- No type aliases, so the allocator's procedure type, the key comparison the tables' probe takes, and the comparisons of the sorts and `binary_search`, are written out wherever they're used, and a time is a struct of one field, `Time`, rather than an `int` of a type of its own.
- No overloading or generics, so procedures are named by type (`min_int`, `min_float64`), the tables are untyped, handing out `*void` (an array is typed, as the language's `[..]T`), sorting and searching take an element's size and compare through pointers, and there is one kind of table per kind of key, `Table` and `Int_Table`, with the same fields.
- No way for a struct to take in another's fields, as Jai's `using` does, so both kinds of table entry repeat the fields of `Table_Entry_Header`, and the code they share reads an entry's start through it.
- No enums, so a table entry's states and a file's modes are constants.
- No named arguments (spec 7.2), so a parameter after a `..` one is always its default value, and a procedure of the program's own that takes `..Any` and a `loc` can't pass its `loc` on to `sprint` or `print_to_builder`: their allocations name its own call of them (Allocators, above; #197).
