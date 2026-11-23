// test_llvm.cpp: Rig's IR to LLVM IR and object files: hello world from
// tokens (issue #10), with the time each part of its object takes (issue
// #173), then IR built by hand for issues #32 to #37 (integer widths,
// blocks and branches, the runtime's panic, several results) and #52 (the
// panic flushing first); C functions declared on Windows under the
// Universal C Runtime's names, the time functions' 64-bit ones (#161) and
// _hypotf (#148); globals and floats (#32 to #37) and memory (#56); then
// milestone 2c's line tables (#71), on modules lowered from source, with
// the debug information of a program of several files (#112) among them;
// last, the optimiser (#116). Uses test_ir.cpp's HELLO, max_module and
// lower_program, test_checker.cpp's chain_of and joined, test_load.cpp's
// load_test_directory, write_source and in_directory, and test_base.cpp's
// md5_hex.

#include "test.h"

TEST(llvm_ir_of_hello_world) {
    Arena a = arena_make();
    Ir_Module *m = lower_tokens(&a, HELLO);
    for (int t = 0; t < 2; t++) {
        Target target = (Target)t;
        String text = {}, error = {};
        CHECK(llvm_ir_text(&a, m, target, &text, &error));
        CHECK(contains(text, "c\"Hello, World!\\00\""));
        CHECK(contains(text, "declare i32 @puts(ptr"));
        // Rig's main is C's, exported, returning 0; nothing wraps it.
        CHECK(contains(text, "define i32 @main()"));
        CHECK(contains(text, "call i32 @puts(ptr @str.0)"));
        CHECK(contains(text, "ret i32 0"));
        CHECK(!contains(text, "__rig_main"));
        CHECK(contains(text, string_cstr(&a, string_printf(&a, "target triple = \"%s\"", target_triple(target)))));
        CHECK(!contains(text, " nsw ") && !contains(text, " nuw "));  // overflow wraps (spec 6.4)
    }
    arena_free(&a);
}

TEST(llvm_widens_by_sign) {
    Arena a = arena_make();
    Ir_Module *m = lower_tokens(&a, "IDENT:g :: ( ) -> IDENT:s32 #foreign IDENT:libc ; | "
                                    "IDENT:h :: ( ) -> IDENT:u16 #foreign IDENT:libc ; | "
                                    "IDENT:k :: ( IDENT:z : IDENT:u64 , IDENT:w : IDENT:s64 ) #foreign IDENT:libc ; | "
                                    "IDENT:main :: ( ) { IDENT:k ( IDENT:h ( ) , IDENT:g ( ) ) ; }");
    String text = {}, error = {};
    CHECK(llvm_ir_text(&a, m, target_host(), &text, &error));
    CHECK(contains(text, "zext i16"));
    CHECK(contains(text, "sext i32"));
    arena_free(&a);
}

// The signature of test_ir.cpp's ir_keeps_the_sign_of_narrow_parameters,
// with calls to both procedures.
static const char *NARROW =
    "IDENT:f :: ( IDENT:a : IDENT:s8 , IDENT:b : IDENT:u8 , IDENT:c : IDENT:s16 , IDENT:d : IDENT:u16 , "
    "IDENT:e : IDENT:s32 , IDENT:g : IDENT:u64 ) -> IDENT:s8 #foreign IDENT:libc ; | "
    "IDENT:take :: ( IDENT:x : IDENT:s16 ) { } | "
    "IDENT:main :: ( ) { IDENT:f ( INT:1 , INT:2 , INT:3 , INT:4 , INT:5 , INT:6 ) ; IDENT:take ( INT:7 ) ; }";

TEST(llvm_extends_narrow_integers_on_linux) {
    // What clang writes for `signed char f(signed char, unsigned char, short,
    // unsigned short, int, unsigned long long)` on x86_64-pc-linux-gnu, on
    // the declaration and the call alike, and on Rig's own procedures, as
    // clang does on a static C function.
    Arena a = arena_make();
    Ir_Module *m = lower_tokens(&a, NARROW);
    String text = {}, error = {};
    CHECK(llvm_ir_text(&a, m, TARGET_LINUX_X64, &text, &error));
    CHECK(contains(text, "declare signext i8 @f(i8 signext, i8 zeroext, i16 signext, i16 zeroext, i32, i64)"));
    CHECK(contains(text, "call signext i8 @f(i8 signext 1, i8 zeroext 2, i16 signext 3, i16 zeroext 4, i32 5, i64 6)"));
    CHECK(contains(text, "define internal void @take(i16 signext %0)"));
    CHECK(contains(text, "call void @take(i16 signext 7)"));
    arena_free(&a);
}

TEST(llvm_doesnt_extend_narrow_integers_on_windows) {
    // The Windows x64 convention leaves an argument's upper bits undefined,
    // and clang marks nothing for x86_64-pc-windows-msvc: the callee widens.
    Arena a = arena_make();
    Ir_Module *m = lower_tokens(&a, NARROW);
    String text = {}, error = {};
    CHECK(llvm_ir_text(&a, m, TARGET_WINDOWS_X64, &text, &error));
    CHECK(contains(text, "declare i8 @f(i8, i8, i16, i16, i32, i64)"));
    CHECK(contains(text, "call i8 @f(i8 1, i8 2, i16 3, i16 4, i32 5, i64 6)"));
    CHECK(contains(text, "define internal void @take(i16 %0)"));
    CHECK(!contains(text, "signext") && !contains(text, "zeroext"));
    arena_free(&a);
}

TEST(objects_for_both_targets) {
    // Lowered from tokens, the module knows its source file (helpers.cpp's
    // locate_file), so it has debug information, which must verify too, and
    // the object carries it: CodeView in COFF's .debug$S section, DWARF in
    // ELF's .debug_info and .debug_line (both names are in the file's bytes).
    // Optimised (issue #116) or not: the optimiser keeps the debug
    // information.
    Arena a = arena_make();
    Ir_Module *m = lower_tokens(&a, HELLO);
    os_make_directory(str(".build"));
    const char *paths[] = {".build/ir-test-windows.obj", ".build/ir-test-linux.o"};
    for (int optimize = 0; optimize < 2; optimize++) {
        for (int t = 0; t < 2; t++) {
            // The file gone first, so a build that writes nothing can't
            // pass on the one the build before it wrote.
            remove(paths[t]);
            String error = {};
            bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), optimize, &a, &error);
            CHECK(ok);
            if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
            String bytes = {};
            CHECK(os_read_file(&a, str(paths[t]), &bytes));
            CHECK(bytes.count > 64);
            if (bytes.count < 4) continue;
            if ((Target)t == TARGET_WINDOWS_X64) {
                CHECK_EQ(bytes.data[0], 0x64);  // COFF: machine AMD64, 0x8664, little-endian
                CHECK_EQ(bytes.data[1], 0x86);
                CHECK(contains(bytes, ".debug$S"));
            } else {
                CHECK(memcmp(bytes.data, "\x7f" "ELF", 4) == 0);
                CHECK(contains(bytes, ".debug_info"));
                CHECK(contains(bytes, ".debug_line"));
            }
        }
    }
    arena_free(&a);
}

TEST(llvm_emit_object_gives_its_parts_times) {
    // rig build's report (spec 10.1) has a line for each part of making the
    // object file: the module built and verified, optimising it (only with
    // optimize: none without) and the object file written.
    Arena a = arena_make();
    Ir_Module *m = lower_tokens(&a, HELLO);
    os_make_directory(str(".build"));
    const char *path = ".build/llvm-times.o";
    for (int optimize = 0; optimize < 2; optimize++) {
        remove(path);
        Llvm_Times times = {};
        String error = {};
        bool ok = llvm_emit_object(m, target_host(), str(path), optimize, &a, &error, &times);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
        CHECK(times.module > 0);
        if (optimize) CHECK(times.optimising > 0);
        else CHECK_EQ(times.optimising, 0);
        CHECK(times.object > 0);
    }
    arena_free(&a);
}

TEST(llvm_reports_a_module_that_doesnt_verify) {
    // An IR the lowering never makes: a procedure returning i32 whose ret
    // gives nothing. Both entry points refuse it with LLVM's reason. The
    // object file's times then have the module's, and 0 for the parts never
    // reached (optimising and the object file), whatever the caller's
    // struct held.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "main", IR_I32);
    p->is_export = true;
    emit_ret(p, ir_new_block(&a, p), -1);
    String text = {}, error = {};
    CHECK(!llvm_ir_text(&a, m, target_host(), &text, &error));
    CHECK(contains(error, "return"));
    for (int optimize = 0; optimize < 2; optimize++) {
        error = {};
        Llvm_Times times = {-1, -1, -1};
        CHECK(!llvm_emit_object(m, target_host(), str(".build/never.o"), optimize, &a, &error, &times));
        CHECK(contains(error, "return"));
        CHECK(times.module > 0);
        CHECK_EQ(times.optimising, 0);
        CHECK_EQ(times.object, 0);
    }
    arena_free(&a);
}

TEST(llvm_reports_several_values_returned_from_a_one_result_procedure) {
    // A ret of two values from a procedure with one result (or none) is
    // another mistake only a lowering could make. The struct is built from
    // the values' own types, so LLVM's verifier reports the mismatched
    // return, as it reports any other; a struct of the procedure's return
    // type would be no struct at all, and inserting into it malformed IR
    // that an LLVM built with assertions stops on.
    Arena a = arena_make();
    for (int results = 0; results < 2; results++) {
        Ir_Module *m = arena_new<Ir_Module>(&a);
        Ir_Proc *p = new_proc_returning(m, &a, "f", results ? IR_I64 : IR_VOID);
        s32 b = ir_new_block(&a, p);
        s32 one = emit_const(p, b, IR_I64, 1), two = emit_const(p, b, IR_I32, 2);
        Ir_Instr *ret = ir_emit(p, b, IR_RET, IR_VOID);
        array_add(&ret->args, one);
        array_add(&ret->args, two);
        String text = {}, error = {};
        CHECK(!llvm_ir_text(&a, m, target_host(), &text, &error));
        CHECK(contains(error, "return"));
        CHECK(!contains(error, "InsertValue"));  // the struct itself is well formed
    }
    arena_free(&a);
}

// Milestone 2a's backend, on IR built by hand (helpers.cpp, test_ir.cpp).

// The module's LLVM text for target t; an empty string, and a failed check,
// when it doesn't verify.
static String llvm_text(Arena *a, Ir_Module *m, Target t) {
    String text = {}, error = {};
    bool ok = llvm_ir_text(a, m, t, &text, &error);
    CHECK(ok);
    if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    return text;
}

TEST(llvm_translates_blocks_and_branches) {
    // One LLVM block per IR block, named as the IR names them.
    Arena a = arena_make();
    String text = llvm_text(&a, max_module(&a), target_host());
    CHECK(contains(text, "define internal i64 @max(i64 %0, i64 %1)"));
    CHECK(contains(text, "icmp sgt i64 %0, %1"));
    CHECK(contains(text, ", label %b1, label %b2"));
    CHECK(contains(text, "\nb1:"));
    CHECK(contains(text, "ret i64 %0"));
    CHECK(contains(text, "\nb2:"));
    CHECK(contains(text, "ret i64 %1"));
    arena_free(&a);
}

TEST(llvm_puts_every_local_in_the_entry_block) {
    // A variable declared in a loop's body is an IR_LOCAL in a later block;
    // an alloca there would take more stack on every iteration, so the
    // backend puts every alloca in the entry block, run once.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    s32 b0 = ir_new_block(&a, p), b1 = ir_new_block(&a, p), b2 = ir_new_block(&a, p);
    ir_emit(p, b0, IR_JUMP, IR_VOID)->target = b1;
    ir_emit(p, b1, IR_JUMP, IR_VOID)->target = b2;
    s32 slot = ir_emit(p, b2, IR_LOCAL, IR_I64)->result;
    emit_binary(p, b2, IR_STORE, IR_I64, emit_const(p, b2, IR_I64, 0), slot);
    emit_ret(p, b2, -1);
    String text = llvm_text(&a, m, target_host());
    s64 alloca = index_of(text, "alloca i64"), first_branch = index_of(text, "br label");
    CHECK(alloca >= 0);
    CHECK(first_branch >= 0);
    CHECK(alloca < first_branch);
    CHECK(contains(text, "store i64 0, ptr %"));
    arena_free(&a);
}

TEST(llvm_returns_several_results_as_a_struct) {
    // divmod returns { i64, i64 }, whole; the call takes it apart again with
    // one extractvalue per result.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *divmod = ir_new_proc(m, &a, str("divmod"));
    s32 x = ir_add_param(divmod, IR_I64, IR_EXT_NONE);
    s32 y = ir_add_param(divmod, IR_I64, IR_EXT_NONE);
    array_add(&divmod->results, {IR_I64, IR_EXT_NONE});
    array_add(&divmod->results, {IR_I64, IR_EXT_NONE});
    s32 b = ir_new_block(&a, divmod);
    s32 q = emit_binary(divmod, b, IR_SDIV, IR_I64, x, y);
    s32 r = emit_binary(divmod, b, IR_SREM, IR_I64, x, y);
    Ir_Instr *ret = ir_emit(divmod, b, IR_RET, IR_VOID);
    array_add(&ret->args, q);
    array_add(&ret->args, r);

    Ir_Proc *use = new_proc_returning(m, &a, "use", IR_I64);
    s32 u = ir_new_block(&a, use);
    Array<s32> args = {};
    array_add(&args, emit_const(use, u, IR_I64, 7));
    array_add(&args, emit_const(use, u, IR_I64, 2));
    s32 first = ir_emit_call(m, use, u, 0, args)->result;
    emit_ret(use, u, emit_binary(use, u, IR_ADD, IR_I64, first, first + 1));

    String text = llvm_text(&a, m, target_host());
    CHECK(contains(text, "define internal { i64, i64 } @divmod(i64 %0, i64 %1)"));
    CHECK(contains(text, "ret { i64, i64 }"));
    CHECK(contains(text, "call { i64, i64 } @divmod(i64 7, i64 2)"));
    CHECK_EQ(count_of(text, "extractvalue { i64, i64 } %"), 2);
    arena_free(&a);
}

TEST(llvm_operations_have_no_wrap_flags) {
    // Rig's arithmetic wraps and its divisions are checked before they run
    // (spec 6.4), so no instruction may promise LLVM otherwise: no nsw or
    // nuw (no signed or unsigned overflow), no exact (no remainder).
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_I64);
    s32 x = ir_add_param(p, IR_I64, IR_EXT_NONE);
    s32 y = ir_add_param(p, IR_I64, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    const Ir_Op ops[] = {IR_ADD, IR_SUB,  IR_MUL, IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
                         IR_AND, IR_OR,   IR_XOR, IR_SHL,  IR_LSHR, IR_ASHR};
    s32 last = -1;
    for (Ir_Op op : ops) last = emit_binary(p, b, op, IR_I64, x, y);
    emit_ret(p, b, last);
    String text = llvm_text(&a, m, target_host());
    const char *names[] = {"add", "sub", "mul", "sdiv", "udiv", "srem", "urem",
                           "and", "or",  "xor", "shl",  "lshr", "ashr"};
    for (const char *name : names) {
        CHECK(contains(text, string_cstr(&a, string_printf(&a, "= %s i64 %%0, %%1", name))));
    }
    CHECK(!contains(text, "nsw"));
    CHECK(!contains(text, "nuw"));
    CHECK(!contains(text, "exact"));
    arena_free(&a);
}

// program :: () { panic with "oops" }.
static Ir_Module *panic_module(Arena *a) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    s32 message = add_string(m, "str.0", "oops");
    Ir_Proc *p = new_proc_returning(m, a, "program", IR_VOID);
    ir_emit(p, ir_new_block(a, p), IR_PANIC, IR_VOID)->target = message;
    return m;
}

TEST(llvm_panic_writes_then_exits) {
    // A panic calls the runtime's __rig_panic with its message, all but the
    // final zero; __rig_panic writes it to stderr (file descriptor 2) and
    // exits with 101, through C's exit (llvm_panic_flushes_before_it_writes:
    // stdout's buffer is flushed first).
    Arena a = arena_make();
    String linux_text = llvm_text(&a, panic_module(&a), TARGET_LINUX_X64);
    CHECK(contains(linux_text, "define internal void @__rig_panic(ptr %0, i64 %1)"));
    CHECK(contains(linux_text, "call void @__rig_panic(ptr @str.0, i64 4)"));
    CHECK(contains(linux_text, "call i64 @write(i32 2,"));
    CHECK(contains(linux_text, "call void @exit(i32 101)"));
    CHECK(contains(linux_text, "noreturn"));
    String windows_text = llvm_text(&a, panic_module(&a), TARGET_WINDOWS_X64);
    CHECK(contains(windows_text, "call i32 @_write(i32 2,"));
    CHECK(contains(windows_text, "call void @exit(i32 101)"));
    CHECK(!contains(windows_text, "@write("));
    // Without a panic there's no runtime routine at all.
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, max_module(&a), (Target)t);
        CHECK(!contains(text, "__rig_panic"));
        CHECK(!contains(text, "@exit"));
    }
    arena_free(&a);
}

TEST(llvm_panic_flushes_before_it_writes) {
    // stdout's buffer is flushed (C's fflush(NULL): every stream) before the
    // message is written, so when stdout and stderr go to one place (2>&1)
    // the program's earlier output comes first, then the error.
    Arena a = arena_make();
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, panic_module(&a), (Target)t);
        s64 flush = index_of(text, "call i32 @fflush(ptr null)");
        s64 write = index_of(text, t == TARGET_WINDOWS_X64 ? "call i32 @_write(i32 2," : "call i64 @write(i32 2,");
        CHECK(flush >= 0);
        CHECK(write >= 0);
        CHECK(flush < write);
    }
    arena_free(&a);
}

TEST(llvm_runtime_names_win_over_the_programs) {
    // A program's own internal procedure called write is renamed by LLVM
    // (write.1), since the runtime's C write was declared first; a program's
    // own foreign exit is the runtime's declaration, shared. Both still
    // called, and the module verifies.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 message = add_string(m, "str.0", "oops");
    Ir_Proc *own_write = new_proc_returning(m, &a, "write", IR_VOID);
    emit_ret(own_write, ir_new_block(&a, own_write), -1);
    Ir_Proc *own_exit = new_proc_returning(m, &a, "exit", IR_VOID);
    own_exit->is_foreign = true;
    ir_add_param(own_exit, IR_I32, IR_EXT_NONE);
    Ir_Proc *p = new_proc_returning(m, &a, "program", IR_VOID);
    s32 b = ir_new_block(&a, p);
    ir_emit_call(m, p, b, 0, {});
    Array<s32> args = {};
    array_add(&args, emit_const(p, b, IR_I32, 3));
    ir_emit_call(m, p, b, 1, args);
    ir_emit(p, b, IR_PANIC, IR_VOID)->target = message;

    // A second declaration of exit would be renamed (exit.1, or another
    // number: LLVM numbers its renamings across the module), and the call of
    // the program's own would go to a function the linker can't find.
    String text = llvm_text(&a, m, TARGET_LINUX_X64);
    CHECK(contains(text, "declare i64 @write("));
    CHECK(contains(text, "define internal void @write.1()"));
    CHECK(contains(text, "call void @write.1()"));
    CHECK_EQ(count_of(text, "declare void @exit("), 1);
    CHECK(!contains(text, "@exit."));
    CHECK(contains(text, "call void @exit(i32 3)"));
    CHECK(contains(text, "call void @exit(i32 101)"));
    // On Windows the runtime's is _write: no clash, the program's write
    // keeps its name.
    text = llvm_text(&a, m, TARGET_WINDOWS_X64);
    CHECK(contains(text, "define internal void @write()"));
    CHECK_EQ(count_of(text, "declare void @exit("), 1);
    CHECK(!contains(text, "@exit."));
    CHECK(contains(text, "call void @exit(i32 3)"));
    CHECK(contains(text, "call void @exit(i32 101)"));

    // A program's own global variable called write: the runtime's C write
    // is declared before the globals too, so on Linux the global is the one
    // renamed, and the program's load reads it under its new name.
    m = arena_new<Ir_Module>(&a);
    message = add_string(m, "str.0", "oops");
    Ir_Global own_global = {};
    own_global.name = str("write");
    own_global.type = IR_I64;
    array_add(&m->globals, own_global);
    p = new_proc_returning(m, &a, "program", IR_VOID);
    b = ir_new_block(&a, p);
    Ir_Instr *address = ir_emit(p, b, IR_ADDR, IR_PTR);
    address->target = 1;
    Ir_Instr *load = ir_emit(p, b, IR_LOAD, IR_I64);
    load->a = address->result;
    ir_emit(p, b, IR_PANIC, IR_VOID)->target = message;
    text = llvm_text(&a, m, TARGET_LINUX_X64);
    CHECK(contains(text, "declare i64 @write("));
    CHECK(contains(text, "@write.1 = internal global i64 0"));
    CHECK(contains(text, "load i64, ptr @write.1"));
    CHECK(contains(text, "call i64 @write(i32 2,"));
    text = llvm_text(&a, m, TARGET_WINDOWS_X64);
    CHECK(contains(text, "@write = internal global i64 0"));
    CHECK(contains(text, "load i64, ptr @write"));

    // fflush, which the runtime calls first, the same way on both targets:
    // a program's own internal procedure called fflush is renamed, and the
    // runtime's call still goes to C's...
    for (int t = 0; t < 2; t++) {
        m = arena_new<Ir_Module>(&a);
        message = add_string(m, "str.0", "oops");
        Ir_Proc *own_flush = new_proc_returning(m, &a, "fflush", IR_VOID);
        emit_ret(own_flush, ir_new_block(&a, own_flush), -1);
        p = new_proc_returning(m, &a, "program", IR_VOID);
        b = ir_new_block(&a, p);
        ir_emit_call(m, p, b, 0, {});
        ir_emit(p, b, IR_PANIC, IR_VOID)->target = message;
        text = llvm_text(&a, m, (Target)t);
        CHECK_EQ(count_of(text, "declare i32 @fflush(ptr)"), 1);
        CHECK(contains(text, "define internal void @fflush.1()"));
        CHECK(contains(text, "call void @fflush.1()"));
        CHECK(contains(text, "call i32 @fflush(ptr null)"));
    }
    // ...and a program's own foreign declaration of fflush is the runtime's,
    // shared: one declaration, both calls to it.
    for (int t = 0; t < 2; t++) {
        m = arena_new<Ir_Module>(&a);
        message = add_string(m, "str.0", "oops");
        Ir_Proc *own_flush = new_proc_returning(m, &a, "fflush", IR_I32);
        own_flush->is_foreign = true;
        ir_add_param(own_flush, IR_PTR, IR_EXT_NONE);
        p = new_proc_returning(m, &a, "program", IR_VOID);
        b = ir_new_block(&a, p);
        Array<s32> null_args = {};
        array_add(&null_args, emit_const(p, b, IR_PTR, 0));
        ir_emit_call(m, p, b, 0, null_args);
        ir_emit(p, b, IR_PANIC, IR_VOID)->target = message;
        text = llvm_text(&a, m, (Target)t);
        CHECK_EQ(count_of(text, "declare i32 @fflush(ptr)"), 1);
        CHECK(!contains(text, "@fflush."));
        CHECK_EQ(count_of(text, "call i32 @fflush(ptr null)"), 2);
    }
    arena_free(&a);
}

// Issue #135: a foreign procedure's C name, `#foreign libc "free"`.

TEST(llvm_foreign_c_name_is_one_symbol) {
    // Two foreign procedures naming one C function are one declaration,
    // under the C name: both calls, and the procedure value, are @free.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "free :: (p: *void) #foreign libc;\n"
                                "c_free :: (p: *void) #foreign libc \"free\";\n"
                                "main :: () {\n"
                                "    free(null);\n"
                                "    c_free(null);\n"
                                "    f := c_free;\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK_EQ(count_of(text, "declare void @free(ptr)"), 1);
        CHECK(!contains(text, "@free."));
        CHECK(!contains(text, "c_free"));
        CHECK_EQ(count_of(text, "call void @free(ptr null)"), 2);
        CHECK(contains(text, "store ptr @free, ptr %"));
    }
    arena_free(&a);
}

TEST(llvm_foreign_c_name_wins_over_the_programs_names) {
    // The C names are declared before the program's globals and its own
    // procedures, as the runtime's are, so a procedure of the program called
    // free, or a global called malloc, is the one LLVM renames (malloc.1,
    // free.2: LLVM numbers its renamings across the module): internal, it
    // works the same under any name. The C names stay C's, whichever comes
    // first in the source.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "malloc : int = 7;\n"
                                "free :: (p: *void) -> int { c_free(p); return malloc; }\n"
                                "c_free :: (p: *void) #foreign libc \"free\";\n"
                                "c_malloc :: (size: u64) -> *void #foreign libc \"malloc\";\n"
                                "main :: () {\n"
                                "    n := free(c_malloc(16));\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "declare void @free(ptr)"));
        CHECK(contains(text, "declare ptr @malloc(i64)"));
        CHECK(contains(text, "@malloc.1 = internal global i64 7"));
        CHECK(contains(text, "define internal i64 @free.2(ptr %0)"));
        CHECK(contains(text, "call void @free(ptr %"));
        CHECK(contains(text, "call ptr @malloc(i64 16)"));
        CHECK(contains(text, "call i64 @free.2(ptr %"));
        CHECK(contains(text, "load i64, ptr @malloc.1"));
        // A debugger still knows them by their names in the source.
        CHECK(contains(text, "!DISubprogram(name: \"free\""));
        CHECK(contains(text, "!DIGlobalVariable(name: \"malloc\""));
    }
    arena_free(&a);
}

TEST(llvm_foreign_c_name_calls_through_each_declaration) {
    // One C function declared twice with different signatures: each call
    // goes through its own declaration's function type and attributes, and
    // the module verifies. Whether the two agree with C is the program's
    // business, as in C. The function itself is declared, and marked, as
    // the first declaration says: on Linux the s8 one's signext, which the
    // u8 one's zeroext would contradict on the same parameter.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "length :: (s: *u8) -> u64 #foreign libc \"strlen\";\n"
                                "short_length :: (s: *u8) -> u32 #foreign libc \"strlen\";\n"
                                "abs_s8 :: (n: s8) -> s32 #foreign libc \"abs\";\n"
                                "abs_u8 :: (n: u8) -> s32 #foreign libc \"abs\";\n"
                                "main :: () {\n"
                                "    a := length(\"four\");\n"
                                "    b := short_length(\"four\");\n"
                                "    c := abs_s8(-3);\n"
                                "    d := abs_u8(3);\n"
                                "}\n");
    String text = llvm_text(&a, m, TARGET_LINUX_X64);
    CHECK_EQ(count_of(text, "declare i64 @strlen(ptr)"), 1);
    CHECK(!contains(text, "@strlen."));
    CHECK(contains(text, "call i64 @strlen(ptr "));
    CHECK(contains(text, "call i32 @strlen(ptr "));
    CHECK_EQ(count_of(text, "declare i32 @abs(i8 signext)"), 1);
    CHECK(contains(text, "call i32 @abs(i8 signext -3)"));
    CHECK(contains(text, "call i32 @abs(i8 zeroext 3)"));
    text = llvm_text(&a, m, TARGET_WINDOWS_X64);
    CHECK_EQ(count_of(text, "declare i32 @abs(i8)"), 1);
    CHECK(contains(text, "call i32 @strlen(ptr "));
    arena_free(&a);
}

// Issue #161: C runtime time functions on Windows, by their 64-bit names.

TEST(llvm_windows_time_functions_are_the_64_bit_ones) {
    // On Windows the C runtime's time functions are declared under the
    // Universal C Runtime's 64-bit exports, which a C program's headers
    // call: the UCRT exports none of their names, nor does anything a
    // program links define them (spec 10.6). _ftime and ftime, and
    // _utime and utime, are one function each, as _time64 declared under
    // its own name too is time's. On Linux each stays under its own name
    // (glibc's are 64 bits; it has no Microsoft names, which the linker
    // would say).
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Timespec :: struct { seconds: s64; nanoseconds: s32; padding: s32; }\n"
                                "Tm :: struct { second: s32; minute: s32; hour: s32; }\n"
                                "Timeb :: struct { time: s64; milliseconds: u16; timezone: s16; dstflag: s16; }\n"
                                "Utimbuf :: struct { access: s64; modification: s64; }\n"
                                "timespec_get :: (ts: *Timespec, base: s32) -> s32 #foreign libc;\n"
                                "time :: (t: *s64) -> s64 #foreign libc;\n"
                                "time64 :: (t: *s64) -> s64 #foreign libc \"_time64\";\n"
                                "c_time :: (t: *s64) -> s64 #foreign libc \"time\";\n"
                                "gmtime :: (t: *s64) -> *Tm #foreign libc;\n"
                                "localtime :: (t: *s64) -> *Tm #foreign libc;\n"
                                "mktime :: (tm: *Tm) -> s64 #foreign libc;\n"
                                "difftime :: (end: s64, start: s64) -> float64 #foreign libc;\n"
                                "ctime :: (t: *s64) -> *u8 #foreign libc;\n"
                                "_mkgmtime :: (tm: *Tm) -> s64 #foreign libc;\n"
                                "_wctime :: (t: *s64) -> *u16 #foreign libc;\n"
                                "_ftime :: (tb: *Timeb) #foreign libc;\n"
                                "ftime :: (tb: *Timeb) #foreign libc;\n"
                                "_ftime_s :: (tb: *Timeb) -> s32 #foreign libc;\n"
                                "_futime :: (fd: s32, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "_utime :: (path: *u8, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "utime :: (path: *u8, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "_wutime :: (path: *u16, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "ctime_s :: (buffer: *u8, size: u64, t: *s64) -> s32 #foreign libc;\n"
                                "_wctime_s :: (buffer: *u16, size: u64, t: *s64) -> s32 #foreign libc;\n"
                                "gmtime_s :: (tm: *Tm, t: *s64) -> s32 #foreign libc;\n"
                                "localtime_s :: (tm: *Tm, t: *s64) -> s32 #foreign libc;\n"
                                "main :: () {\n"
                                "    ts : Timespec;\n"
                                "    r := timespec_get(*ts, 1);\n"
                                "    t := time(null);\n"
                                "    u := time64(null);\n"
                                "    v := c_time(null);\n"
                                "    g := gmtime(*t);\n"
                                "    l := mktime(localtime(*t));\n"
                                "    d := difftime(t, u);\n"
                                "    c := ctime(*t);\n"
                                "    k := _mkgmtime(g);\n"
                                "    w := _wctime(*t);\n"
                                "    tb : Timeb;\n"
                                "    _ftime(*tb);\n"
                                "    ftime(*tb);\n"
                                "    fs := _ftime_s(*tb);\n"
                                "    times : Utimbuf;\n"
                                "    f := _futime(0, *times);\n"
                                "    p := _utime(null, *times);\n"
                                "    q := utime(null, *times);\n"
                                "    wp := _wutime(null, *times);\n"
                                "    cs := ctime_s(null, 0, *t);\n"
                                "    ws := _wctime_s(null, 0, *t);\n"
                                "    tm : Tm;\n"
                                "    gs := gmtime_s(*tm, *t);\n"
                                "    ls := localtime_s(*tm, *t);\n"
                                "}\n");
    // Each function: its declaration, with %s for its name; C's name; and
    // its Windows name.
    struct Time_Function {
        const char *declaration;
        const char *c_name;
        const char *windows_name;
    };
    const Time_Function functions[] = {
        {"declare i32 @%s(ptr, i32)", "timespec_get", "_timespec64_get"},
        {"declare i64 @%s(ptr)", "time", "_time64"},
        {"declare ptr @%s(ptr)", "gmtime", "_gmtime64"},
        {"declare ptr @%s(ptr)", "localtime", "_localtime64"},
        {"declare i64 @%s(ptr)", "mktime", "_mktime64"},
        {"declare double @%s(i64, i64)", "difftime", "_difftime64"},
        {"declare ptr @%s(ptr)", "ctime", "_ctime64"},
        {"declare i64 @%s(ptr)", "_mkgmtime", "_mkgmtime64"},
        {"declare ptr @%s(ptr)", "_wctime", "_wctime64"},
        {"declare void @%s(ptr)", "_ftime", "_ftime64"},
        {"declare void @%s(ptr)", "ftime", "_ftime64"},
        {"declare i32 @%s(ptr)", "_ftime_s", "_ftime64_s"},
        {"declare i32 @%s(i32, ptr)", "_futime", "_futime64"},
        {"declare i32 @%s(ptr, ptr)", "_utime", "_utime64"},
        {"declare i32 @%s(ptr, ptr)", "utime", "_utime64"},
        {"declare i32 @%s(ptr, ptr)", "_wutime", "_wutime64"},
        {"declare i32 @%s(ptr, i64, ptr)", "ctime_s", "_ctime64_s"},
        {"declare i32 @%s(ptr, i64, ptr)", "_wctime_s", "_wctime64_s"},
        {"declare i32 @%s(ptr, ptr)", "gmtime_s", "_gmtime64_s"},
        {"declare i32 @%s(ptr, ptr)", "localtime_s", "_localtime64_s"},
    };
    for (int t = 0; t < 2; t++) {
        bool windows = (Target)t == TARGET_WINDOWS_X64;
        String text = llvm_text(&a, m, (Target)t);
        for (const Time_Function &f : functions) {
            const char *declared = windows ? f.windows_name : f.c_name;
            const char *other = windows ? f.c_name : f.windows_name;
            CHECK_EQ(count_of(text, string_cstr(&a, string_printf(&a, f.declaration, declared))), 1);
            // On Linux _time64 is declared too, under the name it's given.
            if (!windows && string_eq(str(f.c_name), str("time"))) continue;
            CHECK(!contains(text, string_cstr(&a, string_printf(&a, "@%s(", other))));
        }
        // time, time64 and c_time are one declaration on Windows, never
        // renamed (_time64.1), and the three calls go to it: c_time's C
        // name, "time", is renamed as time's own is. On Linux c_time is
        // time's declaration.
        CHECK(!contains(text, "@_time64."));
        CHECK_EQ(count_of(text, "call i64 @_time64(ptr null)"), windows ? 3 : 1);
        CHECK_EQ(count_of(text, "call i64 @time(ptr null)"), windows ? 0 : 2);
    }
    arena_free(&a);
}

TEST(llvm_windows_hypotf_is_the_ucrts_hypotf) {
    // The Universal C Runtime exports hypotf only as _hypotf (its <math.h>
    // defines hypotf as an inline function over it), so on Windows a
    // foreign procedure whose C name is hypotf is declared as _hypotf: one
    // declaration with a program's own _hypotf, never renamed (_hypotf.1),
    // and the three calls go to it. On Linux each stays under its own name,
    // since glibc's libm has hypotf.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "hypotf :: (x: float32, y: float32) -> float32 #foreign libc;\n"
                                "c_hypotf :: (x: float32, y: float32) -> float32 #foreign libc \"hypotf\";\n"
                                "_hypotf :: (x: float32, y: float32) -> float32 #foreign libc;\n"
                                "main :: () {\n"
                                "    h := hypotf(3, 4);\n"
                                "    c := c_hypotf(3, 4);\n"
                                "    u := _hypotf(3, 4);\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        bool windows = (Target)t == TARGET_WINDOWS_X64;
        String text = llvm_text(&a, m, (Target)t);
        CHECK_EQ(count_of(text, "declare float @_hypotf(float, float)"), 1);
        CHECK_EQ(count_of(text, "declare float @hypotf(float, float)"), windows ? 0 : 1);
        CHECK(!contains(text, "@_hypotf."));
        CHECK_EQ(count_of(text, "call float @_hypotf("), windows ? 3 : 1);
        CHECK_EQ(count_of(text, "call float @hypotf("), windows ? 0 : 2);
    }
    arena_free(&a);
}

TEST(llvm_bool_is_zeroext_on_both_targets) {
    // clang marks C's _Bool zeroext on Linux and on Windows alike (see
    // mark_extensions), so an i1 parameter or result is marked on both.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *f = new_proc_returning(m, &a, "f", IR_I1);
    f->is_foreign = true;
    ir_add_param(f, IR_I1, IR_EXT_NONE);
    Ir_Proc *p = new_proc_returning(m, &a, "g", IR_I1);
    s32 b = ir_new_block(&a, p);
    Array<s32> args = {};
    array_add(&args, emit_const(p, b, IR_I1, 1));
    emit_ret(p, b, ir_emit_call(m, p, b, 0, args)->result);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "declare zeroext i1 @f(i1 zeroext)"));
        CHECK(contains(text, "call zeroext i1 @f(i1 zeroext true)"));
        CHECK(contains(text, "define internal zeroext i1 @g()"));
    }
    arena_free(&a);
}

TEST(llvm_variable_global) {
    // A Rig global is an internal LLVM global of its type, with its first
    // value; addr gives its address.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Global counter = {};
    counter.name = str("counter");
    counter.type = IR_I64;
    counter.initial = 5;
    array_add(&m->globals, counter);
    Ir_Proc *p = new_proc_returning(m, &a, "get", IR_I64);
    s32 b = ir_new_block(&a, p);
    Ir_Instr *address = ir_emit(p, b, IR_ADDR, IR_PTR);
    address->target = 0;
    Ir_Instr *load = ir_emit(p, b, IR_LOAD, IR_I64);
    load->a = address->result;
    emit_ret(p, b, load->result);
    String text = llvm_text(&a, m, target_host());
    CHECK(contains(text, "@counter = internal global i64 5"));
    CHECK(contains(text, "load i64, ptr @counter"));
    arena_free(&a);
}

TEST(llvm_bool_parameter_from_rig_is_zeroext) {
    // A Rig bool is C's _Bool: an i1, marked zeroext on both targets, on Rig
    // procedures and foreign ones alike, and on the calls to them.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "isok :: (b: bool) -> bool #foreign libc;\n"
                                "flip :: (b: bool) -> bool { return !b; }\n"
                                "main :: () {\n"
                                "    x := flip(true);\n"
                                "    y := isok(x);\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "declare zeroext i1 @isok(i1 zeroext)"));
        CHECK(contains(text, "define internal zeroext i1 @flip(i1 zeroext %0)"));
        CHECK(contains(text, "call zeroext i1 @flip(i1 zeroext true)"));
        CHECK(contains(text, "call zeroext i1 @isok(i1 zeroext %"));
    }
    arena_free(&a);
}

// Milestone 2a's floats (issue #37), on IR built by hand.

TEST(llvm_float_to_int_saturates) {
    // LLVM's plain fptosi and fptoui give poison for a value outside the
    // integer's range, and for NaN; Rig's cast saturates instead (spec 6.4),
    // which is what the fptosi.sat and fptoui.sat intrinsics do. Each is a
    // call, named by the integer type, then the float type.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_I32);
    s32 x = ir_add_param(p, IR_F64, IR_EXT_NONE);
    s32 y = ir_add_param(p, IR_F32, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    s32 sign = emit_convert(p, b, IR_FPTOSI_SAT, IR_F64, x, IR_I32);
    s32 byte = emit_convert(p, b, IR_FPTOUI_SAT, IR_F32, y, IR_I8);
    s32 wide = emit_convert(p, b, IR_ZEXT, IR_I8, byte, IR_I32);
    emit_ret(p, b, emit_binary(p, b, IR_ADD, IR_I32, sign, wide));
    String text = llvm_text(&a, m, target_host());
    CHECK(contains(text, "call i32 @llvm.fptosi.sat.i32.f64(double %0)"));
    CHECK(contains(text, "call i8 @llvm.fptoui.sat.i8.f32(float %1)"));
    CHECK(!contains(text, "= fptosi "));
    CHECK(!contains(text, "= fptoui "));
    arena_free(&a);
}

TEST(llvm_float32_constant_is_rounded) {
    // A float constant holds its value as a double's bits; for an f32 the
    // backend rounds it to the nearest float. LLVM 23 writes a constant in
    // decimal when six digits give it back exactly, and otherwise as its
    // own bits in hex, f0x...: so 0.1, which six digits give back as a float
    // and as a double, can't show the rounding, and 1/3 can. Its double is
    // 0x3FD5555555555555, a 1 bit then 01 repeated; a float keeps 23 bits of
    // that fraction, 0x2AAAAA, and the next bit is 1, so to nearest it
    // rounds up, to 0x3EAAAAAB (truncating would give 0x3EAAAAAA).
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *f = new_proc_returning(m, &a, "single", IR_F32);
    s32 b = ir_new_block(&a, f);
    emit_ret(f, b, emit_const(f, b, IR_F32, f64_bits(1.0 / 3.0)));
    Ir_Proc *g = new_proc_returning(m, &a, "double", IR_F64);
    b = ir_new_block(&a, g);
    emit_ret(g, b, emit_const(g, b, IR_F64, f64_bits(1.0 / 3.0)));
    Ir_Proc *h = new_proc_returning(m, &a, "tenth", IR_F32);
    b = ir_new_block(&a, h);
    emit_ret(h, b, emit_const(h, b, IR_F32, f64_bits(0.1)));
    String text = llvm_text(&a, m, target_host());
    CHECK(contains(text, "ret float f0x3EAAAAAB"));
    CHECK(contains(text, "ret double f0x3FD5555555555555"));
    CHECK(contains(text, "ret float 1.000000e-01"));
    arena_free(&a);
}

TEST(llvm_float_operations_are_strict) {
    // IEEE 754 arithmetic exactly (spec 6.4): no fast-math flag on any
    // operation, so LLVM may not assume away NaN, infinity or -0.0, nor
    // reorder the arithmetic. NaN != NaN is true: != is fcmp une, the other
    // comparisons ordered.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_F64);
    s32 x = ir_add_param(p, IR_F64, IR_EXT_NONE);
    s32 y = ir_add_param(p, IR_F64, IR_EXT_NONE);
    s32 n = ir_add_param(p, IR_I32, IR_EXT_NONE);
    s32 s = ir_add_param(p, IR_F32, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    const Ir_Op ops[] = {IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV};
    for (Ir_Op op : ops) emit_binary(p, b, op, IR_F64, x, y);
    ir_emit(p, b, IR_FNEG, IR_F64)->a = x;
    const Ir_Pred preds[] = {IR_FOEQ, IR_FUNE, IR_FOLT, IR_FOLE, IR_FOGT, IR_FOGE};
    for (Ir_Pred pred : preds) {
        Ir_Instr *cmp = ir_emit(p, b, IR_FCMP, IR_F64);
        cmp->pred = pred;
        cmp->a = x;
        cmp->b = y;
    }
    emit_convert(p, b, IR_FPEXT, IR_F32, s, IR_F64);
    emit_convert(p, b, IR_FPTRUNC, IR_F64, x, IR_F32);
    emit_convert(p, b, IR_SITOFP, IR_I32, n, IR_F64);
    s32 last = emit_convert(p, b, IR_UITOFP, IR_I32, n, IR_F64);
    emit_ret(p, b, last);
    String text = llvm_text(&a, m, target_host());
    const char *names[] = {"fadd", "fsub", "fmul", "fdiv"};
    for (const char *name : names) {
        CHECK(contains(text, string_cstr(&a, string_printf(&a, "= %s double %%0, %%1", name))));
    }
    CHECK(contains(text, "= fneg double %0"));
    const char *predicates[] = {"oeq", "une", "olt", "ole", "ogt", "oge"};
    for (const char *pred : predicates) {
        CHECK(contains(text, string_cstr(&a, string_printf(&a, "= fcmp %s double %%0, %%1", pred))));
    }
    CHECK(contains(text, "= fpext float %3 to double"));
    CHECK(contains(text, "= fptrunc double %0 to float"));
    CHECK(contains(text, "= sitofp i32 %2 to double"));
    CHECK(contains(text, "= uitofp i32 %2 to double"));
    const char *flags[] = {"fast", "nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc"};
    for (const char *flag : flags) CHECK(!contains(text, flag));
    arena_free(&a);
}

TEST(llvm_float_parameters_and_globals) {
    // A foreign procedure takes and returns floats as C's float and double:
    // LLVM's float and double types, which each target's calling convention
    // passes in the SSE registers, with no extension. A float global holds
    // its first value as a constant of its type.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Global g = {};
    g.name = str("scale");
    g.type = IR_F32;
    g.initial = f64_bits(1.5);
    array_add(&m->globals, g);
    Ir_Proc *f = new_proc_returning(m, &a, "half", IR_F64);
    f->is_foreign = true;
    ir_add_param(f, IR_F32, IR_EXT_NONE);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "@scale = internal global float 1.500000e+00"));
        CHECK(contains(text, "declare double @half(float)"));
    }
    arena_free(&a);
}

// Milestone 2b's memory (issue #56), on IR built by hand.

TEST(llvm_aggregate_locals_are_in_the_entry_block) {
    // A local of bytes (a struct's, an array's) is an alloca like any slot,
    // in the entry block whichever block declares it, so one in a loop's
    // body takes its stack once, not on every iteration.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    s32 b0 = ir_new_block(&a, p), b1 = ir_new_block(&a, p), b2 = ir_new_block(&a, p);
    ir_emit(p, b0, IR_JUMP, IR_VOID)->target = b1;
    ir_emit(p, b1, IR_JUMP, IR_VOID)->target = b2;
    s32 buffer = emit_local_bytes(p, b2, 1000, 1);
    emit_zero(p, b2, buffer, 1000);
    emit_ret(p, b2, -1);
    String text = llvm_text(&a, m, target_host());
    s64 alloca = index_of(text, "alloca [1000 x i8], align 1"), first_branch = index_of(text, "br label");
    CHECK(alloca >= 0);
    CHECK(first_branch >= 0);
    CHECK(alloca < first_branch);
    arena_free(&a);
}

TEST(llvm_ptradd_has_no_inbounds) {
    // ptradd is a getelementptr over bytes, without inbounds: Rig's pointer
    // arithmetic may leave the object it started in (spec 6.4), and inbounds
    // would make that poison.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_PTR);
    s32 at = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    s32 offset = ir_add_param(p, IR_I64, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    emit_ret(p, b, emit_ptradd(p, b, at, offset));
    String text = llvm_text(&a, m, target_host());
    CHECK(contains(text, "getelementptr i8, ptr %0, i64 %1"));
    CHECK(!contains(text, "inbounds"));
    arena_free(&a);
}

TEST(llvm_copy_is_memmove) {
    // copy is llvm.memmove, never llvm.memcpy: the two sides may overlap
    // (a = a, or a struct copied into itself through a pointer). Neither
    // copy nor zero assumes anything of the addresses' alignment.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    s32 to = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    s32 from = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    emit_copy(p, b, to, from, 24);
    emit_zero(p, b, to, 16);
    emit_ret(p, b, -1);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "call void @llvm.memmove.p0.p0.i64(ptr align 1 %0, ptr align 1 %1, i64 24, i1 false)"));
        CHECK(contains(text, "call void @llvm.memset.p0.i64(ptr align 1 %0, i8 0, i64 16, i1 false)"));
        // (C's memcpy is declared, for what LLVM may call itself, but the
        // program's copies are never the memcpy intrinsic.)
        CHECK(!contains(text, "@llvm.memcpy"));
    }
    arena_free(&a);
}

// A module with @str.0 = "abc" and @table, 24 bytes aligned to 8: a byte 3,
// 7 zeros, the address of @str.0, and 8 zeros. `string_first` says which of
// the two is global 0.
static Ir_Module *table_module(Arena *a, bool string_first) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    s32 text = -1;
    if (string_first) text = add_string(m, "str.0", "abc");
    s32 table = add_byte_global(m, "table", 24, 8);
    if (!string_first) text = add_string(m, "str.0", "abc");
    u8 *bytes = (u8 *)arena_alloc(a, 24, 1);  // zeroed
    bytes[0] = 3;
    m->globals[table].bytes = {bytes, 24};
    array_add(&m->globals[table].relocs, {8, text, false});
    return m;
}

TEST(llvm_byte_global_with_a_relocation) {
    // A variable of bytes holding an address is a packed struct of LLVM's
    // (no padding between its fields), its bytes split where the address
    // goes: the address is a ptr constant, which the linker fills in. A
    // variable of bytes that are all zero is an array of zeros. Both are
    // writable, internal, and aligned as the IR says.
    Arena a = arena_make();
    Ir_Module *m = table_module(&a, true);
    add_byte_global(m, "origin", 16, 8);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "@table = internal global <{ [8 x i8], ptr, [8 x i8] }> "
                             "<{ [8 x i8] c\"\\03\\00\\00\\00\\00\\00\\00\\00\", ptr @str.0, "
                             "[8 x i8] zeroinitializer }>, align 8"));
        CHECK(contains(text, "@origin = internal global [16 x i8] zeroinitializer, align 8"));
    }
    arena_free(&a);
}

TEST(llvm_byte_global_layouts) {
    // The struct has a run of bytes only where there are bytes: none before
    // an address at offset 0, between two addresses side by side, or after
    // one that ends the global, so @pair is just its two addresses. Being
    // packed, the struct puts an address at any offset, aligned or not, as
    // @odd's at 3. Bytes that aren't all zero, with no address among them,
    // are an array of them, as a string literal's are.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 text = add_string(m, "str.0", "abc");
    s32 pair = add_byte_global(m, "pair", 16, 8);
    array_add(&m->globals[pair].relocs, {0, text, false});
    array_add(&m->globals[pair].relocs, {8, text, false});
    s32 odd = add_byte_global(m, "odd", 12, 4);
    u8 *odd_bytes = (u8 *)arena_alloc(&a, 12, 1);  // zeroed
    odd_bytes[0] = 1;
    odd_bytes[11] = 2;
    m->globals[odd].bytes = {odd_bytes, 12};
    array_add(&m->globals[odd].relocs, {3, text, false});
    s32 counts = add_byte_global(m, "counts", 4, 4);
    static const u8 four[] = {1, 2, 3, 4};
    m->globals[counts].bytes = {four, 4};
    for (int t = 0; t < 2; t++) {
        String llvm = llvm_text(&a, m, (Target)t);  // and it verifies
        CHECK(contains(llvm, "@pair = internal global <{ ptr, ptr }> <{ ptr @str.0, ptr @str.0 }>, align 8"));
        CHECK(contains(llvm, "@odd = internal global <{ [3 x i8], ptr, [1 x i8] }> "
                             "<{ [3 x i8] c\"\\01\\00\\00\", ptr @str.0, [1 x i8] c\"\\02\" }>, align 4"));
        CHECK(contains(llvm, "@counts = internal global [4 x i8] c\"\\01\\02\\03\\04\", align 4"));
    }
    arena_free(&a);
}

TEST(llvm_big_global_holds_no_zeros) {
    // A global's zeros are never in the compiler's memory: a terabyte with no
    // value is a zeroinitializer (the program's .bss), lowered from the
    // source and built into an object for both targets; a value's bytes
    // stop at its last field given, and a long tail of zeros after them is
    // a zeroinitializer of its own; a short tail is written out with them.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "big : [1099511627776]u8;\n"
                                "S :: struct { x: int; a: [8192]u8; }\n"
                                "s := S.{x = 3};\n"
                                "main :: () { big[5] = 1; }\n");
    CHECK_EQ(m->globals[0].bytes.count, 0);
    CHECK_EQ(m->globals[1].bytes.count, 8);
    String ir = ir_print(&a, m);
    CHECK(contains(ir, "global @big: 1099511627776 align 1 = zero\n"));
    static const u8 one[] = {1};
    s32 short_tail = add_byte_global(m, "short_tail", 16, 8);
    m->globals[short_tail].bytes = {one, 1};
    os_make_directory(str(".build"));
    const char *paths[] = {".build/big-global-windows.obj", ".build/big-global-linux.o"};
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "@big = internal global [1099511627776 x i8] zeroinitializer, align 1"));
        CHECK(contains(text, "@s = internal global <{ [8 x i8], [8192 x i8] }> "
                             "<{ [8 x i8] c\"\\03\\00\\00\\00\\00\\00\\00\\00\", [8192 x i8] zeroinitializer }>, "
                             "align 8"));
        CHECK(contains(text, "@short_tail = internal global [16 x i8] "
                             "c\"\\01\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\", align 8"));
        String error = {};
        bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), false, &a, &error);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    }
    arena_free(&a);
}

TEST(llvm_relocation_to_a_later_global) {
    // Every global is made before any is given its first value, so a
    // relocation may name a global that comes after it.
    Arena a = arena_make();
    Ir_Module *m = table_module(&a, false);
    String text = llvm_text(&a, m, target_host());
    CHECK(contains(text, "@table = internal global <{ [8 x i8], ptr, [8 x i8] }>"));
    CHECK(contains(text, ", ptr @str.0, [8 x i8] zeroinitializer }>"));
    CHECK(index_of(text, "@table =") < index_of(text, "@str.0 ="));
    arena_free(&a);
}

TEST(llvm_panic_with_a_computed_message) {
    // panic %a, %b calls the runtime's __rig_panic with them, as panic @g
    // does with g's address and length.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "fail", IR_VOID);
    s32 message = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    s32 count = ir_add_param(p, IR_I64, IR_EXT_NONE);
    Ir_Instr *panic = ir_emit(p, ir_new_block(&a, p), IR_PANIC, IR_VOID);
    panic->a = message;
    panic->b = count;
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "call void @__rig_panic(ptr %0, i64 %1)"));
        CHECK(contains(text, "define internal void @__rig_panic("));
    }
    arena_free(&a);
}

TEST(llvm_bool_in_memory_is_a_byte) {
    // A bool's slot is a byte. A store writes the i1 widened to 0 or 1; a
    // load reads the byte and compares it with 0, so any nonzero byte (one C
    // or a cast left there) is true. LLVM's own load i1 would be undefined
    // for any byte but 0 and 1. A bool global is a byte too, its first value
    // 0 or 1, read and written the same way through its address.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Global ready = {};
    ready.name = str("ready");
    ready.type = IR_I1;
    ready.initial = 1;
    array_add(&m->globals, ready);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_I1);
    s32 flag = ir_add_param(p, IR_I1, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    s32 slot = ir_emit(p, b, IR_LOCAL, IR_I1)->result;
    emit_binary(p, b, IR_STORE, IR_I1, flag, slot);
    Ir_Instr *load = ir_emit(p, b, IR_LOAD, IR_I1);
    load->a = slot;
    Ir_Instr *address = ir_emit(p, b, IR_ADDR, IR_PTR);
    address->target = 0;
    Ir_Instr *was = ir_emit(p, b, IR_LOAD, IR_I1);
    was->a = address->result;
    emit_binary(p, b, IR_STORE, IR_I1, load->result, address->result);
    s32 both = emit_binary(p, b, IR_AND, IR_I1, load->result, was->result);
    emit_ret(p, b, both);
    String text = llvm_text(&a, m, target_host());
    CHECK(contains(text, "alloca i8"));
    CHECK(contains(text, "zext i1 %0 to i8"));
    CHECK(contains(text, "store i8 %"));
    CHECK(contains(text, "load i8, ptr %"));
    CHECK(contains(text, "icmp ne i8 %"));
    CHECK(contains(text, "@ready = internal global i8 1"));
    CHECK(contains(text, "  %5 = load i8, ptr @ready, align 1\n"
                         "  %6 = icmp ne i8 %5, 0\n"
                         "  %7 = zext i1 %4 to i8\n"
                         "  store i8 %7, ptr @ready, align 1\n"));
    CHECK(!contains(text, "load i1"));
    CHECK(!contains(text, "store i1"));
    arena_free(&a);
}

TEST(llvm_memory_functions_win_over_the_programs) {
    // LLVM turns its memmove and memset intrinsics into calls to C's
    // memmove and memset (and memcpy, where it proves the two sides apart),
    // by name. So the three are declared first, as the runtime's write and
    // exit are: a program's own memset is the one renamed, and keeps working.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *own = new_proc_returning(m, &a, "memset", IR_VOID);
    emit_ret(own, ir_new_block(&a, own), -1);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    s32 at = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    ir_emit_call(m, p, b, 0, {});
    emit_zero(p, b, at, 8);
    emit_ret(p, b, -1);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "declare ptr @memset(ptr, i32, i64)"));
        CHECK(contains(text, "declare ptr @memmove(ptr, ptr, i64)"));
        CHECK(contains(text, "declare ptr @memcpy(ptr, ptr, i64)"));
        CHECK(contains(text, "define internal void @memset.1()"));
        CHECK(contains(text, "call void @memset.1()"));
    }
    // A module with no copy or zero declares none of them.
    String text = llvm_text(&a, max_module(&a), target_host());
    CHECK(!contains(text, "@memset"));
    CHECK(!contains(text, "@memmove"));
    CHECK(!contains(text, "@memcpy"));
    arena_free(&a);
}

TEST(llvm_stack_probe_wins_over_the_programs) {
    // On Windows LLVM calls __chkstk by name for a frame bigger than a page,
    // so the name is declared first: a program's own __chkstk is renamed.
    // Linux probes inline and declares nothing.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *own = new_proc_returning(m, &a, "__chkstk", IR_VOID);
    emit_ret(own, ir_new_block(&a, own), -1);
    String text = llvm_text(&a, m, TARGET_WINDOWS_X64);
    CHECK(contains(text, "declare void @__chkstk()"));
    CHECK(contains(text, "define internal void @__chkstk.1()"));
    text = llvm_text(&a, m, TARGET_LINUX_X64);
    CHECK(!contains(text, "declare void @__chkstk()"));
    CHECK(contains(text, "define internal void @__chkstk()"));
    arena_free(&a);
}

TEST(llvm_linux_functions_probe_the_stack) {
    // On Linux every function Rig defines probes its stack frame page by
    // page (clang's -fstack-clash-protection), so a frame bigger than a page
    // (a [100000]u8 local) can't step over the guard page below the stack
    // into other memory. Windows' __chkstk does that already.
    Arena a = arena_make();
    String text = llvm_text(&a, panic_module(&a), TARGET_LINUX_X64);
    CHECK(contains(text, "\"probe-stack\"=\"inline-asm\""));
    CHECK(contains(text, "define internal void @program() #"));
    CHECK(contains(text, "define internal void @__rig_panic(ptr %0, i64 %1) #"));
    text = llvm_text(&a, panic_module(&a), TARGET_WINDOWS_X64);
    CHECK(!contains(text, "probe-stack"));
    arena_free(&a);
}

// Milestone 2c (issue #71): line tables. A module lowered from a source file
// gets debug information: a compile unit naming the file, a subprogram per
// Rig procedure, a lexical block per scope, and a location on each
// instruction, which LLVM writes as DWARF (Linux) or CodeView (Windows). In
// LLVM's text each is a metadata node, !N, defined on a line of its own at
// the end ("!7 = !DILocation(line: 4, column: 5, scope: !3)"), and an
// instruction names its location as an attachment ("call ..., !dbg !7").

// A file with a call on line 4 and another in an if's block, on line 6.
// Its MD5 is 2d6d83719e184598cb6b6bd6505ea090 (md5sum of these bytes).
static const char *LINES =
    "puts :: (s: *u8) -> s32 #foreign libc;\n"
    "main :: () {\n"
    "    x := 1;\n"
    "    puts(\"one\");\n"
    "    if x > 0 {\n"
    "        puts(\"two\");\n"
    "    }\n"
    "}\n";

// The metadata node a line names after `label` ("!dbg", "scope:"): "!7"
// from "call void @f(), !dbg !7"; {} when it names none.
static String attached(Arena *a, String line, const char *label) {
    s64 at = index_of(line, string_cstr(a, string_printf(a, "%s !", label)));
    if (at < 0) return {};
    s64 start = at + (s64)strlen(label) + 1, end = start + 1;
    while (end < line.count && line.data[end] >= '0' && line.data[end] <= '9') end++;
    return string_slice(line, start, end - start);
}

// The definition of metadata node `node` ("!7"): its line, from "!7 = ".
static String node_line(Arena *a, String text, String node) {
    return line_with(text, string_cstr(a, string_printf(a, "\n" STR_FMT " = ", STR_ARG(node))));
}

// A string as LLVM's text writes it inside quotes: a backslash doubled (so
// Windows' C:\dev is C:\\dev), a quote and anything unprintable as \XX in
// hex.
static String llvm_escaped(Arena *a, String s) {
    String out = string_printf(a, "");
    for (s64 i = 0; i < s.count; i++) {
        u8 c = s.data[i];
        if (c == '\\') {
            out = string_printf(a, STR_FMT "\\\\", STR_ARG(out));
        } else if (c >= 0x20 && c < 0x7f && c != '"') {
            out = string_printf(a, STR_FMT "%c", STR_ARG(out), c);
        } else {
            out = string_printf(a, STR_FMT "\\%02X", STR_ARG(out), c);
        }
    }
    return out;
}

TEST(llvm_debug_info_has_a_compile_unit) {
    // One compile unit, as C99 (whose expression syntax a debugger then
    // uses, and which suits Rig's), produced by "rig 0.1.0 (3f9c2a1b7d4e)
    // (LLVM X.Y.Z)": this build's version and commit, from the build
    // scripts' defines, and the LLVM rig is linked with (issue #104: no
    // "clang", which nothing needs). The file is the source's absolute
    // path, in the directory rig ran in, with its MD5. Module flags say
    // which debug information LLVM writes: CodeView on Windows, DWARF 5 on
    // Linux.
    Arena a = arena_make();
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
    String commit = str(RIG_COMMIT);
    if (commit.count == 40) commit = string_printf(&a, "%.12s%s", RIG_COMMIT, RIG_COMMIT_DIRTY ? "-dirty" : "");
    String producer = string_printf(&a, "producer: \"rig %s (" STR_FMT ") (LLVM %u.%u.%u)\"", RIG_VERSION,
                                    STR_ARG(commit), major, minor, patch);
    Ir_Module *m = lower_source(&a, LINES);
    for (int t = 0; t < 2; t++) {
        Target target = (Target)t;
        String text = llvm_text(&a, m, target);
        CHECK(contains(text, "!DICompileUnit(language: DW_LANG_C99"));
        CHECK(contains(text, string_cstr(&a, producer)));
        CHECK(contains(text, "isOptimized: false"));
        CHECK(contains(text, "emissionKind: FullDebug"));
        String file = string_printf(&a, "!DIFile(filename: \"" STR_FMT "\", directory: \"" STR_FMT "\"",
                                    STR_ARG(llvm_escaped(&a, m->sources[0].path)),
                                    STR_ARG(llvm_escaped(&a, m->directory)));
        CHECK(contains(text, string_cstr(&a, file)));
        CHECK(contains(text, "checksumkind: CSK_MD5, checksum: \"2d6d83719e184598cb6b6bd6505ea090\""));
        CHECK(contains(text, "!\"Debug Info Version\", i32 3}"));
        if (target == TARGET_WINDOWS_X64) {
            CHECK(contains(text, "!\"CodeView\", i32 1}"));
            CHECK(!contains(text, "Dwarf Version"));
        } else {
            CHECK(contains(text, "!\"Dwarf Version\", i32 5}"));
            CHECK(!contains(text, "CodeView"));
        }
    }
    arena_free(&a);
}

TEST(llvm_debug_file_names_keep_a_share) {
    // Issue #104: the names of the source file and the directory that LLVM
    // is given, for the path given to rig build in the directory it ran in.
    // On Windows a file on a network share (a UNC path) is named with a
    // '/' first, /\server\share\dir\a.rig, a name LLVM's CodeView writer
    // keeps whole (it made \\server\share\... into C:\server\share\... after
    // the directory's drive, or the directory and the name end to end) and
    // Windows and cdb read as the share's. A drive path, a
    // drive-relative path, a relative one and the directory are as they
    // are; on Linux nothing changes.
    Arena a = arena_make();
    struct Case {
        const char *directory, *given;   // rig's working directory, and the path rig build is given
        const char *file, *in_directory; // what LLVM gets on Windows
    };
    Case cases[] = {
        {"C:\\work", "\\\\server\\share\\dir\\a.rig", "/\\server\\share\\dir\\a.rig", "C:\\work"},
        {"C:\\work", "//server/share/dir/a.rig", "/\\server\\share\\dir\\a.rig", "C:\\work"},
        {"C:\\work", "C:\\dev\\a.rig", "C:\\dev\\a.rig", "C:\\work"},
        {"C:\\work", "C:a.rig", "C:\\work\\a.rig", "C:\\work"},
        {"C:\\work", "a.rig", "C:\\work\\a.rig", "C:\\work"},
        {"\\\\server\\share\\dir", "sub\\a.rig", "/\\server\\share\\dir\\sub\\a.rig", "\\\\server\\share\\dir"},
        {"\\\\server\\share\\dir", "..\\a.rig", "/\\server\\share\\a.rig", "\\\\server\\share\\dir"},
    };
    for (Case &c : cases) {
        String path = path_absolute(&a, str(c.directory), str(c.given), true);
        String file = {}, directory = {};
        llvm_debug_file_names(&a, path, str(c.directory), TARGET_WINDOWS_X64, &file, &directory);
        CHECK_STR(file, c.file);
        CHECK_STR(directory, c.in_directory);
        llvm_debug_file_names(&a, path, str(c.directory), TARGET_LINUX_X64, &file, &directory);
        CHECK(string_eq(file, path));
        CHECK_STR(directory, c.directory);
    }
    // In the module's debug information: the file's name as LLVM got it.
    Ir_Module *m = lower_source(&a, LINES);
    m->sources[0].path = str("\\\\server\\share\\dir\\lines.rig");
    m->directory = str("\\\\server\\share\\dir");
    String text = llvm_text(&a, m, TARGET_WINDOWS_X64);
    String file = string_printf(&a, "!DIFile(filename: \"" STR_FMT "\", directory: \"" STR_FMT "\"",
                                STR_ARG(llvm_escaped(&a, str("/\\server\\share\\dir\\lines.rig"))),
                                STR_ARG(llvm_escaped(&a, m->directory)));
    CHECK(contains(text, string_cstr(&a, file)));
    arena_free(&a);
}

TEST(llvm_procedures_have_subprograms) {
    // Each Rig procedure is a subprogram named as in the source, with no
    // linkage name: LLVM renamed the Rig memset memset.1 (C's memset was
    // declared first, for the struct's zeroing), and a debugger must still
    // call it memset. The procedures the compiler makes have none.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Point :: struct { x: int; y: int; }\n"
                                "memset :: () { }\n"
                                "get :: (a: []int, i: int) -> int {\n"
                                "    return a[i];\n"
                                "}\n"
                                "main :: () {\n"
                                "    p: Point;\n"
                                "    memset();\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "define internal void @memset.1() #"));
        CHECK(contains(text, "!DISubprogram(name: \"memset\", scope: "));
        CHECK(contains(text, "!DISubprogram(name: \"get\", scope: "));
        CHECK(!contains(text, "linkageName"));
        // main: exported, so not local to the unit; its line and its body's.
        String main = line_with(text, "!DISubprogram(name: \"main\"");
        CHECK(contains(main, "line: 6, "));
        CHECK(contains(main, "scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, "));
        String memset = line_with(text, "!DISubprogram(name: \"memset\"");
        CHECK(contains(memset, "spFlags: DISPFlagLocalToUnit | DISPFlagDefinition, "));
        // Each definition names its subprogram; the compiler's don't.
        CHECK(attached(&a, line_with(text, "define i32 @main()"), "!dbg").count > 0);
        CHECK(attached(&a, line_with(text, "define internal void @memset.1()"), "!dbg").count > 0);
        String bounds = line_with(text, "define internal void @__rig_bounds_failed(");
        CHECK(bounds.count > 0);
        CHECK(!contains(bounds, "!dbg"));
        String panic = line_with(text, "define internal void @__rig_panic(");
        CHECK(panic.count > 0);
        CHECK(!contains(panic, "!dbg"));
        CHECK(!contains(text, "name: \"__rig"));
    }
    arena_free(&a);
}

TEST(llvm_instructions_have_their_lines) {
    // A call is at its statement's line and column (Windows: no column, as
    // clang-cl writes CodeView, where a change of column starts a new
    // entry), in its procedure's subprogram; one inside an if's block is in
    // that block's lexical block, inside the subprogram. The stack slots,
    // hoisted to the entry block, have no location.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, LINES);
    for (int t = 0; t < 2; t++) {
        Target target = (Target)t;
        String text = llvm_text(&a, m, target);
        String one = node_line(&a, text, attached(&a, line_with(text, "call i32 @puts(ptr @str.0)"), "!dbg"));
        CHECK(contains(one, "!DILocation(line: 4, "));
        if (target == TARGET_LINUX_X64) {
            CHECK(contains(one, "!DILocation(line: 4, column: 5, scope: "));
        } else {
            CHECK(!contains(one, "column: "));
        }
        String main = attached(&a, line_with(text, "define i32 @main()"), "!dbg");
        CHECK(main.count > 0);
        CHECK(contains(one, string_cstr(&a, string_printf(&a, "scope: " STR_FMT ")", STR_ARG(main)))));

        String two = node_line(&a, text, attached(&a, line_with(text, "call i32 @puts(ptr @str.1)"), "!dbg"));
        CHECK(contains(two, "!DILocation(line: 6, "));
        String block = node_line(&a, text, attached(&a, two, "scope:"));
        CHECK(contains(block, string_cstr(&a, string_printf(&a, "!DILexicalBlock(scope: " STR_FMT ", ", STR_ARG(main)))));
        CHECK(contains(block, "line: 5, "));

        String alloca = line_with(text, "alloca");
        CHECK(alloca.count > 0);
        CHECK(!contains(alloca, "!dbg"));
    }
    arena_free(&a);
}

// The functions of `text` (LLVM's text of a module) that are defined
// without "frame-pointer"="all" in their attribute group, their define
// lines one after another; with *defined set to how many there are. A
// definition names its group after its parameters ("define i32 @main() #0
// !dbg !5 {"), which is a line of its own ("attributes #0 = { ... }").
static String without_frame_pointers(Arena *a, String text, s64 *defined) {
    String out = string_printf(a, "");
    *defined = 0;
    for (s64 at = index_of(text, "\ndefine "); at >= 0;) {
        String line = line_with(string_slice(text, at, text.count - at), "\ndefine ");
        (*defined)++;
        String group = {};
        s64 hash = index_of(line, ") #");
        if (hash >= 0) {
            s64 start = hash + 2, end = start + 1;
            while (end < line.count && line.data[end] >= '0' && line.data[end] <= '9') end++;
            group = string_slice(line, start, end - start);
        }
        String attributes = {};
        if (group.count > 1) {
            attributes = line_with(text, string_cstr(a, string_printf(a, "\nattributes " STR_FMT " = {", STR_ARG(group))));
        }
        if (!contains(attributes, "\"frame-pointer\"=\"all\"")) {
            out = string_printf(a, STR_FMT STR_FMT "\n", STR_ARG(out), STR_ARG(line));
        }
        s64 next = index_of(string_slice(text, at + 1, text.count - at - 1), "\ndefine ");
        at = next < 0 ? -1 : at + 1 + next;
    }
    return out;
}

TEST(llvm_functions_keep_frame_pointers) {
    // Every function the module defines keeps its frame pointer (rbp), so a
    // debugger or a profiler can walk the stack from frame to frame:
    // lowered or built by hand, Rig's own and the runtime's. Each define's
    // own attribute group says so.
    Arena a = arena_make();
    for (int t = 0; t < 2; t++) {
        s64 defined = 0;
        String text = llvm_text(&a, lower_source(&a, LINES), (Target)t);
        CHECK(contains(text, "define i32 @main() #"));
        String missing = without_frame_pointers(&a, text, &defined);
        CHECK(defined >= 1);
        if (missing.count) printf("    no frame pointer:\n" STR_FMT, STR_ARG(missing));
        CHECK(missing.count == 0);

        text = llvm_text(&a, panic_module(&a), (Target)t);
        CHECK(contains(text, "define internal void @program() #"));
        CHECK(contains(text, "define internal void @__rig_panic(ptr %0, i64 %1) #"));
        missing = without_frame_pointers(&a, text, &defined);
        CHECK(defined == 2);  // program and __rig_panic
        if (missing.count) printf("    no frame pointer:\n" STR_FMT, STR_ARG(missing));
        CHECK(missing.count == 0);
    }
    arena_free(&a);
}

TEST(llvm_hand_built_modules_have_no_debug_info) {
    // A module built by hand has no source file, so no debug information at
    // all: no compile unit, no flag, no location.
    Arena a = arena_make();
    for (int t = 0; t < 2; t++) {
        Ir_Module *modules[] = {max_module(&a), panic_module(&a)};
        for (Ir_Module *m : modules) {
            String text = llvm_text(&a, m, (Target)t);
            CHECK(text.count > 0);
            CHECK(!contains(text, "!dbg"));
            CHECK(!contains(text, "DICompileUnit"));
            CHECK(!contains(text, "Debug Info Version"));
        }
    }
    arena_free(&a);
}

// Milestone 2c (issue #72): types and variables. Each Rig type a variable
// has is a debug type (DIBasicType for a number or a bool, DIDerivedType
// for a pointer or a struct's member, DICompositeType for a struct or an
// array), each variable a DILocalVariable declared on its stack slot
// (#dbg_declare), each global a DIGlobalVariable attached to it.

// The metadata node a line defines: "!25" from "!25 = !DILocalVariable(...".
static String defined_node(String line) {
    s64 at = index_of(line, " = ");
    return at < 0 ? String{} : string_slice(line, 0, at);
}

TEST(llvm_debug_types_of_every_kind) {
    // Every kind of type, the ones that refer to themselves and the ones
    // with no size included: the module verifies (llvm_text checks it), and
    // a struct is in the debug information once.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Node :: struct { value: int; next: *Node; }\n"
                                "Tree :: struct { children: []Tree; }\n"
                                "Empty :: struct {}\n"
                                "Mode :: enum { A; B :: 5; }\n"
                                "Small :: enum u8 { X :: 200; }\n"
                                "Huge :: enum u64 { TOP :: 18446744073709551615; }\n"
                                "Signed :: enum s8 { LOW :: -2; }\n"
                                "main :: () {\n"
                                "    mode: Mode;\n"
                                "    tiny: Small;\n"
                                "    huge: Huge;\n"
                                "    low: Signed;\n"
                                "    n: Node;\n"
                                "    t: Tree;\n"
                                "    e: Empty;\n"
                                "    three: [3]int;\n"
                                "    none: [0]int;\n"
                                "    big: [65536]u8;\n"
                                "    p: *void;\n"
                                "    yes := true;\n"
                                "    f: float64;\n"
                                "    g: float32;\n"
                                "    small: s8;\n"
                                "    wide: u32;\n"
                                "    v: []int;\n"
                                "    s := \"hi\";\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "!DIBasicType(name: \"int\", size: 64, encoding: DW_ATE_signed)"));
        CHECK(contains(text, "!DIBasicType(name: \"s8\", size: 8, encoding: DW_ATE_signed)"));
        CHECK(contains(text, "!DIBasicType(name: \"u32\", size: 32, encoding: DW_ATE_unsigned)"));
        CHECK(contains(text, "!DIBasicType(name: \"u8\", size: 8, encoding: DW_ATE_unsigned_char)"));
        CHECK(contains(text, "!DIBasicType(name: \"bool\", size: 8, encoding: DW_ATE_boolean)"));
        CHECK(contains(text, "!DIBasicType(name: \"float64\", size: 64, encoding: DW_ATE_float)"));
        CHECK(contains(text, "!DIBasicType(name: \"float32\", size: 32, encoding: DW_ATE_float)"));
        // A struct that points at itself is made once, its pointer member
        // pointing back at the same node.
        CHECK(count_of(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Node\"") == 1);
        String node = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Node\""));
        CHECK(node.count > 0);
        String next = line_with(text, "!DIDerivedType(tag: DW_TAG_member, name: \"next\"");
        CHECK(contains(next, "offset: 64)"));
        String pointer = node_line(&a, text, attached(&a, next, "baseType:"));
        CHECK(contains(pointer, "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: "));
        CHECK(string_eq(attached(&a, pointer, "baseType:"), node));
        // A struct holding a view of itself, and views and strings, structs
        // of their own named as Rig names them.
        CHECK(count_of(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Tree\"") == 1);
        CHECK(contains(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"[]Tree\""));
        CHECK(contains(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"[]int\""));
        CHECK(contains(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"string\""));
        CHECK(contains(text, "!DIDerivedType(tag: DW_TAG_member, name: \"count\""));
        CHECK(contains(line_with(text, "!DIDerivedType(tag: DW_TAG_member, name: \"data\""), "offset: 64)"));
        // A struct with no fields: no size.
        CHECK(contains(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Empty\""));
        // Arrays, of 3, none and many.
        CHECK(contains(text, "!DICompositeType(tag: DW_TAG_array_type"));
        CHECK(contains(text, "!DISubrange(count: 3, lowerBound: 0)"));
        CHECK(contains(text, "!DISubrange(count: 0, lowerBound: 0)"));
        CHECK(contains(text, "!DISubrange(count: 65536, lowerBound: 0)"));
        // *void points at no type, as C's void * does.
        CHECK(contains(text, "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64, "));
        // An enum is an enumeration of its members, in order, over its
        // integer type (spec 10.7): an unsigned one's values are unsigned,
        // u64's largest included. It is at its declaration's file and line,
        // as a struct is.
        String mode = line_with(text, "!DICompositeType(tag: DW_TAG_enumeration_type, name: \"Mode\"");
        CHECK(contains(mode, "size: 64"));
        CHECK(contains(mode, "line: 4,"));
        String node_file = attached(&a, line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Node\""),
                                    "file:");
        CHECK(node_file.count > 0 && string_eq(attached(&a, mode, "file:"), node_file));
        CHECK(contains(node_line(&a, text, attached(&a, mode, "baseType:")), "!DIBasicType(name: \"int\""));
        String mode_members = node_line(&a, text, attached(&a, mode, "elements:"));
        String a_member = defined_node(line_with(text, "!DIEnumerator(name: \"A\", value: 0)"));
        String b_member = defined_node(line_with(text, "!DIEnumerator(name: \"B\", value: 5)"));
        CHECK(a_member.count > 0 && b_member.count > 0);
        CHECK(contains(mode_members,
                       string_cstr(&a, string_printf(&a, "!{" STR_FMT ", " STR_FMT "}", STR_ARG(a_member),
                                                     STR_ARG(b_member)))));
        String small = line_with(text, "!DICompositeType(tag: DW_TAG_enumeration_type, name: \"Small\"");
        CHECK(contains(small, "size: 8"));
        CHECK(contains(node_line(&a, text, attached(&a, small, "baseType:")), "!DIBasicType(name: \"u8\""));
        CHECK(contains(text, "!DIEnumerator(name: \"X\", value: 200, isUnsigned: true)"));
        CHECK(contains(line_with(text, "!DICompositeType(tag: DW_TAG_enumeration_type, name: \"Huge\""),
                       "size: 64"));
        CHECK(contains(text, "!DIEnumerator(name: \"TOP\", value: 18446744073709551615, isUnsigned: true)"));
        // A signed one's values are signed, the checker's 64 bits cut to
        // the enum's width (an s32's kept at 64, for cdb: debug_enum), so
        // an s8's -2 is -2, not 254. The text doesn't show the width.
        String low = line_with(text, "!DICompositeType(tag: DW_TAG_enumeration_type, name: \"Signed\"");
        CHECK(contains(low, "size: 8"));
        CHECK(contains(node_line(&a, text, attached(&a, low, "baseType:")), "!DIBasicType(name: \"s8\""));
        CHECK(contains(text, "!DIEnumerator(name: \"LOW\", value: -2)"));
    }
    arena_free(&a);
}

// The line defining the struct type named `name`, the first if several are.
static String struct_line(Arena *a, String text, const char *name) {
    return line_with(text, string_cstr(a, string_printf(a, "DW_TAG_structure_type, name: \"%s\", ", name)));
}

TEST(llvm_debug_types_stop_at_a_depth) {
    // Types nest without a limit the checker sets in two ways: structs held
    // by value, each declared after the one it holds (the checker lays each
    // out before the next needs it), and structs holding a pointer to the
    // next (which needs no layout at all). Made a call per level, their
    // debug types overflowed the compiler's stack past 10000 levels or so,
    // with no message. Now a type has MAX_DEPTH levels at most, and below
    // them a type is a stand-in (debug_type): 50000 levels of each build,
    // and an object is written for both targets (LLVM's code generator
    // walks the debug types too, to write CodeView or DWARF).
    //
    // In the first program, c's type is made from the top: S50000 down to
    // S49002 are defined, and S49001, MAX_DEPTH - 1 levels below S50000, is
    // a declaration. b's S1500 is made after a's S1000, which has MAX_DEPTH
    // levels of its own (S1000 down to S2, and S1 declared): so S1001 holds
    // a declaration of S1000, and no chain has more levels than MAX_DEPTH,
    // wherever it's made (with no cycles to pass through: see
    // llvm_debug_types_through_cycles_are_not_cut). In the second, P0's next is a pointer to P1, a
    // level each, down to P499, whose next is a pointer to no type.
    Arena a = arena_make();
    os_make_directory(str(".build"));
    const char *programs[] = {
        joined(&a, "S0 :: struct { x: int; }\n", chain_of(&a, "S%lld :: struct { s: S%lld; }\n", 1, 50000, -1),
               "main :: () { a : S1000; b : S1500; c : S50000; d : S25000; }\n"),
        joined(&a, chain_of(&a, "P%lld :: struct { next: *P%lld; }\n", 0, 49999, 1),
               "P50000 :: struct { x: int; }\nmain :: () { p : P0; }\n"),
    };
    const char *paths[] = {".build/deep-types-windows.obj", ".build/deep-types-linux.o"};
    for (s64 k = 0; k < 2; k++) {
        CHECK(check_source(&a, programs[k]).count == 0);
        Ir_Module *m = lower_source(&a, programs[k]);
        for (int t = 0; t < 2; t++) {
            String text = llvm_text(&a, m, (Target)t);
            if (k == 0) {
                const char *cut = string_cstr(&a, string_printf(&a, "S%d", 50000 - (MAX_DEPTH - 1)));
                CHECK(contains(struct_line(&a, text, "S50000"), ", elements: !"));
                CHECK(contains(struct_line(&a, text, cut), ", flags: DIFlagFwdDecl)"));
                CHECK(contains(struct_line(&a, text, "S1000"), ", elements: !"));
                CHECK(count_of(text, "DW_TAG_structure_type, name: \"S1000\", ") == 2);
                CHECK(contains(struct_line(&a, text, "S1"), ", flags: DIFlagFwdDecl)"));
            } else {
                const char *last = string_cstr(&a, string_printf(&a, "P%d", (MAX_DEPTH - 2) / 2));
                const char *beyond = string_cstr(&a, string_printf(&a, "P%d", (MAX_DEPTH - 2) / 2 + 1));
                String node = defined_node(struct_line(&a, text, last));
                CHECK(node.count > 0);
                String scope = string_printf(&a, "name: \"next\", scope: " STR_FMT ", ", STR_ARG(node));
                String member = line_with(text, string_cstr(&a, scope));
                String pointer = node_line(&a, text, attached(&a, member, "baseType:"));
                CHECK(contains(pointer, "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64, "));
                CHECK(struct_line(&a, text, beyond).count == 0);
            }
            String error = {};
            bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), false, &a, &error);
            CHECK(ok);
            if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
        }
    }
    arena_free(&a);
}

TEST(llvm_debug_types_through_cycles_are_not_cut) {
    // A debug type's count of levels takes a struct still being made, which
    // a part leads back to, as one level, then and after (debug_type): so
    // what MAX_DEPTH bounds is debug_type's recursion, not every chain in
    // the metadata. Here Ak holds a *Bk, and Bk a *Ak back and, by value, a
    // chain of MAX_DEPTH - 10 structs Ck_i down to Ck_0, which holds a
    // *A(k-1). Made a variable at a time, v0 : A0 first, each Ak finds
    // A(k-1) made already, and every count stays under MAX_DEPTH: nothing
    // is cut (no declaration, no pointer to no type), though the chain from
    // A20 down to A0 is about 20 * MAX_DEPTH types long. The module
    // verifies, and an object is written for both targets.
    const s64 links = 20;
    const s64 between = MAX_DEPTH - 10;
    Arena a = arena_make();
    os_make_directory(str(".build"));
    const char *program = "A0 :: struct { p: *B0; }\nB0 :: struct { back: *A0; x: int; }\n";
    for (s64 k = 1; k <= links; k++) {
        const char *chain = string_cstr(&a, string_printf(&a, "C%lld_%%lld :: struct { c: C%lld_%%lld; }\n",
                                                           (long long)k, (long long)k));
        program = joined(&a, program,
                         string_cstr(&a, string_printf(&a, "C%lld_0 :: struct { q: *A%lld; }\n", (long long)k,
                                                       (long long)(k - 1))),
                         chain_of(&a, chain, 1, between, -1),
                         string_cstr(&a, string_printf(&a,
                                                       "A%lld :: struct { p: *B%lld; }\n"
                                                       "B%lld :: struct { back: *A%lld; c: C%lld_%lld; }\n",
                                                       (long long)k, (long long)k, (long long)k, (long long)k,
                                                       (long long)k, (long long)between)));
    }
    program = joined(&a, program, "main :: () {\n", chain_of(&a, "    v%lld : A%lld;\n", 0, links, 0), "}\n");
    CHECK(check_source(&a, program).count == 0);
    Ir_Module *m = lower_source(&a, program);
    const char *paths[] = {".build/cycle-types-windows.obj", ".build/cycle-types-linux.o"};
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        const char *last = string_cstr(&a, string_printf(&a, "A%lld", (long long)links));
        CHECK(contains(struct_line(&a, text, last), ", elements: !"));
        CHECK(contains(struct_line(&a, text, "C1_0"), ", elements: !"));
        CHECK(count_of(text, "DIFlagFwdDecl") == 0);
        CHECK(count_of(text, "baseType: null") == 0);
        String error = {};
        bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), false, &a, &error);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    }
    arena_free(&a);
}

// The node of the 64-bit pointer to debug type `node` ("!12"); {} when
// there's none.
static String pointer_node(Arena *a, String text, String node) {
    return defined_node(line_with(
        text, string_cstr(a, string_printf(a, "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: " STR_FMT ", size: 64,",
                                           STR_ARG(node)))));
}

TEST(llvm_variables_are_declared) {
    // Each variable is declared on its stack slot (#dbg_declare), in its
    // scope: a local; a scalar parameter, stored into a slot of its own; an
    // aggregate parameter, whose slot holds the pointer to the caller's copy,
    // a pointer to its type, the slot itself (declare_variables says why),
    // on both systems; a bool parameter,
    // stored as a byte; a loop's it and it_index, in the loop's lexical
    // block.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "sum :: (a: []int, k: int, b: bool) -> int {\n"
                                "    x := 0;\n"
                                "    for a {\n"
                                "        x += it * k;\n"
                                "    }\n"
                                "    return x;\n"
                                "}\n"
                                "main :: () {\n"
                                "    numbers := int.[1, 2];\n"
                                "    n := sum(numbers, 3, true);\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String sum = attached(&a, line_with(text, "define internal i64 @sum("), "!dbg");
        String main = attached(&a, line_with(text, "define i32 @main("), "!dbg");
        CHECK(sum.count > 0 && main.count > 0);
        enum Where { IN_SUM, IN_LOOP, IN_MAIN };
        struct Expected {
            const char *variable;    // how its DILocalVariable starts
            const char *expression;  // its declare's
            Where scope;
        };
        Expected expected[] = {
            {"!DILocalVariable(name: \"a\", arg: 1, ", "!DIExpression()", IN_SUM},
            {"!DILocalVariable(name: \"k\", arg: 2, ", "!DIExpression()", IN_SUM},
            {"!DILocalVariable(name: \"b\", arg: 3, ", "!DIExpression()", IN_SUM},
            {"!DILocalVariable(name: \"x\", scope: ", "!DIExpression()", IN_SUM},
            {"!DILocalVariable(name: \"it\", scope: ", "!DIExpression()", IN_LOOP},
            {"!DILocalVariable(name: \"it_index\", scope: ", "!DIExpression()", IN_LOOP},
            {"!DILocalVariable(name: \"numbers\", scope: ", "!DIExpression()", IN_MAIN},
            {"!DILocalVariable(name: \"n\", scope: ", "!DIExpression()", IN_MAIN},
        };
        for (Expected &e : expected) {
            String variable = line_with(text, e.variable);
            if (variable.count == 0) printf("    no %s\n", e.variable);
            CHECK(variable.count > 0);
            if (variable.count == 0) continue;
            String declare = string_printf(&a, ", " STR_FMT ", %s, !", STR_ARG(defined_node(variable)), e.expression);
            CHECK(contains(line_with(text, string_cstr(&a, declare)), "#dbg_declare(ptr %"));
            String scope = attached(&a, variable, "scope:");
            if (e.scope == IN_LOOP) {
                String block = node_line(&a, text, scope);
                CHECK(contains(block, string_cstr(&a, string_printf(&a, "!DILexicalBlock(scope: " STR_FMT ", ",
                                                                    STR_ARG(sum)))));
                CHECK(contains(block, "line: 3, "));
            } else {
                CHECK(string_eq(scope, e.scope == IN_SUM ? sum : main));
            }
        }
        // a's type: a 64-bit pointer to the view, the slot being the
        // variable.
        String view = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"[]int\""));
        String view_type = attached(&a, line_with(text, "!DILocalVariable(name: \"a\", arg: 1, "), "type:");
        CHECK(view.count > 0 && view_type.count > 0);
        String pointer = node_line(&a, text, view_type);
        CHECK(contains(pointer, "!DIDerivedType(tag: DW_TAG_pointer_type, "));
        CHECK(contains(pointer, string_cstr(&a, string_printf(&a, "baseType: " STR_FMT ", size: 64",
                                                             STR_ARG(view)))));
        CHECK(!contains(text, "DW_OP_deref"));
        // b, a bool, is an i1 in LLVM: its slot is an i8, as a bool in
        // memory is, and it's stored zero-extended, so the byte a debugger
        // reads is 0 or 1 (an i1 stored as is leaves the other 7 bits
        // undefined).
        String flag = defined_node(line_with(text, "!DILocalVariable(name: \"b\", arg: 3, "));
        String declare = line_with(text, string_cstr(&a, string_printf(&a, ", " STR_FMT ", !DIExpression(), !",
                                                                       STR_ARG(flag))));
        s64 from = index_of(declare, "#dbg_declare(ptr ") + (s64)strlen("#dbg_declare(ptr ");
        s64 to = index_of(declare, ", !");
        CHECK(flag.count > 0 && from > 0 && to > from);
        if (to > from) {
            String slot = string_slice(declare, from, to - from);
            CHECK(contains(text, string_cstr(&a, string_printf(&a, "  " STR_FMT " = alloca i8, align 1\n",
                                                                STR_ARG(slot)))));
            String store = line_with(text, string_cstr(&a, string_printf(&a, ", ptr " STR_FMT ", align 1\n",
                                                                         STR_ARG(slot))));
            CHECK(contains(store, "  store i8 %"));
            s64 value_from = index_of(store, "store i8 ") + (s64)strlen("store i8 ");
            s64 value_to = index_of(store, ", ptr ");
            String stored = string_slice(store, value_from, value_to > value_from ? value_to - value_from : 0);
            CHECK(contains(text, string_cstr(&a, string_printf(&a, "\n  " STR_FMT " = zext i1 %%",
                                                                STR_ARG(stored)))));
        }
        // sum's type: its result's, then its parameters', int, a pointer to
        // []int (how the view is passed), int, bool.
        String type = node_line(&a, text, attached(&a, line_with(text, "!DISubprogram(name: \"sum\""), "type:"));
        String types = node_line(&a, text, attached(&a, type, "types:"));
        String integer = defined_node(line_with(text, "!DIBasicType(name: \"int\""));
        String boolean = defined_node(line_with(text, "!DIBasicType(name: \"bool\""));
        String list = string_printf(&a, " = !{" STR_FMT ", " STR_FMT ", " STR_FMT ", " STR_FMT "}", STR_ARG(integer),
                                    STR_ARG(pointer_node(&a, text, view)), STR_ARG(integer), STR_ARG(boolean));
        CHECK(contains(types, string_cstr(&a, list)));
        // main returns nothing in Rig, so its type's first is null.
        type = node_line(&a, text, attached(&a, line_with(text, "!DISubprogram(name: \"main\""), "type:"));
        CHECK(contains(node_line(&a, text, attached(&a, type, "types:")), "!{null}"));
    }
    arena_free(&a);
}

// The source of the tests of procedures taking and returning aggregates.
static const char *AGGREGATE_CALLS = "Pair :: struct { a: int; b: int; }\n"
                                     "make :: (n: int) -> Pair { return Pair.{n, n}; }\n"
                                     "sum :: (p: Pair, s: string) -> int { return p.a; }\n"
                                     "both :: (n: int) -> Pair, int { return Pair.{n, n}, n; }\n"
                                     "twice :: (n: int) -> int, int { return n, n; }\n"
                                     "pairs :: (n: [2]int) -> Pair, Pair { return Pair.{1, 2}, Pair.{3, 4}; }\n"
                                     "row :: (n: int) -> [2]int { return .[n, n]; }\n"
                                     "main :: () {\n"
                                     "    q := make(1);\n"
                                     "    r := row(2);\n"
                                     "    b, i := both(3);\n"
                                     "}\n";

TEST(llvm_procedure_types_say_how_they_are_called) {
    // A procedure's debug type is how it's called (spec 10.4), not how C
    // would pass the same types, alike in DWARF and CodeView: an aggregate
    // parameter is a pointer to the caller's copy, and an aggregate result
    // a hidden pointer parameter before the visible ones, so the procedure
    // returns its one scalar result, or nothing. With several scalar
    // results it returns nothing a C type describes, so none either.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, AGGREGATE_CALLS);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String integer = defined_node(line_with(text, "!DIBasicType(name: \"int\""));
        String pair = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Pair\""));
        String string = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"string\""));
        String array = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_array_type, baseType: "));
        String to_pair = pointer_node(&a, text, pair);
        String to_array = pointer_node(&a, text, array);
        CHECK(integer.count > 0 && pair.count > 0 && string.count > 0 && array.count > 0 && to_pair.count > 0 &&
              to_array.count > 0);
        struct Expected {
            const char *name;
            String types;  // the list's elements
        };
        Expected expected[] = {
            {"make", string_printf(&a, "null, " STR_FMT ", " STR_FMT, STR_ARG(to_pair), STR_ARG(integer))},
            {"sum", string_printf(&a, STR_FMT ", " STR_FMT ", " STR_FMT, STR_ARG(integer), STR_ARG(to_pair),
                                  STR_ARG(pointer_node(&a, text, string)))},
            {"both", string_printf(&a, STR_FMT ", " STR_FMT ", " STR_FMT, STR_ARG(integer), STR_ARG(to_pair),
                                   STR_ARG(integer))},
            {"twice", string_printf(&a, "null, " STR_FMT, STR_ARG(integer))},
            {"pairs", string_printf(&a, "null, " STR_FMT ", " STR_FMT ", " STR_FMT, STR_ARG(to_pair), STR_ARG(to_pair),
                                    STR_ARG(to_array))},
            {"row", string_printf(&a, "null, " STR_FMT ", " STR_FMT, STR_ARG(to_array), STR_ARG(integer))},
        };
        for (Expected &e : expected) {
            String subprogram =
                line_with(text, string_cstr(&a, string_printf(&a, "!DISubprogram(name: \"%s\"", e.name)));
            String type = node_line(&a, text, attached(&a, subprogram, "type:"));
            String types = node_line(&a, text, attached(&a, type, "types:"));
            String list = string_printf(&a, " = !{" STR_FMT "}", STR_ARG(e.types));
            if (!contains(types, string_cstr(&a, list))) {
                printf("    %s: " STR_FMT ", not" STR_FMT "\n", e.name, STR_ARG(types), STR_ARG(list));
            }
            CHECK(contains(types, string_cstr(&a, list)));
        }
        // No type is marked passed by reference: the pointers say it.
        CHECK(!contains(text, "DIFlagTypePassByReference"));
    }
    arena_free(&a);
}

TEST(llvm_one_aggregate_result_is_returned_as_c_returns_a_struct) {
    // The hidden pointer of a procedure whose one result is an aggregate is
    // sret, on its definition and at every call, so LLVM returns it in rax
    // too, as C returns a struct in memory. With several results, none is.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, AGGREGATE_CALLS);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "define internal void @make(ptr sret([16 x i8]) %0, i64 %1)"));
        CHECK(contains(text, "define internal void @row(ptr sret([16 x i8]) %0, i64 %1)"));
        CHECK(contains(text, "call void @make(ptr sret([16 x i8]) %"));
        CHECK(contains(text, "call void @row(ptr sret([16 x i8]) %"));
        CHECK(contains(text, "define internal i64 @both(ptr %0, i64 %1)"));
        CHECK(contains(text, "define internal void @pairs(ptr %0, ptr %1, ptr %2)"));
        CHECK(contains(text, "call i64 @both(ptr %"));
        CHECK(count_of(text, "sret(") == 4);
    }
    arena_free(&a);
}

TEST(llvm_result_pointers_are_parameters) {
    // Each hidden result pointer is an artificial parameter variable,
    // result$ (result$1, result$2, ... for several), a pointer to the
    // result's type, declared on a slot the pointer is stored into. The
    // visible parameters come after them, and count them.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, AGGREGATE_CALLS);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String pair = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Pair\""));
        String array = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_array_type, baseType: "));
        struct Expected {
            const char *procedure;
            const char *name;  // the variable's
            int arg;
            String points_at;
        };
        Expected expected[] = {
            {"make", "result$", 1, pair},   {"row", "result$", 1, array},   {"both", "result$", 1, pair},
            {"pairs", "result$1", 1, pair}, {"pairs", "result$2", 2, pair},
        };
        for (Expected &e : expected) {
            String procedure = defined_node(
                line_with(text, string_cstr(&a, string_printf(&a, "!DISubprogram(name: \"%s\"", e.procedure))));
            String variable = line_with(
                text, string_cstr(&a, string_printf(&a, "!DILocalVariable(name: \"%s\", arg: %d, scope: " STR_FMT ",",
                                                    e.name, e.arg, STR_ARG(procedure))));
            if (variable.count == 0) printf("    no %s in %s\n", e.name, e.procedure);
            CHECK(variable.count > 0);
            CHECK(contains(variable, "flags: DIFlagArtificial)"));
            CHECK(string_eq(attached(&a, variable, "type:"), pointer_node(&a, text, e.points_at)));
            String declare = string_printf(&a, ", " STR_FMT ", !DIExpression(), !", STR_ARG(defined_node(variable)));
            CHECK(contains(line_with(text, string_cstr(&a, declare)), "#dbg_declare(ptr %"));
        }
        String make = defined_node(line_with(text, "!DISubprogram(name: \"make\""));
        String pairs = defined_node(line_with(text, "!DISubprogram(name: \"pairs\""));
        String n_of_make =
            string_printf(&a, "!DILocalVariable(name: \"n\", arg: 2, scope: " STR_FMT ",", STR_ARG(make));
        String n_of_pairs =
            string_printf(&a, "!DILocalVariable(name: \"n\", arg: 3, scope: " STR_FMT ",", STR_ARG(pairs));
        CHECK(contains(text, string_cstr(&a, n_of_make)));
        CHECK(contains(text, string_cstr(&a, n_of_pairs)));
    }
    arena_free(&a);
}

TEST(llvm_globals_have_debug_info) {
    // A Rig global is a DIGlobalVariable of its type, local to the unit,
    // attached to the LLVM global (!dbg); a string literal's bytes are none.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                "counter := 3;\n"
                                "main :: () {\n"
                                "    puts(\"hi\");\n"
                                "    counter += 1;\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String variable = line_with(text, "!DIGlobalVariable(name: \"counter\"");
        CHECK(contains(variable, "line: 2, "));
        CHECK(contains(variable, "isLocal: true, isDefinition: true"));
        CHECK(contains(line_with(text, "@counter = internal global i64 3"), "!dbg !"));
        String literal = line_with(text, "@str.0 = ");
        CHECK(literal.count > 0);
        CHECK(!contains(literal, "!dbg"));
        CHECK(count_of(text, "!DIGlobalVariable(") == 1);
    }
    arena_free(&a);
}

TEST(llvm_read_only_global) {
    // A data constant's bytes are an LLVM constant, laid out as a variable
    // of bytes is (a packed struct around each address, or an array of
    // bytes), private and aligned as the IR says. Unlike a string literal's
    // bytes it isn't unnamed_addr, and, made here with no constant naming
    // it (Ir_Module.constants), it has no debug information, though the
    // module has some.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, "main :: () { }\n");
    s32 text = add_string(m, "str.0", "abc");
    s32 constant = add_read_only_global(m, "const.0", 24, 8);
    u8 *bytes = (u8 *)arena_alloc(&a, 24, 1);  // zeroed
    bytes[0] = 3;
    m->globals[constant].bytes = {bytes, 24};
    array_add(&m->globals[constant].relocs, {8, text, false});
    add_read_only_global(m, "zeros", 16, 8);
    for (int t = 0; t < 2; t++) {
        String llvm = llvm_text(&a, m, (Target)t);
        CHECK(contains(llvm, "!DICompileUnit("));
        String table = line_with(llvm, "@const.0 = ");
        CHECK(contains(table, "@const.0 = private constant <{ [8 x i8], ptr, [8 x i8] }> "
                              "<{ [8 x i8] c\"\\03\\00\\00\\00\\00\\00\\00\\00\", ptr @str.0, "
                              "[8 x i8] zeroinitializer }>, align 8"));
        String zeros = line_with(llvm, "@zeros = ");
        CHECK(contains(zeros, "@zeros = private constant [16 x i8] zeroinitializer, align 8"));
        CHECK(!contains(table, "unnamed_addr") && !contains(table, "!dbg"));
        CHECK(!contains(zeros, "unnamed_addr") && !contains(zeros, "!dbg"));
    }
    arena_free(&a);
}

TEST(llvm_data_constant_is_private_constant) {
    // Issue #113: a data constant lowered from source is the same LLVM
    // constant, its string literal's and procedure's addresses inside it.
    // Issue #172: a debugger shows it, by a global variable named as the
    // constant and attached to it.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                "add :: (a: int, b: int) -> int { return a + b; }\n"
                                "Op :: struct { symbol: *u8; run: (int, int) -> int; }\n"
                                "OPS :: Op.[.{\"+\", add}];\n"
                                "main :: () { i := 0; puts(OPS[i].symbol); }\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String table = line_with(text, "@const.0 = ");
        CHECK(contains(table, "private constant"));
        CHECK(contains(table, "ptr @str."));
        CHECK(contains(table, "ptr @add"));
        CHECK(contains(table, "align 8"));
        CHECK(!contains(table, "unnamed_addr") && contains(table, "!dbg"));
        CHECK_EQ(count_of(text, "!DIGlobalVariable(name: \"OPS\""), 1);
    }
    arena_free(&a);
}

// Issue #172: named constants. The DIGlobalVariableExpression whose
// variable is the DIGlobalVariable named `name`: its node ("!12"); {} when
// there's none.
static String global_variable_expression(Arena *a, String text, const char *name) {
    const char *global = string_cstr(a, string_printf(a, "!DIGlobalVariable(name: \"%s\"", name));
    String variable = defined_node(line_with(text, global));
    if (variable.count == 0) return {};
    const char *needle =
        string_cstr(a, string_printf(a, "!DIGlobalVariableExpression(var: " STR_FMT ", ", STR_ARG(variable)));
    return defined_node(line_with(text, needle));
}

// The expression of the DIGlobalVariableExpression whose variable is the
// DIGlobalVariable named `name`: "!DIExpression(...)"; {} when none.
static String constant_expression(Arena *a, String text, const char *name) {
    String node = global_variable_expression(a, text, name);
    if (node.count == 0) return {};
    String line = node_line(a, text, node);
    s64 at = index_of(line, "expr: ");
    if (at < 0) return {};
    // The expression runs to the line's last ')', which closes the global
    // variable expression.
    at += (s64)strlen("expr: ");
    return string_slice(line, at, line.count - 1 - at);
}

// The line of the LLVM global that metadata node `node` (a global variable
// expression) is attached to; {} when none is. A global may have several.
static String global_attached_to(Arena *a, String text, String node) {
    String line = line_with(text, string_cstr(a, string_printf(a, "!dbg " STR_FMT "\n", STR_ARG(node))));
    if (line.count > 0) return line;
    return line_with(text, string_cstr(a, string_printf(a, "!dbg " STR_FMT ",", STR_ARG(node))));
}

// Whether the tuple metadata node `tuple` ("!{!3, !7}", defined on its own
// line) lists node `node`.
static bool tuple_lists(Arena *a, String text, String tuple, String node) {
    String line = node_line(a, text, tuple);
    return contains(line, string_cstr(a, string_printf(a, STR_FMT ",", STR_ARG(node)))) ||
           contains(line, string_cstr(a, string_printf(a, STR_FMT "}", STR_ARG(node))));
}

TEST(llvm_constants_have_debug_info) {
    // Issue #172. A top-level number, bool or null is a global variable
    // expression of its value, attached to nothing (CodeView's S_CONSTANT,
    // DWARF's DW_AT_const_value); data is one attached to its const.N. A
    // body's number is a local variable whose value a #dbg_value gives, as
    // an i64 (a float's bits), in its block; a body's data a global
    // variable scoped to its block, in its subprogram's retained nodes. A
    // procedure-typed null is a pointer to its subroutine type, as a
    // procedure-typed variable is.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Point :: struct { x: int; y: int; }\n"     // 1
                                "LIMIT :: 12;\n"                             // 2
                                "RATIO :: 0.1;\n"
                                "HALF :: cast(float32) 1.5;\n"
                                "SMALL :: cast(s8) -5;\n"
                                "READY :: true;\n"
                                "DONE :: false;\n"
                                "NOTHING :: null;\n"
                                "NO_PROC :: cast((int) -> int) null;\n"
                                "NAME :: \"rig\";\n"                         // 10
                                "add :: (a: int, b: int) -> int { return a + b; }\n"
                                "OP :: add;\n"
                                "main :: () {\n"                             // 13
                                "    STEP :: 7;\n"
                                "    SCALE :: cast(float32) 0.25;\n"
                                "    RATE :: 0.25;\n"
                                "    NO_CALL :: cast((int) -> int) null;\n"
                                "    { CORNER :: Point.{7, 8}; INNER :: 5; i := 1; }\n"  // 18
                                "    NEG :: cast(s8) -5;\n"
                                "    NF :: cast(float32) -1.5;\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(line_with(text, "!DIGlobalVariable(name: \"LIMIT\""), "line: 2, "));
        CHECK(contains(line_with(text, "!DIGlobalVariable(name: \"LIMIT\""), "isLocal: true, isDefinition: true"));
        CHECK(string_eq(constant_expression(&a, text, "LIMIT"),
                        str("!DIExpression(DW_OP_constu, 12, DW_OP_stack_value)")));
        CHECK(contains(constant_expression(&a, text, "RATIO"), "DW_OP_constu, 4591870180066957722, "));
        CHECK(contains(constant_expression(&a, text, "HALF"), "DW_OP_constu, 1069547520, "));
        CHECK(contains(constant_expression(&a, text, "SMALL"), "DW_OP_constu, 18446744073709551611, "));
        CHECK(contains(constant_expression(&a, text, "READY"), "DW_OP_constu, 1, "));
        CHECK(string_eq(constant_expression(&a, text, "DONE"),
                        str("!DIExpression(DW_OP_constu, 0, DW_OP_stack_value)")));
        CHECK(contains(constant_expression(&a, text, "NOTHING"), "DW_OP_constu, 0, "));
        CHECK(string_eq(constant_expression(&a, text, "NO_PROC"),
                        str("!DIExpression(DW_OP_constu, 0, DW_OP_stack_value)")));
        String no_proc = node_line(&a, text, attached(&a, line_with(text, "!DIGlobalVariable(name: \"NO_PROC\""), "type:"));
        CHECK(contains(no_proc, "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: "));
        CHECK(contains(node_line(&a, text, attached(&a, no_proc, "baseType:")), "!DISubroutineType("));
        CHECK(string_eq(constant_expression(&a, text, "NAME"), str("!DIExpression()")));
        CHECK(!contains(text, "!DIGlobalVariable(name: \"OP\""));
        // NAME's is attached to its const.N; LIMIT's to nothing, the unit
        // lists it.
        String name = global_variable_expression(&a, text, "NAME");
        CHECK(name.count > 0 && starts_with(global_attached_to(&a, text, name), "@const."));
        String limit = global_variable_expression(&a, text, "LIMIT");
        CHECK(limit.count > 0 && global_attached_to(&a, text, limit).count == 0);
        String unit_globals = attached(&a, line_with(text, "!DICompileUnit("), "globals:");
        CHECK(unit_globals.count > 0 && tuple_lists(&a, text, unit_globals, limit));
        // STEP, SCALE and RATE: local variables of main, their values by
        // #dbg_value, SCALE's as a float32's bits in an i64 and RATE's, an
        // untyped float, as a float64's (LLVM's CodeView writer drops a
        // double).
        String main_line = line_with(text, "!DISubprogram(name: \"main\"");
        String main = defined_node(main_line);
        String step_line = line_with(text, "!DILocalVariable(name: \"STEP\"");
        CHECK(contains(step_line, string_cstr(&a, string_printf(&a, "scope: " STR_FMT ",", STR_ARG(main)))));
        CHECK(contains(step_line, "line: 14, "));
        String step = defined_node(step_line);
        CHECK(step.count > 0 &&
              contains(text, string_cstr(&a, string_printf(&a, "#dbg_value(i64 7, " STR_FMT ", !DIExpression(), ",
                                                           STR_ARG(step)))));
        String scale = defined_node(line_with(text, "!DILocalVariable(name: \"SCALE\""));
        CHECK(scale.count > 0 &&
              contains(text, string_cstr(&a, string_printf(&a, "#dbg_value(i64 1048576000, " STR_FMT ", ",
                                                           STR_ARG(scale)))));
        String rate_line = line_with(text, "!DILocalVariable(name: \"RATE\"");
        CHECK(contains(node_line(&a, text, attached(&a, rate_line, "type:")), "name: \"float64\""));
        String rate = defined_node(rate_line);
        CHECK(rate.count > 0 &&
              contains(text, string_cstr(&a, string_printf(&a, "#dbg_value(i64 4598175219545276416, " STR_FMT ", ",
                                                           STR_ARG(rate)))));
        // NEG: an s8's bits extended by its sign, which LLVM prints as a
        // signed i64; NF: a negative float32's 32 bits (0xBFC00000),
        // zero-extended.
        String neg = defined_node(line_with(text, "!DILocalVariable(name: \"NEG\""));
        CHECK(neg.count > 0 &&
              contains(text, string_cstr(&a, string_printf(&a, "#dbg_value(i64 -5, " STR_FMT ", ", STR_ARG(neg)))));
        String nf = defined_node(line_with(text, "!DILocalVariable(name: \"NF\""));
        CHECK(nf.count > 0 &&
              contains(text, string_cstr(&a, string_printf(&a, "#dbg_value(i64 3217031168, " STR_FMT ", ",
                                                           STR_ARG(nf)))));
        CHECK(!contains(text, "!DIGlobalVariable(name: \"STEP\""));
        // NO_CALL: a pointer to its subroutine type, its value null.
        String no_call_line = line_with(text, "!DILocalVariable(name: \"NO_CALL\"");
        CHECK(contains(node_line(&a, text, attached(&a, no_call_line, "type:")),
                       "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: "));
        String no_call = defined_node(no_call_line);
        CHECK(no_call.count > 0 &&
              contains(text, string_cstr(&a, string_printf(&a, "#dbg_value(i64 0, " STR_FMT ", ", STR_ARG(no_call)))));
        // CORNER: a global variable whose scope is the block's lexical
        // block, inside main, attached to its const.N, in main's retained
        // nodes (not the unit's globals, which LLVM's verifier refuses).
        String corner_line = line_with(text, "!DIGlobalVariable(name: \"CORNER\"");
        String block = attached(&a, corner_line, "scope:");
        CHECK(contains(node_line(&a, text, block),
                       string_cstr(&a, string_printf(&a, "!DILexicalBlock(scope: " STR_FMT ",", STR_ARG(main)))));
        CHECK(contains(corner_line, "line: 18, "));
        // INNER, a number beside it: a local variable of that block, not of
        // main.
        String inner_line = line_with(text, "!DILocalVariable(name: \"INNER\"");
        CHECK(block.count > 0 &&
              contains(inner_line, string_cstr(&a, string_printf(&a, "scope: " STR_FMT ",", STR_ARG(block)))));
        String inner = defined_node(inner_line);
        CHECK(inner.count > 0 &&
              contains(text, string_cstr(&a, string_printf(&a, "#dbg_value(i64 5, " STR_FMT ", ", STR_ARG(inner)))));
        String corner = global_variable_expression(&a, text, "CORNER");
        CHECK(corner.count > 0 && starts_with(global_attached_to(&a, text, corner), "@const."));
        CHECK(tuple_lists(&a, text, attached(&a, main_line, "retainedNodes:"), corner));
        CHECK(!tuple_lists(&a, text, unit_globals, corner));
    }
    arena_free(&a);
}

TEST(llvm_constants_in_deferred_code) {
    // Review Focus 1: a data constant in deferred code copied twice is one
    // const.N with two !dbg, one per copy's variable, and the module
    // verifies (llvm_text checks it); each copy's number is a variable of
    // its own.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "seven :: () -> int { K :: 7; return K; }\n"
                                "C :: #run seven();\n"
                                "main :: () {\n"
                                "    i := 0;\n"
                                "    defer { K :: 3; D :: int.[1, 2]; i = K + D[i]; }\n"
                                "    if i == 0 { return; }\n"
                                "    #if OS == .WINDOWS { W :: 1; } else { W :: 1; }\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK_EQ(count_of(line_with(text, "@const.0 = "), "!dbg"), 2);
        CHECK_EQ(count_of(text, "!DIGlobalVariable(name: \"D\""), 2);
        // Each copy's D in that copy's block.
        const char *d = "!DIGlobalVariable(name: \"D\"";
        s64 after = index_of(text, d) + 1;
        String first = attached(&a, line_with(text, d), "scope:");
        String second = attached(&a, line_with(string_slice(text, after, text.count - after), d), "scope:");
        CHECK(first.count > 0 && second.count > 0 && !string_eq(first, second));
        CHECK(contains(node_line(&a, text, first), "!DILexicalBlock("));
        CHECK(contains(node_line(&a, text, second), "!DILexicalBlock("));
        // seven's K, and main's two copies.
        CHECK_EQ(count_of(text, "!DILocalVariable(name: \"K\""), 3);
        CHECK(contains(text, "#dbg_value(i64 3, "));
        CHECK(contains(constant_expression(&a, text, "C"), "DW_OP_constu, 7, "));
    }
    arena_free(&a);
}

static String object_from(Arena *a, Ir_Module *m, Target t, bool optimize, bool lowest);

TEST(llvm_unread_constant_data_survives_optimisation) {
    // Review Focus 5: data constants no code reads, optimised: LLVM's O2
    // may drop their const.N, and inline and drop pick, whose constants
    // are in its subprogram's retained nodes. The optimised module still
    // verifies (run_passes doesn't check it), and the CodeView or DWARF
    // writer still writes an object of it.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Point :: struct { x: int; y: int; }\n"
                                "ORIGIN :: int.[3, 4];\n"
                                "pick :: (i: int) -> int { T :: int.[1, 2, 3]; U :: Point.{5, 6}; return T[i]; }\n"
                                "main :: () { { CORNER :: int.[7, 8]; i := 1; } n := pick(1); }\n");
    for (int t = 0; t < 2; t++) {
        String plain = {}, error = {};
        CHECK(llvm_ir_text(&a, m, (Target)t, &plain, &error, false));
        CHECK(contains(line_with(plain, "@const.0 = "), "!dbg"));
        const char *names[] = {"ORIGIN", "T", "U", "CORNER"};
        for (const char *name : names) {
            CHECK(contains(plain, string_cstr(&a, string_printf(&a, "!DIGlobalVariable(name: \"%s\"", name))));
        }
        Backend b = {};
        bool ok = backend_prepare(&b, m, (Target)t, true, &a, &error);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
        char *message = nullptr;
        if (ok && LLVMVerifyModule(b.module, LLVMReturnStatusAction, &message)) {
            fprintf(stderr, "  %s\n", message);
            CHECK(false);
        }
        LLVMDisposeMessage(message);
        backend_free(&b);
        CHECK(object_from(&a, m, (Target)t, true, false).count > 0);
    }
    arena_free(&a);
}

// Issue #112: a program of several files has a DIFile for each, with its
// own checksum, in the directory rig ran in, and each node that names a
// file names the one its declaration is in: a procedure's subprogram, its
// lexical blocks and its variables (result$ too) the procedure's; a struct
// type, its members and its stand-in below DEBUG_TYPE_LEVELS the struct's;
// a global and a constant (issue #172) its own. A subprogram and a struct
// type name it twice, as their file and as their scope. The compile unit
// names the main file, and so do a view type and its members, which no
// line declares, though only the loaded file uses it. Each file has a
// struct, a global, a constant and a procedure with a block, so a node
// that named the wrong one, either way, would show.
TEST(debug_info_names_each_procedures_file) {
    Arena a = arena_make();
    String dir = load_test_directory(&a, "debug_info_names_each_procedures_file");
    const char *names[] = {"main.rig", "helper.rig"};
    const char *texts[] = {
        string_cstr(&a, string_printf(&a,
                                      "#load \"helper.rig\";\n"
                                      "Point :: struct { x: int; }\n"
                                      "count := 0;\n"
                                      "LIMIT :: 9;\n"
                                      "main :: () {\n"
                                      "    p := Pair.{1, 2};\n"
                                      "    q := Point.{3};\n"
                                      "    if p.a > 0 {\n"
                                      "        count = q.x;\n"
                                      "    }\n"
                                      "    total = helper(p.a);\n"
                                      "    deep : D%d;\n"
                                      "}\n",
                                      MAX_DEPTH)),
        // D0 to D<MAX_DEPTH>, each holding the one before: main's deep, a
        // D<MAX_DEPTH>, is a level deeper than a debug type may be
        // (DEBUG_TYPE_LEVELS), so D1 is a stand-in, a declaration.
        joined(&a,
               "Pair :: struct { a: int; b: int; }\n"
               "total := 0;\n"
               "STRIDE :: 4;\n"
               "STEPS :: int.[1, 2];\n"
               "helper :: (n: int) -> int {\n"
               "    doubled := n * 2;\n"
               "    pairs : []Pair;\n"
               "    if doubled > 40 {\n"
               "        bigger := doubled + 1;\n"
               "    }\n"
               "    return doubled;\n"
               "}\n"
               "pair_of :: (n: int) -> Pair {\n"
               "    return Pair.{n, n};\n"
               "}\n"
               "D0 :: struct { d: int; }\n",
               chain_of(&a, "D%lld :: struct { inner: D%lld; }\n", 1, MAX_DEPTH, -1)),
    };
    for (int i = 0; i < 2; i++) write_source(&a, dir, names[i], texts[i]);
    Ast_File *tree = nullptr;
    // Without Preload, which every program imports as file 1, so the
    // program is these two files, with a node each.
    Ir_Module *m = lower_program(&a, str(in_directory(&a, dir, "main.rig")), &tree, false);
    CHECK(m != nullptr);
    if (!m) {
        arena_free(&a);
        return;
    }
    CHECK_EQ(m->sources.count, 2);
    if (m->sources.count != 2) {
        arena_free(&a);
        return;
    }
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK_EQ(count_of(text, "!DIFile("), 2);
        // Each file's node, found by its name and its directory (the
        // absolute paths are drive paths or Linux ones, which
        // llvm_debug_file_names keeps), with the MD5 of that file's text.
        String files[2];
        for (int i = 0; i < 2; i++) {
            String name = string_printf(&a, "!DIFile(filename: \"" STR_FMT "\", directory: \"" STR_FMT "\"",
                                        STR_ARG(llvm_escaped(&a, m->sources[i].path)),
                                        STR_ARG(llvm_escaped(&a, m->directory)));
            String line = line_with(text, string_cstr(&a, name));
            files[i] = defined_node(line);
            CHECK(files[i].count > 0);
            String checksum = string_printf(&a, "checksumkind: CSK_MD5, checksum: \"" STR_FMT "\"",
                                            STR_ARG(md5_hex(&a, texts[i])));
            CHECK(contains(line, string_cstr(&a, checksum)));
        }
        CHECK(!string_eq(files[0], files[1]));
        // A procedure's block, found by its scope: the procedure's
        // subprogram.
        const char *blocks[2];
        const char *procs[] = {"main", "helper"};
        for (int i = 0; i < 2; i++) {
            String subprogram = defined_node(
                line_with(text, string_cstr(&a, string_printf(&a, "!DISubprogram(name: \"%s\"", procs[i]))));
            CHECK(subprogram.count > 0);
            blocks[i] = string_cstr(&a, string_printf(&a, "!DILexicalBlock(scope: " STR_FMT ",", STR_ARG(subprogram)));
        }
        // D1 is the stand-in, a declaration.
        CHECK(contains(struct_line(&a, text, "D1"), ", flags: DIFlagFwdDecl)"));
        struct Named {
            const char *node;  // the start of the node's definition
            int file;          // the source it must name
            bool file_scope;   // whether its scope is that file too
        };
        Named nodes[] = {
            {"!DICompileUnit(", 0, false},
            {"!DISubprogram(name: \"main\"", 0, true},
            {"!DILocalVariable(name: \"p\"", 0, false},
            {"!DILocalVariable(name: \"q\"", 0, false},
            {blocks[0], 0, false},
            {"!DICompositeType(tag: DW_TAG_structure_type, name: \"Point\"", 0, true},
            {"!DIDerivedType(tag: DW_TAG_member, name: \"x\"", 0, false},
            {"!DICompositeType(tag: DW_TAG_structure_type, name: \"[]Pair\"", 0, true},
            {"!DIDerivedType(tag: DW_TAG_member, name: \"count\"", 0, false},
            {"!DIGlobalVariable(name: \"count\"", 0, false},
            {"!DIGlobalVariable(name: \"LIMIT\"", 0, false},
            {"!DISubprogram(name: \"helper\"", 1, true},
            {"!DILocalVariable(name: \"n\"", 1, false},
            {"!DILocalVariable(name: \"doubled\"", 1, false},
            {"!DILocalVariable(name: \"bigger\"", 1, false},
            {blocks[1], 1, false},
            {"!DILocalVariable(name: \"result$\"", 1, false},
            {"!DICompositeType(tag: DW_TAG_structure_type, name: \"Pair\"", 1, true},
            {"!DIDerivedType(tag: DW_TAG_member, name: \"a\"", 1, false},
            {"!DIGlobalVariable(name: \"total\"", 1, false},
            {"!DIGlobalVariable(name: \"STRIDE\"", 1, false},
            {"!DIGlobalVariable(name: \"STEPS\"", 1, false},
            {"!DICompositeType(tag: DW_TAG_structure_type, name: \"D1\"", 1, true},
        };
        const char *labels[] = {"file:", "scope:"};
        for (Named &n : nodes) {
            for (int k = 0; k < (n.file_scope ? 2 : 1); k++) {
                String file = attached(&a, line_with(text, n.node), labels[k]);
                if (!string_eq(file, files[n.file])) {
                    fprintf(stderr, "  %s: %s has %s " STR_FMT ", not " STR_FMT "\n", target_triple((Target)t),
                            n.node, labels[k], STR_ARG(file), STR_ARG(files[n.file]));
                }
                CHECK(string_eq(file, files[n.file]));
            }
        }
    }
    // On Windows, each file on a network share is named as
    // llvm_debug_file_names makes it, with a '/' first: the loaded file as
    // well as the main one.
    m->directory = str("\\\\server\\share\\dir");
    for (int i = 0; i < 2; i++) m->sources[i].path = string_printf(&a, "\\\\server\\share\\dir\\%s", names[i]);
    String text = llvm_text(&a, m, TARGET_WINDOWS_X64);
    for (int i = 0; i < 2; i++) {
        String name = string_printf(&a, "/\\server\\share\\dir\\%s", names[i]);
        String file = string_printf(&a, "!DIFile(filename: \"" STR_FMT "\", directory: \"" STR_FMT "\"",
                                    STR_ARG(llvm_escaped(&a, name)), STR_ARG(llvm_escaped(&a, m->directory)));
        CHECK(contains(text, string_cstr(&a, file)));
    }
    arena_free(&a);
}

// Issue #82: a variadic foreign procedure is a variadic LLVM function,
// declared with its fixed parameters; a call gives the extra arguments
// after them, already promoted as C promotes them. LLVM then follows each
// target's variadic convention: on x86-64 System V, al holds the number of
// vector registers the call uses; on Windows x64, a double is passed in its
// integer register too.
static const char *PRINTF_CALLS =
    "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
    "main :: () {\n"
    "    b : u8 = 7;\n"
    "    s : s16 = -2;\n"
    "    t := true;\n"
    "    f : float32 = 1.5;\n"
    "    printf(\"%d %d %d %.2f %lld %.2f\", b, s, t, f, 5, 2.5);\n"
    "}\n";

TEST(llvm_declares_and_calls_a_variadic_function) {
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, PRINTF_CALLS);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "declare i32 @printf(ptr, ...)"));
        // Each to the width C's '...' receives: an int, a double.
        CHECK(contains(line_with(text, "zext i8 %"), " to i32"));
        CHECK(contains(line_with(text, "sext i16 %"), " to i32"));
        CHECK(contains(line_with(text, "zext i1 %"), " to i32"));
        CHECK(contains(line_with(text, "fpext float %"), " to double"));
        CHECK(contains(text, "call i32 (ptr, ...) @printf(ptr @str.0, i32 %"));
        CHECK(contains(text, ", double %"));
        CHECK(contains(text, ", i64 5, double 2.500000e+00)"));
    }
    // With debug information too: a foreign procedure has none of its own,
    // variadic or not.
    m = lower_file(&a, "printf.rig", PRINTF_CALLS, true);
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "declare i32 @printf(ptr, ...)"));
    }
    arena_free(&a);
}

TEST(llvm_calls_through_values_as_through_names) {
    // A call through a procedure value is LLVM's call of a ptr, with the
    // function type and the attributes a call of the procedure would have:
    // the extensions (on Linux, where the caller extends; on Windows the
    // callee does, and a bool is zeroext on both, as clang has them), sret
    // and the several results' struct.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Pair :: struct { a: int; b: int; }\n"
                                "make :: (n: int) -> Pair { return Pair.{n, n}; }\n"
                                "small :: (n: s8) -> u8 { return cast(u8) n; }\n"
                                "two :: (n: int) -> (int, int) { return n, n; }\n"
                                "flip :: (b: bool) -> bool { return !b; }\n"
                                "main :: () {\n"
                                "    m := make;\n"
                                "    p := m(1);\n"
                                "    s := small;\n"
                                "    x := s(2);\n"
                                "    t := two;\n"
                                "    q, r := t(3);\n"
                                "    f := flip;\n"
                                "    y := f(true);\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "call void %"));
        CHECK(contains(text, "(ptr sret([16 x i8]) %"));
        CHECK(contains(text, "call { i64, i64 } %"));
        CHECK(count_of(text, "sret(") == 2);  // make's definition and the call through m
        CHECK(contains(text, "call zeroext i1 %"));
        CHECK(contains(text, "(i1 zeroext true)"));
        if ((Target)t == TARGET_LINUX_X64) {
            CHECK(contains(text, "call zeroext i8 %"));
            CHECK(contains(text, "(i8 signext 2)"));
        } else {
            CHECK(contains(text, "call i8 %"));
            CHECK(contains(text, "(i8 2)"));
            CHECK(!contains(text, "signext") && count_of(text, "zeroext i8") == 0);
        }
    }
    arena_free(&a);
}

TEST(llvm_c_calls_procedures_given_and_held) {
    // A #c_call procedure given to a foreign one is its address, a ptr, as
    // a C function's is, and the foreign procedure takes a ptr for it; a
    // foreign procedure held in a value is called through it as C's.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "qsort :: (base: *void, n: u64, size: u64, f: (*void, *void) -> s32 #c_call) "
                                "#foreign libc;\n"
                                "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                "compare :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                                "main :: () {\n"
                                "    qsort(null, 0, 8, compare);\n"
                                "    say := puts;\n"
                                "    say(\"hi\");\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "declare void @qsort(ptr, i64, i64, ptr)"));
        CHECK(contains(text, "call void @qsort(ptr null, i64 0, i64 8, ptr @compare)"));
        CHECK(contains(text, " i32 @compare(ptr %0, ptr %1)"));
        CHECK(contains(text, "declare i32 @puts(ptr"));
        CHECK(contains(line_with(text, "call i32 %"), "(ptr @str.0)"));
    }
    arena_free(&a);
}

// The subroutine type a procedure-typed variable named `name` points at,
// and the list of its types: "!{!9, !9, !9}" from the variable's
// "type: !12", whose pointer's "baseType: !11", whose "types: !10".
static String pointed_signature(Arena *a, String text, const char *name) {
    String variable = line_with(text, string_cstr(a, string_printf(a, "!DILocalVariable(name: \"%s\"", name)));
    String pointer = node_line(a, text, attached(a, variable, "type:"));
    if (!contains(pointer, "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: ")) return {};
    String signature = node_line(a, text, attached(a, pointer, "baseType:"));
    if (!contains(signature, "!DISubroutineType(")) return {};
    String types = node_line(a, text, attached(a, signature, "types:"));
    s64 at = index_of(types, "!{");
    return at < 0 ? String{} : string_slice(types, at, types.count - at);
}

TEST(llvm_procedure_values_are_function_pointers) {
    // A procedure-typed variable, parameter or field is a pointer to the
    // subroutine type of its signature, as a C function pointer is, so a
    // debugger shows the procedure it holds and can call it. The signature
    // is the one a procedure of the type has (spec 10.4, 10.7): aggregates
    // as pointers, a hidden pointer per aggregate result first, the one
    // scalar result or none. One type is one node, wherever it's used, and
    // #c_call changes nothing a debugger sees.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Pair :: struct { a: int; b: int; }\n"
                                "Ops :: struct { op: (int, int) -> int; }\n"
                                "add :: (a: int, b: int) -> int { return a + b; }\n"
                                "make :: (p: Pair, n: int) -> Pair { return p; }\n"
                                "two :: (n: int) -> (int, int) { return n, n; }\n"
                                "none :: () { }\n"
                                "compare :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                                "apply :: (g: (int, int) -> int) -> int { return g(1, 2); }\n"
                                "main :: () {\n"
                                "    f := add;\n"
                                "    m := make;\n"
                                "    t := two;\n"
                                "    z := none;\n"
                                "    c := compare;\n"
                                "    o := Ops.{add};\n"
                                "    apply(f);\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String integer = defined_node(line_with(text, "!DIBasicType(name: \"int\""));
        String s32 = defined_node(line_with(text, "!DIBasicType(name: \"s32\""));
        String pair = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Pair\""));
        String to_pair = pointer_node(&a, text, pair);
        String to_void = defined_node(line_with(text, "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null"));
        CHECK(integer.count > 0 && s32.count > 0 && to_pair.count > 0 && to_void.count > 0);
        struct Expected {
            const char *name;
            String types;
        };
        Expected expected[] = {
            {"f", string_printf(&a, "!{" STR_FMT ", " STR_FMT ", " STR_FMT "}", STR_ARG(integer), STR_ARG(integer),
                                STR_ARG(integer))},
            {"g", string_printf(&a, "!{" STR_FMT ", " STR_FMT ", " STR_FMT "}", STR_ARG(integer), STR_ARG(integer),
                                STR_ARG(integer))},
            {"m", string_printf(&a, "!{null, " STR_FMT ", " STR_FMT ", " STR_FMT "}", STR_ARG(to_pair),
                                STR_ARG(to_pair), STR_ARG(integer))},
            {"t", string_printf(&a, "!{null, " STR_FMT "}", STR_ARG(integer))},
            {"z", string_printf(&a, "!{null}")},
            {"c", string_printf(&a, "!{" STR_FMT ", " STR_FMT ", " STR_FMT "}", STR_ARG(s32), STR_ARG(to_void),
                                STR_ARG(to_void))},
        };
        for (Expected &e : expected) {
            String got = pointed_signature(&a, text, e.name);
            if (!string_eq(got, e.types)) {
                printf("    %s: " STR_FMT ", not " STR_FMT "\n", e.name, STR_ARG(got), STR_ARG(e.types));
            }
            CHECK(string_eq(got, e.types));
        }
        // f, apply's g and Ops' op have one type, one pointer node.
        String f = attached(&a, line_with(text, "!DILocalVariable(name: \"f\""), "type:");
        CHECK(string_eq(attached(&a, line_with(text, "!DILocalVariable(name: \"g\""), "type:"), f));
        CHECK(string_eq(attached(&a, line_with(text, "!DIDerivedType(tag: DW_TAG_member, name: \"op\""), "baseType:"),
                        f));
    }
    arena_free(&a);
}

// Issue #116: optimize (spec 10.2).

TEST(llvm_optimises_with_the_o2_pipeline) {
    // Unoptimised, each local and each parameter lives in a stack slot, an
    // alloca. With optimize, LLVM's default<O2> pipeline runs: SROA puts
    // scalars in registers, so no alloca is left (the program has scalars
    // only: an aggregate passed by address could keep its slot). The debug
    // information stays, and says the code is optimised, as clang's -O2
    // -g does.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "twice :: (n: int) -> int {\n"
                                "    return n * 2;\n"
                                "}\n"
                                "main :: () {\n"
                                "    x := twice(3);\n"
                                "    x += 1;\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String plain = {}, optimised = {}, error = {};
        CHECK(llvm_ir_text(&a, m, (Target)t, &plain, &error, false));
        CHECK(contains(plain, "alloca"));
        CHECK(contains(plain, "DICompileUnit"));
        CHECK(contains(plain, "isOptimized: false"));
        CHECK(!contains(plain, "DISPFlagOptimized"));
        bool ok = llvm_ir_text(&a, m, (Target)t, &optimised, &error, true);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
        CHECK(!contains(optimised, "alloca"));
        CHECK(contains(optimised, "DICompileUnit"));
        CHECK(contains(optimised, "isOptimized: true"));
        CHECK(contains(optimised, "DISPFlagOptimized"));
    }
    arena_free(&a);
}

// The object file for m on target t, emitted into memory from the module
// backend_prepare leaves for `optimize`: by the target machine
// backend_build made, or, with `lowest`, by one made here at LLVM's lowest
// codegen level instead, as backend_build makes it unoptimised. Empty, and
// a failed check, when it can't be made.
static String object_from(Arena *a, Ir_Module *m, Target t, bool optimize, bool lowest) {
    Backend b = {};
    String error = {}, object = {};
    bool ok = backend_prepare(&b, m, t, optimize, a, &error);
    CHECK(ok);
    if (ok && lowest) {
        LLVMTargetRef target = LLVMGetTargetMachineTarget(b.machine);
        LLVMRelocMode reloc = t == TARGET_LINUX_X64 ? LLVMRelocPIC : LLVMRelocDefault;
        LLVMDisposeTargetMachine(b.machine);
        b.machine = LLVMCreateTargetMachine(target, target_triple(t), "x86-64", "", LLVMCodeGenLevelNone, reloc,
                                            LLVMCodeModelDefault);
    }
    LLVMMemoryBufferRef buffer = nullptr;
    char *message = nullptr;
    if (ok && LLVMTargetMachineEmitToMemoryBuffer(b.machine, b.module, LLVMObjectFile, &message, &buffer)) {
        CHECK(false);
        LLVMDisposeMessage(message);
    } else if (ok) {
        object = string_copy(a, {(const u8 *)LLVMGetBufferStart(buffer), (s64)LLVMGetBufferSize(buffer)});
        LLVMDisposeMemoryBuffer(buffer);
    }
    backend_free(&b);
    return object;
}

TEST(llvm_optimises_the_target_machines_code_too) {
    // With optimize, the target machine compiles at LLVM's default level,
    // not its lowest, as clang's -O2 has it: instruction selection and
    // register allocation do their own work, so the object differs from
    // the lowest level's, made from the same optimised module. Unoptimised,
    // the target machine is at the lowest level, so its object is the one
    // the lowest level makes.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "rand :: () -> s32 #foreign libc;\n"
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "main :: () {\n"
                                "    n := rand();\n"
                                "    sum := 0;\n"
                                "    for i: 0..n { sum += i * i + n; }\n"
                                "    printf(\"%lld\\n\", sum);\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String optimised = object_from(&a, m, (Target)t, true, false);
        CHECK(optimised.count > 0);
        CHECK(!string_eq(optimised, object_from(&a, m, (Target)t, true, true)));
        String plain = object_from(&a, m, (Target)t, false, false);
        CHECK(plain.count > 0);
        CHECK(string_eq(plain, object_from(&a, m, (Target)t, false, true)));
    }
    arena_free(&a);
}

TEST(llvm_optimised_uninitialised_local_is_still_a_value) {
    // A `---` local's slot is never stored to before it's read here, and a
    // read of memory nothing stored is undefined to LLVM's optimiser: it
    // would take a branch on it as unreachable, and main as nothing but
    // `unreachable`. The language gives the read an unspecified value
    // instead (spec 8.6), so an optimised main still makes one of each
    // if's two calls, for the scalar's slot and for the struct's bytes
    // alike (the optimiser may fold either if to the arm the value picks:
    // the declaration and two calls of printf).
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "Vec2 :: struct { x: int; y: int; }\n"
                                "main :: () {\n"
                                "    x : int = ---;\n"
                                "    if x == 1 { printf(\"one\\n\"); } else { printf(\"notone\\n\"); }\n"
                                "    v : Vec2 = ---;\n"
                                "    if v.y == 1 { printf(\"vone\\n\"); } else { printf(\"vnotone\\n\"); }\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = {}, error = {};
        bool ok = llvm_ir_text(&a, m, (Target)t, &text, &error, true);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
        CHECK(!contains(text, "unreachable"));
        CHECK_EQ(count_of(text, "@printf("), 3);
    }
    arena_free(&a);
}

TEST(llvm_optimiser_takes_no_procedure_for_c_libraries) {
    // LLVM knows C's library functions by name and type alone, whether the
    // module defines them or not. The program's own strlen and sqrt keep
    // their bodies (42 + 7), not C's results folded in (3 + 2), and its own
    // puts doesn't receive the call LLVM would make of printf("x\n").
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "strlen :: (s: *u8) -> int {\n"
                                "    return 42;\n"
                                "}\n"
                                "puts :: (s: *u8) -> s32 {\n"
                                "    return 7;\n"
                                "}\n"
                                "sqrt :: (x: float64) -> float64 {\n"
                                "    return 7.0;\n"
                                "}\n"
                                "main :: () {\n"
                                "    printf(\"x\\n\");\n"
                                "    printf(\"%lld\\n\", strlen(\"abc\") + cast(int) sqrt(4.0));\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = {}, error = {};
        bool ok = llvm_ir_text(&a, m, (Target)t, &text, &error, true);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
        CHECK(contains(text, "i64 49)"));
        CHECK(!contains(text, "@puts"));
    }
    arena_free(&a);
}

TEST(llvm_optimised_module_takes_the_memory_functions_names) {
    // LLVM's optimiser may make memset and memcpy intrinsics of its own,
    // which become calls of C's memset and memcpy, by name, in a module
    // that neither copies nor zeroes memory. So an optimised module always
    // takes their names, and a program's own memset is the one renamed,
    // which LLVM never calls. The loop that zeroes bytes stays a loop: LLVM
    // would make it a memset if it knew C's functions (mark_definition).
    // Unoptimised, the module makes no such call, and the program's memset
    // keeps its name.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "keep :: (f: (*u8, s32, int) -> *u8 #c_call) #foreign libc;\n"
                                "buffer :: () -> *u8 #foreign libc;\n"
                                "memset :: (p: *u8, v: s32, n: int) -> *u8 #c_call {\n"
                                "    return p;\n"
                                "}\n"
                                "main :: () {\n"
                                "    keep(memset);\n"
                                "    p := buffer();\n"
                                "    for 0..99 { (p + it).* = 0; }\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String plain = {}, optimised = {}, error = {};
        CHECK(llvm_ir_text(&a, m, (Target)t, &plain, &error, false));
        CHECK(contains(plain, "define internal ptr @memset("));
        bool ok = llvm_ir_text(&a, m, (Target)t, &optimised, &error, true);
        CHECK(ok);
        if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
        CHECK(contains(optimised, "define internal ptr @memset.1("));
        CHECK(!contains(optimised, "call ptr @memset.1("));
        CHECK(!contains(optimised, "@llvm.memset"));
    }
    arena_free(&a);
}

// Issue #156: the context (spec 7.10, 10.7).

TEST(llvm_describes_the_context) {
    // twice's hidden parameter is its first parameter variable, context,
    // artificial, a pointer to Preload's Context, and n its second; main's
    // slot is an artificial local context, and cb has none. __runtime_init
    // has no subprogram, and main's call of it no location, so a debugger
    // steps over it. twice's type, and the type of a procedure value of
    // it, list the context's pointer after the (none) result pointers, so
    // a debugger's call passes it.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "twice :: (n: int) -> int { return n; }\n"
                                          "cb :: () #c_call { }\n"
                                          "main :: () {\n"
                                          "    f := twice;\n"
                                          "    n := f(3);\n"
                                          "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String twice = defined_node(line_with(text, "!DISubprogram(name: \"twice\""));
        String main = defined_node(line_with(text, "!DISubprogram(name: \"main\""));
        String cb = defined_node(line_with(text, "!DISubprogram(name: \"cb\""));
        String integer = defined_node(line_with(text, "!DIBasicType(name: \"int\""));
        String context = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Context\""));
        String to_context = pointer_node(&a, text, context);
        CHECK(twice.count > 0 && main.count > 0 && cb.count > 0 && integer.count > 0 && to_context.count > 0);

        const char *parameter_of_twice = string_cstr(
            &a, string_printf(&a, "!DILocalVariable(name: \"context\", arg: 1, scope: " STR_FMT ",", STR_ARG(twice)));
        String parameter = line_with(text, parameter_of_twice);
        CHECK(contains(parameter, "flags: DIFlagArtificial)"));
        CHECK(string_eq(attached(&a, parameter, "type:"), to_context));
        const char *n = string_cstr(
            &a, string_printf(&a, "!DILocalVariable(name: \"n\", arg: 2, scope: " STR_FMT ",", STR_ARG(twice)));
        CHECK(contains(text, n));

        const char *local_of_main = string_cstr(
            &a, string_printf(&a, "!DILocalVariable(name: \"context\", scope: " STR_FMT ",", STR_ARG(main)));
        String local = line_with(text, local_of_main);
        CHECK(contains(local, "flags: DIFlagArtificial)"));
        CHECK(string_eq(attached(&a, local, "type:"), to_context));
        CHECK(!contains(text, string_cstr(&a, string_printf(&a, "\"context\", scope: " STR_FMT ",", STR_ARG(cb)))));
        CHECK(!contains(text, string_cstr(&a, string_printf(&a, "\"context\", arg: 1, scope: " STR_FMT ",", STR_ARG(cb)))));

        CHECK(!contains(text, "!DISubprogram(name: \"__runtime_init\""));
        String call = line_with(text, "call void @__runtime_init()");
        CHECK(call.count > 0 && !contains(call, "!dbg"));

        String expected = string_printf(&a, "!{" STR_FMT ", " STR_FMT ", " STR_FMT "}", STR_ARG(integer),
                                        STR_ARG(to_context), STR_ARG(integer));
        String subprogram = line_with(text, "!DISubprogram(name: \"twice\"");
        String subroutine = node_line(&a, text, attached(&a, subprogram, "type:"));
        String types = node_line(&a, text, attached(&a, subroutine, "types:"));
        s64 at = index_of(types, "!{");
        CHECK(at >= 0 && string_eq(string_slice(types, at, types.count - at), expected));
        CHECK(string_eq(pointed_signature(&a, text, "f"), expected));
    }
    arena_free(&a);
}

TEST(mains_hidden_parameters_have_no_debug_information) {
    // Issue #152 (spec 10.7): C's argc and argv are main's two IR
    // parameters, but no debugger is told of them: main's type is still
    // !{null}, no parameter variable is main's, and __save_command_line,
    // as __runtime_init, has no subprogram, main's call of it no location.
    // So `break main` stops at main's first line, the arguments saved.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "main :: () { }\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "define i32 @main(i32 %0, ptr %1)"));
        CHECK(!contains(text, "!DISubprogram(name: \"__save_command_line\""));
        String call = line_with(text, "call void @__save_command_line(i32 %0, ptr %1)");
        CHECK(call.count > 0 && !contains(call, "!dbg"));

        String subprogram = line_with(text, "!DISubprogram(name: \"main\"");
        String main = defined_node(subprogram);
        CHECK(main.count > 0);
        String subroutine = node_line(&a, text, attached(&a, subprogram, "type:"));
        String types = node_line(&a, text, attached(&a, subroutine, "types:"));
        s64 at = index_of(types, "!{");
        CHECK(at >= 0 && string_eq(string_slice(types, at, types.count - at), str("!{null}")));

        // No "arg: N, scope: MAIN," anywhere: a parameter variable of main.
        String scope = string_printf(&a, ", scope: " STR_FMT ",", STR_ARG(main));
        for (s64 i = 0; i + 5 <= text.count; i++) {
            if (memcmp(text.data + i, "arg: ", 5) != 0) continue;
            s64 end = i + 5;
            while (end < text.count && text.data[end] >= '0' && text.data[end] <= '9') end++;
            String rest = string_slice(text, end, text.count - end);
            CHECK(!(rest.count >= scope.count && string_eq(string_slice(rest, 0, scope.count), scope)));
        }
    }
    arena_free(&a);
}

// Issue #158: resizable arrays (spec 3.17, 10.7).

TEST(a_resizable_arrays_debug_type) {
    // [..]int is a struct named as Rig names it, of 40 bytes: a view's
    // count and data (a pointer to int), then allocated, an int, and
    // allocator, Preload's Allocator struct, each of its size at its
    // offset.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "main :: () { a : [..]int; }\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String array = struct_line(&a, text, "[..]int");
        CHECK(contains(array, "!DICompositeType(tag: DW_TAG_structure_type, name: \"[..]int\""));
        CHECK(contains(array, ", size: 320, "));
        String node = defined_node(array);
        String integer = defined_node(line_with(text, "!DIBasicType(name: \"int\""));
        String allocator = defined_node(struct_line(&a, text, "Allocator"));
        String to_int = pointer_node(&a, text, integer);
        CHECK(node.count > 0 && integer.count > 0 && allocator.count > 0 && to_int.count > 0);
        struct Member {
            const char *name;
            s64 size, offset;  // in bits
            String type;
        };
        Member members[] = {{"count", 64, 0, integer}, {"data", 64, 64, to_int}, {"allocated", 64, 128, integer},
                            {"allocator", 128, 192, allocator}};
        for (Member &member : members) {
            String line = line_with(text, string_cstr(&a, string_printf(&a,
                                                                        "!DIDerivedType(tag: DW_TAG_member, name: "
                                                                        "\"%s\", scope: " STR_FMT ",",
                                                                        member.name, STR_ARG(node))));
            CHECK(line.count > 0);
            CHECK(string_eq(attached(&a, line, "baseType:"), member.type));
            // LLVM prints no offset of 0: the size ends the line.
            if (member.offset) {
                CHECK(contains(line, string_cstr(&a, string_printf(&a, "size: %lld, offset: %lld)", (long long)member.size,
                                                                   (long long)member.offset))));
            } else {
                CHECK(contains(line, string_cstr(&a, string_printf(&a, "size: %lld)", (long long)member.size))));
                CHECK(!contains(line, "offset:"));
            }
        }
    }
    arena_free(&a);
}

TEST(a_resizable_array_of_its_own_struct_debug_type) {
    // A struct holding a resizable array of itself is made once, as one
    // holding a view of itself is (debug_struct): the array's data member
    // points at the struct's own node.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a,
                                      "Tree :: struct { value: int; children: [..]Tree; }\n"
                                      "main :: () { t : Tree; }\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(count_of(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Tree\"") == 1);
        String tree = defined_node(struct_line(&a, text, "Tree"));
        String array = defined_node(struct_line(&a, text, "[..]Tree"));
        String to_tree = pointer_node(&a, text, tree);
        CHECK(tree.count > 0 && array.count > 0 && to_tree.count > 0);
        String data = line_with(text, string_cstr(&a, string_printf(&a,
                                                                    "!DIDerivedType(tag: DW_TAG_member, name: "
                                                                    "\"data\", scope: " STR_FMT ",",
                                                                    STR_ARG(array))));
        CHECK(string_eq(attached(&a, data, "baseType:"), to_tree));
        String children = line_with(text, string_cstr(&a, string_printf(&a,
                                                                        "!DIDerivedType(tag: DW_TAG_member, name: "
                                                                        "\"children\", scope: " STR_FMT ",",
                                                                        STR_ARG(tree))));
        CHECK(string_eq(attached(&a, children, "baseType:"), array));
        CHECK(contains(children, "size: 320, offset: 64)"));
    }
    arena_free(&a);
}

// Issue #222: the switch (spec 6.14).

TEST(llvm_switch) {
    // An IR switch is LLVM's: the value, the default's label, then each
    // case's value and label; it verifies on both targets.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "main :: () {\n"
                                "    n := 2;\n"
                                "    if n == {\n"
                                "        case 1; n = 10;\n"
                                "        case 2; n = 20;\n"
                                "        case; n = 30;\n"
                                "    }\n"
                                "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(text, "switch i64 %"));
        CHECK(contains(text, ", label %"));
        CHECK(contains(text, "i64 1, label %"));
        CHECK(contains(text, "i64 2, label %"));
    }
    arena_free(&a);
}
