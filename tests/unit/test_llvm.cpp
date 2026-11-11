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
