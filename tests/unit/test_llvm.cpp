// test_llvm.cpp: Rig's IR to LLVM IR and object files: hello world from
// tokens (issue #10), then IR built by hand for issues #32 to #37 (integer
// widths, blocks and branches, the runtime's panic, several results,
// globals and floats), #52 (the panic flushing first) and #56 (memory);
// last, milestone 2c's line tables (#71), on modules lowered from source.
// Uses test_ir.cpp's HELLO and max_module.

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
    Arena a = arena_make();
    Ir_Module *m = lower_tokens(&a, HELLO);
    os_make_directory(str(".build"));
    const char *paths[] = {".build/ir-test-windows.obj", ".build/ir-test-linux.o"};
    for (int t = 0; t < 2; t++) {
        String error = {};
        bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), &a, &error);
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
    arena_free(&a);
}

TEST(llvm_reports_a_module_that_doesnt_verify) {
    // An IR the lowering never makes: a procedure returning i32 whose ret
    // gives nothing. Both entry points refuse it with LLVM's reason.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "main", IR_I32);
    p->is_export = true;
    emit_ret(p, ir_new_block(&a, p), -1);
    String text = {}, error = {};
    CHECK(!llvm_ir_text(&a, m, target_host(), &text, &error));
    CHECK(contains(error, "return"));
    error = {};
    CHECK(!llvm_emit_object(m, target_host(), str(".build/never.o"), &a, &error));
    CHECK(contains(error, "return"));
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
    array_add(&m->globals[table].relocs, {8, text});
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
    array_add(&m->globals[pair].relocs, {0, text});
    array_add(&m->globals[pair].relocs, {8, text});
    s32 odd = add_byte_global(m, "odd", 12, 4);
    u8 *odd_bytes = (u8 *)arena_alloc(&a, 12, 1);  // zeroed
    odd_bytes[0] = 1;
    odd_bytes[11] = 2;
    m->globals[odd].bytes = {odd_bytes, 12};
    array_add(&m->globals[odd].relocs, {3, text});
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
        bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), &a, &error);
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

// The first line of `text` containing `needle`, without its "\n"; {} when
// there's none. A needle starting with "\n" finds the line after that one.
static String line_with(String text, const char *needle) {
    s64 at = index_of(text, needle);
    if (at < 0) return {};
    if (needle[0] == '\n') at++;
    s64 start = at, end = at;
    while (start > 0 && text.data[start - 1] != '\n') start--;
    while (end < text.count && text.data[end] != '\n') end++;
    return string_slice(text, start, end - start);
}

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
    // uses, and which suits Rig's), produced by "rig 0.1.0 (LLVM X.Y.Z)",
    // the LLVM rig is linked with (issue #104: no "clang", which nothing
    // needs). The file is the source's absolute path, in the directory rig
    // ran in, with its MD5. Module flags say which debug information LLVM
    // writes: CodeView on Windows, DWARF 5 on Linux.
    Arena a = arena_make();
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
    String producer = string_printf(&a, "producer: \"rig %s (LLVM %u.%u.%u)\"", RIG_VERSION, major, minor, patch);
    Ir_Module *m = lower_source(&a, LINES);
    for (int t = 0; t < 2; t++) {
        Target target = (Target)t;
        String text = llvm_text(&a, m, target);
        CHECK(contains(text, "!DICompileUnit(language: DW_LANG_C99"));
        CHECK(contains(text, string_cstr(&a, producer)));
        CHECK(contains(text, "isOptimized: false"));
        CHECK(contains(text, "emissionKind: FullDebug"));
        String file = string_printf(&a, "!DIFile(filename: \"" STR_FMT "\", directory: \"" STR_FMT "\"",
                                    STR_ARG(llvm_escaped(&a, m->source_path)), STR_ARG(llvm_escaped(&a, m->directory)));
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
    m->source_path = str("\\\\server\\share\\dir\\lines.rig");
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
                                "main :: () {\n"
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
            bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), &a, &error);
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
        bool ok = llvm_emit_object(m, (Target)t, str(paths[t]), &a, &error);
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
    // read through it on Linux (DW_OP_deref) and on Windows a pointer to its
    // type, the slot itself (declare_variables says why); a bool parameter,
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
            {"!DILocalVariable(name: \"a\", arg: 1, ",
             (Target)t == TARGET_WINDOWS_X64 ? "!DIExpression()" : "!DIExpression(DW_OP_deref)", IN_SUM},
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
        // a's type: the view's own on Linux, where the expression reads
        // through the slot; a 64-bit pointer to it on Windows, where the
        // slot is the variable.
        String view = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"[]int\""));
        String view_type = attached(&a, line_with(text, "!DILocalVariable(name: \"a\", arg: 1, "), "type:");
        CHECK(view.count > 0 && view_type.count > 0);
        if ((Target)t == TARGET_WINDOWS_X64) {
            String pointer = node_line(&a, text, view_type);
            CHECK(contains(pointer, "!DIDerivedType(tag: DW_TAG_pointer_type, "));
            CHECK(contains(pointer, string_cstr(&a, string_printf(&a, "baseType: " STR_FMT ", size: 64",
                                                                 STR_ARG(view)))));
        } else {
            CHECK(string_eq(view_type, view));
        }
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

TEST(llvm_procedure_types_say_how_they_are_called) {
    // A procedure's debug type is how it's called (spec 10.4), not how C
    // would pass the same types: an aggregate parameter is a pointer to the
    // caller's copy, and an aggregate result a hidden pointer parameter
    // before the visible ones, so the procedure returns its one scalar
    // result, or nothing. With several scalar results it returns nothing a
    // C type describes, so none either.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Pair :: struct { a: int; b: int; }\n"
                                "make :: (n: int) -> Pair { return Pair.{n, n}; }\n"
                                "sum :: (p: Pair, s: string) -> int { return p.a; }\n"
                                "both :: (n: int) -> Pair, int { return Pair.{n, n}, n; }\n"
                                "twice :: (n: int) -> int, int { return n, n; }\n"
                                "pairs :: (n: [2]int) -> Pair, Pair { return Pair.{1, 2}, Pair.{3, 4}; }\n"
                                "main :: () { }\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        String integer = defined_node(line_with(text, "!DIBasicType(name: \"int\""));
        String pair = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"Pair\""));
        String string = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_structure_type, name: \"string\""));
        String array = defined_node(line_with(text, "!DICompositeType(tag: DW_TAG_array_type, baseType: "));
        String to_pair = pointer_node(&a, text, pair);
        CHECK(integer.count > 0 && pair.count > 0 && string.count > 0 && array.count > 0 && to_pair.count > 0);
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
                                    STR_ARG(pointer_node(&a, text, array)))},
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
