/**
 * @file    test_ocv.c
 * @brief   Unit tests for OCV Lookup Table SoC estimator
 *
 * @author  Orfeu Mouret
 */

#include <stdio.h>
#include <math.h>
#include "bms_types.h"
#include "soc_ocv.h"
#include "test_helpers.h"

static int s_pass = 0, s_fail = 0;

#define FLOAT_TOL   0.01f   /* 0.01% SoC tolerance  */
#define VOLT_TOL    0.1f    /* 0.1 mV voltage tolerance */

/* ---- Tests: SocOcv_LookupSoc ---- */

void test_lookup_null_output_returns_error(void)
{
    Bms_Error_t err = SocOcv_LookupSoc(3660.0f, NULL);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, err);
}

void test_lookup_below_range_returns_oot(void)
{
    float soc = -1.0f;
    Bms_Error_t err = SocOcv_LookupSoc(2999.0f, &soc);
    ASSERT_EQ(BMS_ERR_VOLTAGE_OOT, err);
    ASSERT_FLOAT_NEAR(0.0f, soc, FLOAT_TOL);
}

void test_lookup_above_range_returns_oot(void)
{
    float soc = -1.0f;
    Bms_Error_t err = SocOcv_LookupSoc(4201.0f, &soc);
    ASSERT_EQ(BMS_ERR_VOLTAGE_OOT, err);
    ASSERT_FLOAT_NEAR(100.0f, soc, FLOAT_TOL);
}

void test_lookup_at_min_boundary_returns_oot(void)
{
    /* The boundary check is <=, so exactly 3000 mV is treated as out-of-table */
    float soc = -1.0f;
    Bms_Error_t err = SocOcv_LookupSoc(3000.0f, &soc);
    ASSERT_EQ(BMS_ERR_VOLTAGE_OOT, err);
    ASSERT_FLOAT_NEAR(0.0f, soc, FLOAT_TOL);
}

void test_lookup_exact_table_entry(void)
{
    /* 3660 mV is the exact table entry for 50% SoC */
    float soc = 0.0f;
    Bms_Error_t err = SocOcv_LookupSoc(3660.0f, &soc);
    ASSERT_EQ(BMS_OK, err);
    ASSERT_FLOAT_NEAR(50.0f, soc, FLOAT_TOL);
}

void test_lookup_interpolates_midpoint(void)
{
    /* 3675 mV is the midpoint between 50% (3660 mV) and 55% (3690 mV) → 52.5% */
    float soc = 0.0f;
    Bms_Error_t err = SocOcv_LookupSoc(3675.0f, &soc);
    ASSERT_EQ(BMS_OK, err);
    ASSERT_FLOAT_NEAR(52.5f, soc, FLOAT_TOL);
}

/* ---- Tests: SocOcv_GetOcv ---- */

void test_getocv_below_min_clamps(void)
{
    /* Negative SoC must return the minimum table voltage */
    ASSERT_FLOAT_NEAR(3000.0f, SocOcv_GetOcv(-5.0f), VOLT_TOL);
}

void test_getocv_above_max_clamps(void)
{
    /* SoC above 100% must return the maximum table voltage */
    ASSERT_FLOAT_NEAR(4200.0f, SocOcv_GetOcv(105.0f), VOLT_TOL);
}

void test_getocv_at_zero_pct(void)
{
    ASSERT_FLOAT_NEAR(3000.0f, SocOcv_GetOcv(0.0f), VOLT_TOL);
}

void test_getocv_at_full_pct(void)
{
    ASSERT_FLOAT_NEAR(4200.0f, SocOcv_GetOcv(100.0f), VOLT_TOL);
}

void test_getocv_at_mid_table(void)
{
    /* Exact table entry — no interpolation involved */
    ASSERT_FLOAT_NEAR(3660.0f, SocOcv_GetOcv(50.0f), VOLT_TOL);
}

void test_getocv_interpolates_midpoint(void)
{
    /* 52.5% lies midway between 50% (3660 mV) and 55% (3690 mV) → 3675 mV */
    ASSERT_FLOAT_NEAR(3675.0f, SocOcv_GetOcv(52.5f), VOLT_TOL);
}

/* ---- Round-trip ---- */

void test_round_trip_consistency(void)
{
    /* GetOcv followed by LookupSoc must recover the original SoC for a mid-table value */
    float soc = 0.0f;
    SocOcv_LookupSoc(SocOcv_GetOcv(70.0f), &soc);
    ASSERT_FLOAT_NEAR(70.0f, soc, FLOAT_TOL);
}

/* ============================================================ */
int main(void)
{
    printf("\n=== OCV Lookup Table Unit Tests ===\n\n");

    test_lookup_null_output_returns_error();
    test_lookup_below_range_returns_oot();
    test_lookup_above_range_returns_oot();
    test_lookup_at_min_boundary_returns_oot();
    test_lookup_exact_table_entry();
    test_lookup_interpolates_midpoint();
    test_getocv_below_min_clamps();
    test_getocv_above_max_clamps();
    test_getocv_at_zero_pct();
    test_getocv_at_full_pct();
    test_getocv_at_mid_table();
    test_getocv_interpolates_midpoint();
    test_round_trip_consistency();

    printf("\n--- Results: %d passed, %d failed ---\n\n", s_pass, s_fail);
    return (s_fail > 0) ? 1 : 0;
}
