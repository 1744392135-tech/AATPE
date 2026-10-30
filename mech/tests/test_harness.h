
#ifndef MECH_TEST_HARNESS_H
#define MECH_TEST_HARNESS_H

#include <stdio.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; \
        printf("  [FAIL] %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_MSG(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; \
        printf("  [FAIL] %s:%d: CHECK(%s) :: %s\n", __FILE__, __LINE__, #cond, msg); } \
} while (0)

static int test_summary(const char *name)
{
    printf("== %s : %d checks, %d failures ==\n", name, g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
