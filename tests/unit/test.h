// test.h: Rig's unit test harness, a few macros and no framework.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
//
// TEST registers the function before main runs (a static object's
// constructor), so a test file needs no list of its tests. A failed check
// prints where and what, marks the test failed, and the test goes on.

#pragma once

#include "../../src/common.h"

typedef void (*Test_Function)();

void test_register(const char *name, Test_Function function);
void test_fail(const char *file, int line, const char *message);
void test_fail_values(const char *file, int line, const char *check, s64 a, s64 b);

struct Test_Registrar {
    Test_Registrar(const char *name, Test_Function function) { test_register(name, function); }
};

#define TEST(name)                                                    \
    static void test_##name();                                        \
    static Test_Registrar test_registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) test_fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

// Compares two integers (any integer types, or pointers cast to them) and
// prints both when they differ.
#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        s64 check_a_ = (s64)(a), check_b_ = (s64)(b);                                      \
        if (check_a_ != check_b_)                                                          \
            test_fail_values(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")", check_a_, check_b_); \
    } while (0)
