/**
 * @file    test_helpers.h
 * @brief   Shared assertion macros for unit tests
 *
 * Each translation unit must define s_pass and s_fail before including this header.
 */

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>

/** Assert that two floating-point values are near each other */
#define ASSERT_FLOAT_NEAR(expected, actual, tol) \
    do { \
        float diff = (expected) - (actual); \
        if (diff < 0.0f) diff = -diff; \
        if (diff <= (tol)) { s_pass++; printf("  [PASS] %s\n", __func__); } \
        else { s_fail++; printf("  [FAIL] %s — expected %.4f got %.4f (line %d)\n", \
               __func__, (double)(expected), (double)(actual), __LINE__); } \
    } while(0)

/** Assert that two values are equal (for integers, enums, etc.) */
#define ASSERT_EQ(a, b) \
    do { if ((a)==(b)) { s_pass++; printf("  [PASS] %s\n", __func__); } \
         else { s_fail++; printf("  [FAIL] %s (line %d)\n", __func__, __LINE__); } \
    } while(0)
    
/** Assert that a condition is true */
#define ASSERT_TRUE(cond) \
    do { if (cond) { s_pass++; printf("  [PASS] %s\n", __func__); } \
         else { s_fail++; printf("  [FAIL] %s — condition false (line %d)\n", \
                __func__, __LINE__); } \
    } while(0)

#endif /* TEST_HELPERS_H */
