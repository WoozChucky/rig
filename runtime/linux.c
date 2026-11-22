// linux.c: Rig's runtime on Linux, what a program needs besides its own
// objects and glibc's shared libraries, libc.so.6 and libm.so.6 (spec
// 10.6): its start-up, _start, and C's three registration functions that
// glibc keeps out of libc.so.6.
//
// A C program linked by gcc or clang gets these from glibc's development
// package (libc6-dev on Debian and Ubuntu, glibc-devel on Fedora), which a
// Linux system needn't have: _start from crt1.o, and atexit, at_quick_exit
// and pthread_atfork from libc_nonshared.a, which libc.so's linker script
// links into each program statically. Rig programs linked with them until
// issue #125; now build.sh compiles this file into rig_runtime.o, beside
// rig, and rig links every program with it against libc.so.6 and
// libm.so.6 themselves, which every glibc system has (the libc6 package).
// So a user of rig needs nothing installed besides Rig.
//
// Nothing of crt1.o's else is needed: crti.o and crtn.o, the prologue and
// epilogue of the .init and .fini sections, frame what nothing of Rig's
// uses (glibc runs a program's .init_array and .fini_array itself), and
// crt1.o's _IO_stdin_used is read only by i386's old libio.
//
// Compiled freestanding, with no header and no C library of its own
// (build.sh): this is the C library's static part. The functions it calls
// are declared here by hand, from glibc's sources.

// glibc's, in libc.so.6. Each registration takes the DSO handle of the
// module registering, which glibc uses to run, or forget, a shared
// library's handlers when dlclose unloads it. A program is never unloaded,
// so null serves, where libc_nonshared.a passes __dso_handle; its handlers
// run at exit all the same.
int __cxa_atexit(void (*function)(void *), void *argument, void *dso_handle);
int __cxa_at_quick_exit(void (*function)(void *), void *dso_handle);
int __register_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso_handle);

// The program's entry point, where the kernel starts it, as glibc's own
// start.S has it: the stack holds argc, then argv's pointers and a null,
// then the environment's and a null; rdx holds a function for the dynamic
// loader's clean-up at exit. It calls glibc's __libc_start_main, which
// starts the C library, runs the program's initialisers, calls main with
// argc, argv and the environment, and exits with main's result; it never
// returns, so hlt follows, a crash should it:
//   __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
// main in rdi; argc, popped, in rsi; argv, then where rsp points, in rdx;
// init and fini null in rcx and r8 (glibc 2.34 and later runs the
// initialisers itself, from the program's dynamic section); the loader's
// function, moved from rdx, in r9; and on the stack, aligned to 16 bytes
// as a call needs, the stack's end. rbp is cleared to mark the outermost
// frame, and the CFI says rip has no caller to return to, so a debugger's
// backtrace stops here. __libc_start_main is called through the GOT, as a
// function of a shared library is.
__asm__(".text\n"
        ".globl _start\n"
        ".type _start, @function\n"
        "_start:\n"
        "    .cfi_startproc\n"
        "    .cfi_undefined %rip\n"
        "    xorl %ebp, %ebp\n"
        "    movq %rdx, %r9\n"
        "    popq %rsi\n"
        "    movq %rsp, %rdx\n"
        "    andq $~15, %rsp\n"
        // Two words pushed keep the 16-byte alignment: rax, whose value
        // doesn't matter, and the stack's end, __libc_start_main's seventh
        // argument.
        "    pushq %rax\n"
        "    pushq %rsp\n"
        "    xorl %r8d, %r8d\n"
        "    xorl %ecx, %ecx\n"
        "    leaq main(%rip), %rdi\n"
        "    call *__libc_start_main@GOTPCREL(%rip)\n"
        "    hlt\n"
        "    .cfi_endproc\n"
        ".size _start, . - _start\n");

// C's exit handlers, run the last registered first: atexit's when the
// program exits (main returning included), at_quick_exit's on quick_exit.
// glibc keeps the tables and runs them; its registration functions take a
// handler with an argument, which these handlers, taking none, ignore (as
// glibc's own atexit passes them, the x86-64 calling convention making the
// extra argument harmless).
int atexit(void (*function)(void)) {
    return __cxa_atexit((void (*)(void *))function, 0, 0);
}

int at_quick_exit(void (*function)(void)) {
    return __cxa_at_quick_exit((void (*)(void *))function, 0);
}

// POSIX's handlers around fork: prepare before it, parent and child after
// it in each process. glibc keeps and runs them.
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    return __register_atfork(prepare, parent, child, 0);
}
