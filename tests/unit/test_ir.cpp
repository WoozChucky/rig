// test_ir.cpp: lowering checked trees to Rig's IR (issue #10). Tokens by
// hand (helpers.cpp).

#include "test.h"

// Hello world's tokens; test_llvm.cpp uses them too.
static const char *HELLO =
    "IDENT:puts :: ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ; | "
    "IDENT:main :: ( ) { IDENT:puts ( STRING:Hello,~World! ) ; }";

TEST(ir_of_hello_world) {
    Arena a = arena_make();
    CHECK_STR(ir_print(&a, lower_tokens(&a, HELLO)),
              "global @str.0 = \"Hello, World!\\00\"\n"
              "foreign proc @puts(ptr) -> i32\n"
              "proc @__rig_main() {\n"
              "  %0 = addr @str.0\n"
              "  %1 = call @puts(%0)\n"
              "  ret\n"
              "}\n"
              "export proc @main() -> i32 {\n"
              "  call @__rig_main()\n"
              "  %0 = const i32 0\n"
              "  ret %0\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_internal_call) {
    Arena a = arena_make();
    CHECK_STR(ir_print(&a, lower_tokens(&a, "IDENT:puts :: ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ; | "
                                            "IDENT:greet :: ( ) { IDENT:puts ( STRING:hi ) ; } | "
                                            "IDENT:main :: ( ) { IDENT:greet ( ) ; IDENT:greet ( ) ; }")),
              "global @str.0 = \"hi\\00\"\n"
              "foreign proc @puts(ptr) -> i32\n"
              "proc @greet() {\n"
              "  %0 = addr @str.0\n"
              "  %1 = call @puts(%0)\n"
              "  ret\n"
              "}\n"
              "proc @__rig_main() {\n"
              "  call @greet()\n"
              "  call @greet()\n"
              "  ret\n"
              "}\n"
              "export proc @main() -> i32 {\n"
              "  call @__rig_main()\n"
              "  %0 = const i32 0\n"
              "  ret %0\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_integer_arguments) {
    // A constant takes its parameter's type; a narrower value is widened by
    // its sign (spec 6.4).
    Arena a = arena_make();
    String ir = ir_print(&a, lower_tokens(&a, "IDENT:f :: ( IDENT:x : IDENT:s64 , IDENT:y : IDENT:u8 ) #foreign IDENT:libc ; | "
                                              "IDENT:g :: ( ) -> IDENT:s32 #foreign IDENT:libc ; | "
                                              "IDENT:h :: ( ) -> IDENT:u16 #foreign IDENT:libc ; | "
                                              "IDENT:k :: ( IDENT:z : IDENT:u64 ) #foreign IDENT:libc ; | "
                                              "IDENT:main :: ( ) { IDENT:f ( IDENT:g ( ) , INT:255 ) ; "
                                              "IDENT:k ( IDENT:h ( ) ) ; }"));
    CHECK_STR(ir,
              "foreign proc @f(i64, i8 zeroext)\n"
              "foreign proc @g() -> i32\n"
              "foreign proc @h() -> i16 zeroext\n"
              "foreign proc @k(i64)\n"
              "proc @__rig_main() {\n"
              "  %0 = call @g()\n"
              "  %1 = sext i32 %0 to i64\n"
              "  %2 = const i8 255\n"
              "  call @f(%1, %2)\n"
              "  %3 = call @h()\n"
              "  %4 = zext i16 %3 to i64\n"
              "  call @k(%4)\n"
              "  ret\n"
              "}\n"
              "export proc @main() -> i32 {\n"
              "  call @__rig_main()\n"
              "  %0 = const i32 0\n"
              "  ret %0\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_keeps_the_sign_of_narrow_parameters) {
    // The C calling conventions widen an i8 or i16 by its sign at a call
    // (llvm_backend.cpp), so a signature keeps it, on parameters and on the
    // return; wider integers and pointers need no mark.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_tokens(&a, "IDENT:f :: ( IDENT:a : IDENT:s8 , IDENT:b : IDENT:u8 , IDENT:c : IDENT:s16 , "
                                              "IDENT:d : IDENT:u16 , IDENT:e : IDENT:s32 , IDENT:g : IDENT:u64 , "
                                              "IDENT:p : * IDENT:u8 ) -> IDENT:s8 #foreign IDENT:libc ; | "
                                              "IDENT:take :: ( IDENT:x : IDENT:u16 ) { } | "
                                              "IDENT:main :: ( ) { IDENT:take ( INT:7 ) ; }"));
    CHECK(contains(ir, "foreign proc @f(i8 signext, i8 zeroext, i16 signext, i16 zeroext, i32, i64, ptr) -> i8 signext\n"));
    CHECK(contains(ir, "proc @take(i16 zeroext) {\n"));
    arena_free(&a);
}
