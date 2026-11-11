// test_harness.cpp: tests of the test harness itself. bin/unit-tests
// --self-test runs only these, and passes only when the passing one passes
// and the deliberately failing one fails: a harness that never fails proves
// nothing.

#include "test.h"

TEST(harness_checks_pass) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(2 + 2, 4);
}

// Expected to fail: --self-test checks that it does. Normal runs skip it.
TEST(harness_counts_failures_deliberately) {
    CHECK_EQ(1, 2);
}
