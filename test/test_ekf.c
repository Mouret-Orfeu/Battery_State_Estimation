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
 * even for flat regions of signals and high measurement noise (10 000s ~ 3h, at 10 Hz) */
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

/* ============================================================ */
int main(void)
{
    printf("\n=== Extended Kalman Filter SoC Unit Tests ===\n\n");

    test_init_sets_correct_soc();
    test_null_ekf_returns_error();
    test_null_state_returns_error();
    test_converges_from_overestimate();
    test_converges_from_underestimate();
    test_no_drift_at_correct_init();
    test_covariance_decreases_with_updates();
    test_soc_clamps_at_zero();
    test_soc_clamps_at_100();

    printf("\n--- Results: %d passed, %d failed ---\n\n", s_pass, s_fail);
    return (s_fail > 0) ? 1 : 0;
}
