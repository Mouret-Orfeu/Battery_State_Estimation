/**
 * @file    test_coulomb.c
 * @brief   Unit tests for Coulomb Counting SoC estimator
 *
 * @author  Kamal Kadakara
 */

#include <stdio.h>
#include <math.h>
#include "bms_types.h"
#include "soc_coulomb.h"

static int s_pass = 0, s_fail = 0;

#define FLOAT_TOL 0.01f   /* 0.01% SoC tolerance */

#define ASSERT_FLOAT_NEAR(expected, actual, tol) \
    do { \
        float diff = (expected) - (actual); \
        if (diff < 0.0f) diff = -diff; \
        if (diff <= (tol)) { s_pass++; printf("  [PASS] %s\n", __func__); } \
        else { s_fail++; printf("  [FAIL] %s — expected %.4f got %.4f (line %d)\n", \
               __func__, (double)(expected), (double)(actual), __LINE__); } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a)==(b)) { s_pass++; printf("  [PASS] %s\n", __func__); } \
         else { s_fail++; printf("  [FAIL] %s (line %d)\n", __func__, __LINE__); } \
    } while(0)

/* ---- Tests ---- */

void test_init_sets_correct_soc(void)
{
    Bms_SocState_t state = {0};
    SocCoulomb_Init(&state, 80.0f);
    ASSERT_FLOAT_NEAR(80.0f, state.soc_pct, FLOAT_TOL);
}

void test_zero_current_no_change(void)
{
    Bms_SocState_t state = {0};
    SocCoulomb_Init(&state, 50.0f);
    SocCoulomb_Update(&state, 0.0f, 0.1f);
    ASSERT_FLOAT_NEAR(50.0f, state.soc_pct, FLOAT_TOL);
}

void test_discharge_reduces_soc(void)
{
    Bms_SocState_t state = {0};
    SocCoulomb_Init(&state, 50.0f);
    /* -60A for 3600s (1 full Ah) on 60Ah cell = -1/60 * 100 = -1.667% */
    SocCoulomb_Update(&state, -60.0f, 3600.0f);
    ASSERT_FLOAT_NEAR(0.0f, state.soc_pct, 0.1f);  /* Should hit ~0% */
}

void test_charge_increases_soc(void)
{
    Bms_SocState_t state = {0};
    SocCoulomb_Init(&state, 50.0f);
    /* +30A for 3600s = 0.5 Ah on 60Ah = +0.833% */
    float soc_before = state.soc_pct;
    SocCoulomb_Update(&state, 30.0f, 3600.0f);
    /* SoC should increase */
    ASSERT_EQ(1, state.soc_pct > soc_before);
}

void test_soc_clamps_at_100(void)
{
    Bms_SocState_t state = {0};
    SocCoulomb_Init(&state, 99.9f);
    /* Large charge pulse */
    SocCoulomb_Update(&state, 100.0f, 360.0f);
    ASSERT_FLOAT_NEAR(100.0f, state.soc_pct, FLOAT_TOL);
}

void test_soc_clamps_at_zero(void)
{
    Bms_SocState_t state = {0};
    SocCoulomb_Init(&state, 0.1f);
    /* Large discharge pulse */
    SocCoulomb_Update(&state, -200.0f, 360.0f);
    ASSERT_FLOAT_NEAR(0.0f, state.soc_pct, FLOAT_TOL);
}

void test_null_state_returns_error(void)
{
    Bms_Error_t ret = SocCoulomb_Update(NULL, -10.0f, 0.1f);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, ret);
}

/* ============================================================ */
int main(void)
{
    printf("\n=== Coulomb Counting SoC Unit Tests ===\n\n");

    test_init_sets_correct_soc();
    test_zero_current_no_change();
    test_discharge_reduces_soc();
    test_charge_increases_soc();
    test_soc_clamps_at_100();
    test_soc_clamps_at_zero();
    test_null_state_returns_error();

    printf("\n--- Results: %d passed, %d failed ---\n\n", s_pass, s_fail);
    return (s_fail > 0) ? 1 : 0;
}
