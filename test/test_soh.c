/**
 * @file    test_soh.c
 * @brief   Unit tests for the State-of-Health (SoH) estimator
 *
 * Test time step: 10 s (coarser than production 0.1 s to keep runtimes short).
 * A 2-hour rest therefore completes in exactly 720 steps.
 *
 * Synthetic scenario used by several tests:
 *   Phase 1 — 720 steps at rest  (I = 0,   V = OCV(90 %))
 *   Phase 2 — 576 steps active   (I = -30 A, V = OCV(10 %))
 *   Phase 3 — 720 steps at rest  (I = 0,   V = OCV(10 %))
 *
 *   Q = 30 A × 5760 s / 3600 = 48 Ah
 *   ΔSoC = 80 %  →  Qmax = 48 / 0.8 = 60 Ah  →  SoH = 100 %
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
#define DISCHARGE_STEPS 576U     /* 576 × 10 s × 30 A / 3600 = 48 Ah           */
#define DISCHARGE_A     (-30.0f)

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

/* Build the standard healthy-cell scenario and return final t_s */
static float run_healthy_scenario(Soh_State_t *s)
{
    float v_90pct = SocOcv_GetOcv(90.0f);
    float v_10pct = SocOcv_GetOcv(10.0f);
    float t = 0.0f;

    run_rest  (s, v_90pct, REST_STEPS,      &t);
    run_active(s, DISCHARGE_A, v_10pct, DISCHARGE_STEPS, &t);
    run_rest  (s, v_10pct, REST_STEPS,      &t);
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
    /* ΔSoC ≈ 20 % (< 80 % threshold) → no SoH update expected */
    Soh_State_t s;
    Soh_Init(&s);
    float v_80pct = SocOcv_GetOcv(80.0f);
    float v_60pct = SocOcv_GetOcv(60.0f);
    float t = 0.0f;

    /* First rest at SoC = 80 % */
    run_rest(&s, v_80pct, REST_STEPS, &t);
    /* Active: 30 A discharge for 1440 s → ΔQ = 12 Ah → ΔSoC = 20 %*/
    run_active(&s, DISCHARGE_A, v_60pct, 144U, &t);
    /* Second rest at SoC = 60 % */
    run_rest(&s, v_60pct, REST_STEPS, &t);

    ASSERT_EQ(0U, s.soh_update_count);
}

void test_healthy_cell_soh_near_100(void)
{
    /* Full 80 % ΔSoC discharge on a nominal cell → SoH = 100 % */
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
    float expected_t = (float)(REST_STEPS + DISCHARGE_STEPS + REST_STEPS) * DT_TEST;
    ASSERT_FLOAT_NEAR(expected_t, s.soh_update_time_s, DT_TEST);
}

void test_two_consecutive_updates(void)
{
    /* Two complete charge/discharge windows → two SoH updates */
    Soh_State_t s;
    Soh_Init(&s);
    float t = 0.0f;
    float v_90pct = SocOcv_GetOcv(90.0f);
    float v_10pct = SocOcv_GetOcv(10.0f);

    /* Window 1 */
    run_rest  (&s, v_90pct, REST_STEPS, &t);
    run_active(&s, DISCHARGE_A, v_10pct, DISCHARGE_STEPS, &t);
    run_rest  (&s, v_10pct, REST_STEPS, &t);

    /* Window 2 — charge back up */
    run_active(&s, 30.0f, v_90pct, DISCHARGE_STEPS, &t);
    run_rest  (&s, v_90pct, REST_STEPS, &t);

    ASSERT_EQ(2U, s.soh_update_count);
}

/* ============================================================ */
int main(void)
{
    printf("\n=== SoH Estimator Unit Tests ===\n\n");

    test_init_sets_seeking_rest_phase();
    test_init_sets_no_valid_estimate();
    test_get_returns_minus1_before_estimate();
    test_null_update_returns_error();
    test_null_get_returns_minus1();
    test_no_update_before_first_rest();
    test_no_update_below_min_delta_soc();
    test_healthy_cell_soh_near_100();
    test_soh_update_time_is_set();
    test_two_consecutive_updates();

    printf("\n--- Results: %d passed, %d failed ---\n\n", s_pass, s_fail);
    return (s_fail > 0) ? 1 : 0;
}
