/**
 * @file    test_ekf.c
 * @brief   Unit tests for Extended Kalman Filter SoC estimator
 *
 * @author  Orfeu Mouret
 */

#include <stdio.h>
#include <math.h>
#include "bms_types.h"
#include "soc_ekf.h"
#include "soc_ocv.h"
#include "test_helpers.h"

static int s_pass = 0, s_fail = 0;

#define FLOAT_TOL       0.01f   /* 0.01% SoC — tight equality check  */
#define STEADY_TOL      0.5f    /* 0.5%  SoC — no-drift check        */
#define CONV_TOL_PCT    3.0f    /* 3%    SoC — convergence tolerance  */

/* A good amount of steps to be sure to reach convergence
 * even for flat regions of signals and high measurement noise
 * (10 000 steps × BMS_SAMPLE_TIME_S = 4 000 s ~ 1.1 h, at 2.5 Hz) */
#define CONV_STEPS      10000

static const Bms_EcmParams_t s_default_ecm = BMS_ECM_DEFAULT;

/* Run n_steps EKF updates at rest (I=0, V_RC=0) with a fixed true SoC.
 * The measured voltage equals OCV(true_soc_pct) exactly — no noise. */
static void run_rest_steps(Bms_EkfState_t *ekf,
                           Bms_SocState_t *state,
                           float           true_soc_pct,
                           int             n_steps)
{
    float v_meas_mv = SocOcv_GetOcv(true_soc_pct);
    for (int i = 0; i < n_steps; i++) {
        SocEkf_Update(ekf, state, 0.0f, v_meas_mv, BMS_SAMPLE_TIME_S);
    }
}

/* ---- Tests ---- */

void test_init_sets_correct_soc(void)
{
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 75.0f);
    ASSERT_FLOAT_NEAR(75.0f, state.soc_pct, FLOAT_TOL);
}

void test_null_ekf_returns_error(void)
{
    Bms_SocState_t state = {0};
    Bms_Error_t err = SocEkf_Update(NULL, &state, 0.0f, 3660.0f, BMS_SAMPLE_TIME_S);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, err);
}

void test_null_state_returns_error(void)
{
    Bms_EkfState_t ekf = {0};
    Bms_Error_t err = SocEkf_Update(&ekf, NULL, 0.0f, 3660.0f, BMS_SAMPLE_TIME_S);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, err);
}

void test_converges_from_overestimate(void)
{
    /* EKF starts 30% above true SoC — measurements must pull it down */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 80.0f);
    run_rest_steps(&ekf, &state, 50.0f, CONV_STEPS);
    ASSERT_FLOAT_NEAR(50.0f, state.soc_pct, CONV_TOL_PCT);
}

void test_converges_from_underestimate(void)
{
    /* EKF starts 30% below true SoC — measurements must pull it up */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 20.0f);
    run_rest_steps(&ekf, &state, 50.0f, CONV_STEPS);
    ASSERT_FLOAT_NEAR(50.0f, state.soc_pct, CONV_TOL_PCT);
}

void test_no_drift_at_correct_init(void)
{
    /* With correct initialisation, consistent measurements must not drift the estimate */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 50.0f);
    run_rest_steps(&ekf, &state, 50.0f, 100);
    ASSERT_FLOAT_NEAR(50.0f, state.soc_pct, STEADY_TOL);
}

void test_covariance_decreases_with_updates(void)
{
    /* Filter confidence on SoC must increase (P[0][0] shrinks) as measurements accumulate */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 50.0f);
    float p_init = ekf.P[0][0];
    run_rest_steps(&ekf, &state, 50.0f, 100);
    ASSERT_TRUE(ekf.P[0][0] < p_init);
}

void test_soc_clamps_at_zero(void)
{
    /* Voltage below OCV(0%) drives EKF to push SoC below 0% — clamp must hold at 0% */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 5.0f);
    float v_meas_mv = SocOcv_GetOcv(0.0f) - 50.0f;
    for (int i = 0; i < 200; i++) {
        SocEkf_Update(&ekf, &state, 0.0f, v_meas_mv, BMS_SAMPLE_TIME_S);
    }
    ASSERT_FLOAT_NEAR(0.0f, state.soc_pct, FLOAT_TOL);
}

void test_soc_clamps_at_100(void)
{
    /* Voltage above OCV(100%) drives EKF to push SoC above 100% — clamp must hold at 100% */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 95.0f);
    float v_meas_mv = SocOcv_GetOcv(100.0f) + 50.0f;
    for (int i = 0; i < 200; i++) {
        SocEkf_Update(&ekf, &state, 0.0f, v_meas_mv, BMS_SAMPLE_TIME_S);
    }
    ASSERT_FLOAT_NEAR(100.0f, state.soc_pct, FLOAT_TOL);
}

void test_init_starts_on_nominal_capacity(void)
{
    /* No SoH measurement exists at startup, so the filter must begin on nominal */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 50.0f);
    ASSERT_FLOAT_NEAR(BMS_CELL_CAPACITY_INI_AH, ekf.capacity_ah, FLOAT_TOL);
}

void test_set_capacity_accepts_aged_cell(void)
{
    /* A plausibly aged capacity is what the coupling exists to apply */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 50.0f);

    float aged_capacity_ah = 0.8f * BMS_CELL_CAPACITY_INI_AH;
    Bms_Error_t err = SocEkf_SetCapacityAh(&ekf, aged_capacity_ah);

    ASSERT_EQ(BMS_OK, err);
    ASSERT_FLOAT_NEAR(aged_capacity_ah, ekf.capacity_ah, FLOAT_TOL);
}

void test_set_capacity_applies_end_of_life_value(void)
{
    /* Past end of life the caller is warned, but the value is still applied:
     * a worn cell is exactly where the nominal capacity misestimates SoC most */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 50.0f);

    float worn_capacity_ah = 0.5f * BMS_CELL_CAPACITY_INI_AH;
    Bms_Error_t err = SocEkf_SetCapacityAh(&ekf, worn_capacity_ah);

    ASSERT_EQ(BMS_ERR_CAPACITY_EOL, err);
    ASSERT_FLOAT_NEAR(worn_capacity_ah, ekf.capacity_ah, FLOAT_TOL);
}

void test_set_capacity_rejects_above_band(void)
{
    /* The EKF re-checks its input rather than trusting the capacity source */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 50.0f);

    Bms_Error_t err = SocEkf_SetCapacityAh(&ekf, 2.0f * BMS_CELL_CAPACITY_INI_AH);

    ASSERT_EQ(BMS_ERR_CAPACITY_IMPLAUSIBLE, err);
    /* Rejected values must leave the previous capacity in place */
    ASSERT_FLOAT_NEAR(BMS_CELL_CAPACITY_INI_AH, ekf.capacity_ah, FLOAT_TOL);
}

void test_set_capacity_rejects_below_band(void)
{
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 50.0f);

    Bms_Error_t err = SocEkf_SetCapacityAh(&ekf, 0.05f * BMS_CELL_CAPACITY_INI_AH);

    ASSERT_EQ(BMS_ERR_CAPACITY_IMPLAUSIBLE, err);
    ASSERT_FLOAT_NEAR(BMS_CELL_CAPACITY_INI_AH, ekf.capacity_ah, FLOAT_TOL);
}

void test_set_capacity_null_returns_error(void)
{
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, SocEkf_SetCapacityAh(NULL, 3.0f));
}

void test_reduced_capacity_moves_soc_faster(void)
{
    /* The behavioural consequence of the coupling: the same charge poured into
     * a smaller capacity has to raise SoC further.  Both filters see identical
     * inputs, so only the capacity can separate them. */
    Bms_EkfState_t  ekf_nominal = {0}, ekf_aged = {0};
    Bms_SocState_t  state_nominal = {0}, state_aged = {0};
    SocEkf_Init(&ekf_nominal, &state_nominal, &s_default_ecm, 50.0f);
    SocEkf_Init(&ekf_aged,    &state_aged,    &s_default_ecm, 50.0f);
    SocEkf_SetCapacityAh(&ekf_aged, 0.7f * BMS_CELL_CAPACITY_INI_AH);

    /* 1C charge, held at the OCV of the starting SoC so both filters get the
     * same measurement correction and the capacity is the only difference */
    float v_meas_mv = SocOcv_GetOcv(50.0f);
    for (int i = 0; i < 100; i++) {
        SocEkf_Update(&ekf_nominal, &state_nominal, BMS_CELL_CAPACITY_INI_AH,
                      v_meas_mv, BMS_SAMPLE_TIME_S);
        SocEkf_Update(&ekf_aged,    &state_aged,    BMS_CELL_CAPACITY_INI_AH,
                      v_meas_mv, BMS_SAMPLE_TIME_S);
    }

    ASSERT_TRUE(state_aged.soc_pct > state_nominal.soc_pct);
}

/* ============================================================ */
int main(void)
{
    printf("\n=== Extended Kalman Filter SoC Unit Tests ===\n\n");

    /* Prerequisite: the EKF reads OCV(SoC) for its measurement model and Jacobian,
     * and the helpers below build v_meas_mv from the same table */
    LOAD_OCV_TABLE_OR_FAIL();

    test_init_sets_correct_soc();
    test_null_ekf_returns_error();
    test_null_state_returns_error();
    test_converges_from_overestimate();
    test_converges_from_underestimate();
    test_no_drift_at_correct_init();
    test_covariance_decreases_with_updates();
    test_soc_clamps_at_zero();
    test_soc_clamps_at_100();
    test_init_starts_on_nominal_capacity();
    test_set_capacity_accepts_aged_cell();
    test_set_capacity_applies_end_of_life_value();
    test_set_capacity_rejects_above_band();
    test_set_capacity_rejects_below_band();
    test_set_capacity_null_returns_error();
    test_reduced_capacity_moves_soc_faster();

    printf("\n--- Results: %d passed, %d failed ---\n\n", s_pass, s_fail);
    return (s_fail > 0) ? 1 : 0;
}
