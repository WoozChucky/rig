// test_llvm.cpp: Rig's IR to LLVM IR and object files: hello world from
// tokens (issue #10), then IR built by hand for issues #32 to #37 (integer
// widths, blocks and branches, the runtime's panic, several results,
// globals and floats) and #52 (the panic flushing first).
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
        CHECK(contains(text, "define internal void @__rig_main()"));
        CHECK(contains(text, "call i32 @puts(ptr @str.0)"));
        CHECK(contains(text, "define i32 @main()"));
        CHECK(contains(text, "call void @__rig_main()"));
        CHECK(contains(text, "ret i32 0"));
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
        } else {
            CHECK(memcmp(bytes.data, "\x7f" "ELF", 4) == 0);
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

// __rig_main :: () { panic with "oops" }.
static Ir_Module *panic_module(Arena *a) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    s32 message = add_string(m, "str.0", "oops");
    Ir_Proc *p = new_proc_returning(m, a, "__rig_main", IR_VOID);
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
    Ir_Proc *p = new_proc_returning(m, &a, "__rig_main", IR_VOID);
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
    p = new_proc_returning(m, &a, "__rig_main", IR_VOID);
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
