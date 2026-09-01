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

#define FLOAT_TOL   0.01f   /* 0.01% SoC tolerance */
#define VOLT_TOL    0.1f    /* 0.1 mV voltage tolerance */

/* The NCA processed OCV-SoC CSV path comes from OCV_TABLE_CSV_PATH in test_helpers.h */

/* ---- OCV table reference values ----
 * Switch active/commented block to match the loaded chemistry.
 *
 * Old NMC hardcoded table (25°C, 1C):
 * #define OCV_MIN_MV       3000.0f
 * #define OCV_MAX_MV       4200.0f
 * #define OCV_BELOW_MIN_MV 2999.0f
 * #define OCV_ABOVE_MAX_MV 4201.0f
 * #define OCV_50PCT_MV     3660.0f
 * #define OCV_55PCT_MV     3690.0f
 * #define OCV_MID_MV       3675.0f  // midpoint 50–55% → 52.5% SoC
 *
 * NCA table — OCV_SOC_NCA_1_processed.csv (101 entries, 1 % SoC steps).
 * Interpolation is checked between two *adjacent* entries, so the midpoint test
 * uses 52 % / 53 % rather than the 50 % / 55 % pair the old 5 % table allowed.
 */
#define OCV_MIN_MV       2835.0f
#define OCV_MAX_MV       4172.0f
#define OCV_BELOW_MIN_MV 2800.0f
#define OCV_ABOVE_MAX_MV 4201.0f
#define OCV_50PCT_MV     3680.0f
#define OCV_52PCT_MV     3696.0f
#define OCV_53PCT_MV     3705.0f
#define OCV_MID_MV       3700.5f   /* midpoint 52–53% → 52.5% SoC */

/* ---- Tests: SocOcv_LoadTableFromCsv ---- */

void test_load_csv_null_path_returns_error(void)
{
    ASSERT_EQ(BMS_ERR_INVALID_PARAM, SocOcv_LoadTableFromCsv(NULL));
}

void test_load_csv_nonexistent_path_returns_error(void)
{
    ASSERT_EQ(BMS_ERR_INVALID_PARAM, SocOcv_LoadTableFromCsv("/nonexistent/path.csv"));
}

void test_load_csv_valid_nca_file_returns_ok(void)
{
    ASSERT_EQ(BMS_OK, SocOcv_LoadTableFromCsv(OCV_TABLE_CSV_PATH));
}

/* ---- Tests: SocOcv_LookupSoc ---- */

void test_lookup_null_output_returns_error(void)
{
    Bms_Error_t err = SocOcv_LookupSoc(OCV_50PCT_MV, NULL);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, err);
}

void test_lookup_below_range_returns_oot(void)
{
    float soc = -1.0f;
    Bms_Error_t err = SocOcv_LookupSoc(OCV_BELOW_MIN_MV, &soc);
    ASSERT_EQ(BMS_ERR_VOLTAGE_OOT, err);
    ASSERT_FLOAT_NEAR(0.0f, soc, FLOAT_TOL);
}

void test_lookup_above_range_returns_oot(void)
{
    float soc = -1.0f;
    Bms_Error_t err = SocOcv_LookupSoc(OCV_ABOVE_MAX_MV, &soc);
    ASSERT_EQ(BMS_ERR_VOLTAGE_OOT, err);
    ASSERT_FLOAT_NEAR(100.0f, soc, FLOAT_TOL);
}

void test_lookup_at_min_boundary_returns_oot(void)
{
    /* Boundary check is <=, so exactly OCV_MIN_MV (table[0]) is out-of-table */
    float soc = -1.0f;
    Bms_Error_t err = SocOcv_LookupSoc(OCV_MIN_MV, &soc);
    ASSERT_EQ(BMS_ERR_VOLTAGE_OOT, err);
    ASSERT_FLOAT_NEAR(0.0f, soc, FLOAT_TOL);
}

void test_lookup_exact_table_entry(void)
{
    /* OCV_50PCT_MV is the table entry for 50% SoC */
    float soc = 0.0f;
    Bms_Error_t err = SocOcv_LookupSoc(OCV_50PCT_MV, &soc);
    ASSERT_EQ(BMS_OK, err);
    ASSERT_FLOAT_NEAR(50.0f, soc, FLOAT_TOL);
}

void test_lookup_interpolates_midpoint(void)
{
    /* OCV_MID_MV is midway between the 52% and 53% entries → 52.5% SoC */
    float soc = 0.0f;
    Bms_Error_t err = SocOcv_LookupSoc(OCV_MID_MV, &soc);
    ASSERT_EQ(BMS_OK, err);
    ASSERT_FLOAT_NEAR(52.5f, soc, FLOAT_TOL);
}

/* ---- Tests: SocOcv_GetOcv ---- */

void test_getocv_below_min_clamps(void)
{
    ASSERT_FLOAT_NEAR(OCV_MIN_MV, SocOcv_GetOcv(-5.0f), VOLT_TOL);
}

void test_getocv_above_max_clamps(void)
{
    ASSERT_FLOAT_NEAR(OCV_MAX_MV, SocOcv_GetOcv(105.0f), VOLT_TOL);
}

void test_getocv_at_zero_pct(void)
{
    ASSERT_FLOAT_NEAR(OCV_MIN_MV, SocOcv_GetOcv(0.0f), VOLT_TOL);
}

void test_getocv_at_full_pct(void)
{
    ASSERT_FLOAT_NEAR(OCV_MAX_MV, SocOcv_GetOcv(100.0f), VOLT_TOL);
}

void test_getocv_at_mid_table(void)
{
    /* Exact table entry — no interpolation involved */
    ASSERT_FLOAT_NEAR(OCV_50PCT_MV, SocOcv_GetOcv(50.0f), VOLT_TOL);
}

void test_getocv_interpolates_midpoint(void)
{
    /* 52.5% lies midway between the 52% and 53% entries → OCV_MID_MV */
    ASSERT_FLOAT_NEAR(OCV_MID_MV, SocOcv_GetOcv(52.5f), VOLT_TOL);
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

    /* Prerequisite: populate the table — all lookup/getocv tests depend on this */
    LOAD_OCV_TABLE_OR_FAIL();

    test_load_csv_null_path_returns_error();
    test_load_csv_nonexistent_path_returns_error();
    test_load_csv_valid_nca_file_returns_ok();  /* reloads the table cleanly */

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
