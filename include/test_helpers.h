/**
 * @file    test_helpers.h
 * @brief   Shared assertion macros for unit tests
 *
 * Each translation unit must define s_pass and s_fail before including this header.
 */

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include "soc_ocv.h"

/* Processed OCV–SoC table (101 rows, 1 % SoC steps) shared by every estimator.
 * The path is relative to the repository root, which is the working directory
 * the Makefile runs the test binaries from. */
#ifndef OCV_TABLE_CSV_PATH
#define OCV_TABLE_CSV_PATH \
    "data/OCV_SoC/OCV_SOC_NCA_1_folder/OCV_SOC_NCA_1_processed.csv"
#endif

/**
 * Populate the OCV table, or abort the suite.
 *
 * The table is module-level state inside soc_ocv.c and starts zero-filled, so
 * every binary exercising the OCV lookup, the EKF (which reads OCV(SoC) for its
 * measurement model) or the SoH estimator must load it once before any test
 * runs.  Skipping this silently flattens OCV(SoC) to zero, which drives the EKF
 * Kalman gain to zero and freezes its estimate.
 *
 * Must be used from a function returning int (calls return 1 on failure).
 */
#define LOAD_OCV_TABLE_OR_FAIL() \
    do { \
        if (SocOcv_LoadTableFromCsv(OCV_TABLE_CSV_PATH) != BMS_OK) { \
            printf("[FATAL] Failed to load OCV table:\n  %s\n" \
                   "  Run the tests from the repository root (e.g. via make).\n", \
                   OCV_TABLE_CSV_PATH); \
            return 1; \
        } \
    } while(0)

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
