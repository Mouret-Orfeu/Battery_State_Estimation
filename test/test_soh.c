/**
 * @file    test_soh.c
 * @brief   Unit tests for the State-of-Health (SoH) estimator
 *
 * Test time step: 10 s (coarser than production 0.4 s to keep runtimes short).
 * A 2-hour rest therefore completes in exactly 720 steps.
 *
 * Synthetic scenario used by several tests — a charge window, since only those
 * produce a Qmax update:
 *   Phase 1 — 720 steps at rest   (I = 0,      V = OCV(10 %))
 *   Phase 2 — 576 steps active    (I = +1.7 A, V = OCV(90 %))
 *   Phase 3 — 720 steps at rest   (I = 0,      V = OCV(90 %))
 *
 *   Q = 1.7 A × 5760 s / 3600 = 2.72 Ah
 *   ΔSoC = +80 %  →  Qmax = 2.72 / 0.8 = 3.4 Ah  →  SoH = 100 %
 *
 * The current is C/2 for the 3.4 Ah cell, sized so a healthy cell lands exactly
 * on SoH = 100 % rather than being clamped there from above.
 *
 * @author  Orfeu Mouret
 */

#include <stdio.h>
#include <math.h>
#include "bms_types.h"
#include "soh.h"
#include "soc_ocv.h"
#include "test_helpers.h"

static int s_pass = 0, s_fail = 0;

#define DT_TEST         10.0f    /* 10 s time step                              */
#define REST_STEPS      720U     /* 720 × 10 s = 7200 s = SOH_MIN_REST_DURATION */
#define ACTIVE_STEPS    576U     /* 576 × 10 s × 1.7 A / 3600 = 2.72 Ah        */
#define CHARGE_A        (1.7f)   /* C/2 for the 3.4 Ah cell                    */
#define DISCHARGE_A     (-1.7f)  /* same magnitude, opposite direction         */

#define SOH_TOL         0.5f     /* SoH comparison tolerance [%] */

/* ---- Helpers ---- */

static void run_rest(Soh_State_t *s, float v_mv, uint32_t n_steps, float *t_s)
{
    for (uint32_t i = 0U; i < n_steps; i++) {
        Soh_Update(s, 0.0f, v_mv, *t_s, DT_TEST);
        *t_s += DT_TEST;
    }
}

static void run_active(Soh_State_t *s, float current_a, float v_mv,
                       uint32_t n_steps, float *t_s)
{
    for (uint32_t i = 0U; i < n_steps; i++) {
        Soh_Update(s, current_a, v_mv, *t_s, DT_TEST);
        *t_s += DT_TEST;
    }
}

/* Build the standard healthy-cell scenario (a charge window) and return final t_s */
static float run_healthy_scenario(Soh_State_t *s)
{
    float v_10pct = SocOcv_GetOcv(10.0f);
    float v_90pct = SocOcv_GetOcv(90.0f);
    float t = 0.0f;

    run_rest  (s, v_10pct, REST_STEPS,              &t);
    run_active(s, CHARGE_A, v_90pct, ACTIVE_STEPS,  &t);
    run_rest  (s, v_90pct, REST_STEPS,              &t);
    return t;
}

/* ---- Tests ---- */

void test_init_sets_seeking_rest_phase(void)
{
    Soh_State_t s;
    Soh_Init(&s);
    ASSERT_EQ(SOH_PHASE_SEEKING_REST, s.phase);
}

void test_init_sets_no_valid_estimate(void)
{
    Soh_State_t s;
    Soh_Init(&s);
    ASSERT_EQ(0U, s.soh_update_count);
}

void test_get_returns_minus1_before_estimate(void)
{
    Soh_State_t s;
    Soh_Init(&s);
    ASSERT_FLOAT_NEAR(-1.0f, Soh_Get(&s), 0.001f);
}

void test_null_update_returns_error(void)
{
    Bms_Error_t err = Soh_Update(NULL, 0.0f, 3600.0f, 0.0f, DT_TEST);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, err);
}

void test_null_get_returns_minus1(void)
{
    ASSERT_FLOAT_NEAR(-1.0f, Soh_Get(NULL), 0.001f);
}

void test_no_update_before_first_rest(void)
{
    /* Only active current, no rest → no SoH */
    Soh_State_t s;
    Soh_Init(&s);
    float t = 0.0f;
    run_active(&s, DISCHARGE_A, SocOcv_GetOcv(50.0f), 1000U, &t);
    ASSERT_EQ(0U, s.soh_update_count);
}

void test_no_update_below_min_delta_soc(void)
{
    /* A charge window, so only its width can disqualify it:
     * ΔSoC = +20 % (< 80 % threshold) → no SoH update expected */
    Soh_State_t s;
    Soh_Init(&s);
    float v_60pct = SocOcv_GetOcv(60.0f);
    float v_80pct = SocOcv_GetOcv(80.0f);
    float t = 0.0f;

    /* First rest at SoC = 60 % */
    run_rest(&s, v_60pct, REST_STEPS, &t);
    /* Active for 1440 s; ΔSoC comes from the two OCV lookups (60 % → 80 %) */
    run_active(&s, CHARGE_A, v_80pct, 144U, &t);
    /* Second rest at SoC = 80 % */
    run_rest(&s, v_80pct, REST_STEPS, &t);

    ASSERT_EQ(0U, s.soh_update_count);
}

void test_no_update_on_discharge_window(void)
{
    /* A full-depth discharge window must be rejected however wide its ΔSoC:
     * only charge windows are reproducible enough to measure Qmax from */
    Soh_State_t s;
    Soh_Init(&s);
    float v_90pct = SocOcv_GetOcv(90.0f);
    float v_10pct = SocOcv_GetOcv(10.0f);
    float t = 0.0f;

    run_rest  (&s, v_90pct, REST_STEPS,               &t);
    run_active(&s, DISCHARGE_A, v_10pct, ACTIVE_STEPS, &t);
    run_rest  (&s, v_10pct, REST_STEPS,               &t);

    ASSERT_EQ(0U, s.soh_update_count);
    /* The rejected window must still leave the closing rest SoC as the reference
     * for the next window, otherwise a following charge would be mismeasured */
    ASSERT_FLOAT_NEAR(10.0f, s.soc_at_rest_entry_pct, 1.0f);
}

void test_healthy_cell_soh_near_100(void)
{
    /* Full 80 % ΔSoC charge on a nominal cell → SoH = 100 % */
    Soh_State_t s;
    Soh_Init(&s);
    run_healthy_scenario(&s);

    ASSERT_EQ(1U, s.soh_update_count);
    ASSERT_FLOAT_NEAR(100.0f, s.soh_pct, SOH_TOL);
}

void test_soh_update_time_is_set(void)
{
    /* soh_update_time_s must point to the step when the second rest was confirmed */
    Soh_State_t s;
    Soh_Init(&s);
    run_healthy_scenario(&s);

    /* Expected: REST + ACTIVE + REST confirmation step */
    float expected_t = (float)(REST_STEPS + ACTIVE_STEPS + REST_STEPS) * DT_TEST;
    ASSERT_FLOAT_NEAR(expected_t, s.soh_update_time_s, DT_TEST);
}

void test_two_consecutive_updates(void)
{
    /* Two charge windows → two SoH updates.  The discharge needed to return to
     * 10 % sits between them as a third window, and must be silently rejected. */
    Soh_State_t s;
    Soh_Init(&s);
    float t = 0.0f;
    float v_10pct = SocOcv_GetOcv(10.0f);
    float v_90pct = SocOcv_GetOcv(90.0f);

    /* Window 1 — charge 10 % → 90 % */
    run_rest  (&s, v_10pct, REST_STEPS,               &t);
    run_active(&s, CHARGE_A, v_90pct, ACTIVE_STEPS,   &t);
    run_rest  (&s, v_90pct, REST_STEPS,               &t);

    /* Window 2 — discharge back down, rejected but repositions the reference */
    run_active(&s, DISCHARGE_A, v_10pct, ACTIVE_STEPS, &t);
    run_rest  (&s, v_10pct, REST_STEPS,                &t);

    /* Window 3 — charge 10 % → 90 % again */
    run_active(&s, CHARGE_A, v_90pct, ACTIVE_STEPS,   &t);
    run_rest  (&s, v_90pct, REST_STEPS,               &t);

    ASSERT_EQ(2U, s.soh_update_count);
}

/* ============================================================ */
int main(void)
{
    printf("\n=== SoH Estimator Unit Tests ===\n\n");

    /* Prerequisite: the estimator resolves rest SoC through SocOcv_LookupSoc(),
     * and the scenario helpers build their voltages from the same table */
    LOAD_OCV_TABLE_OR_FAIL();

    test_init_sets_seeking_rest_phase();
    test_init_sets_no_valid_estimate();
    test_get_returns_minus1_before_estimate();
    test_null_update_returns_error();
    test_null_get_returns_minus1();
    test_no_update_before_first_rest();
    test_no_update_below_min_delta_soc();
    test_no_update_on_discharge_window();
    test_healthy_cell_soh_near_100();
    test_soh_update_time_is_set();
    test_two_consecutive_updates();

    printf("\n--- Results: %d passed, %d failed ---\n\n", s_pass, s_fail);
    return (s_fail > 0) ? 1 : 0;
}
