// test_llvm.cpp: Rig's IR to LLVM IR and object files (issue #10). Uses
// test_ir.cpp's HELLO.

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
    Ir_Proc *p = arena_new<Ir_Proc>(&a);
    p->name = str("main");
    p->is_export = true;
    p->return_type = IR_I32;
    Ir_Instr ret = {};
    ret.op = IR_RET;
    ret.result = -1;
    ret.operand = -1;
    array_add(&p->body, ret);
    array_add(&m->procs, p);
    String text = {}, error = {};
    CHECK(!llvm_ir_text(&a, m, target_host(), &text, &error));
    CHECK(contains(error, "return"));
    error = {};
    CHECK(!llvm_emit_object(m, target_host(), str(".build/never.o"), &a, &error));
    CHECK(contains(error, "return"));
    array_free(&p->body);
    array_free(&m->procs);
    arena_free(&a);
}
