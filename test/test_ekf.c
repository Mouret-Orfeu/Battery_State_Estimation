/**
 * @file    test_ekf.c
 * @brief   Unit tests for Extended Kalman Filter SoC estimator
 *
 * @author  Kamal Kadakara
 */

#include <stdio.h>
#include <math.h>
#include "bms_types.h"
#include "soc_ekf.h"
#include "soc_ocv.h"

static int s_pass = 0, s_fail = 0;

#define FLOAT_TOL       0.01f   /* 0.01% SoC — tight equality check  */
#define STEADY_TOL      0.5f    /* 0.5%  SoC — no-drift check        */
#define CONV_TOL_PCT    3.0f    /* 3%    SoC — convergence tolerance  */
#define CONV_STEPS      1000    /* Steps to reach convergence (100 s at 10 Hz) */

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

#define ASSERT_TRUE(cond) \
    do { if (cond) { s_pass++; printf("  [PASS] %s\n", __func__); } \
         else { s_fail++; printf("  [FAIL] %s — condition false (line %d)\n", \
                __func__, __LINE__); } \
    } while(0)

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
    Bms_Error_t ret = SocEkf_Update(NULL, &state, 0.0f, 3660.0f, BMS_SAMPLE_TIME_S);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, ret);
}

void test_null_state_returns_error(void)
{
    Bms_EkfState_t ekf = {0};
    Bms_Error_t ret = SocEkf_Update(&ekf, NULL, 0.0f, 3660.0f, BMS_SAMPLE_TIME_S);
    ASSERT_EQ(BMS_ERR_NOT_INITIALISED, ret);
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
    /* Large sustained discharge must not push SoC below 0% */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 1.0f);
    float v_meas_mv = SocOcv_GetOcv(0.0f);
    for (int i = 0; i < 200; i++) {
        SocEkf_Update(&ekf, &state, -200.0f, v_meas_mv, BMS_SAMPLE_TIME_S);
    }
    ASSERT_FLOAT_NEAR(0.0f, state.soc_pct, FLOAT_TOL);
}

void test_soc_clamps_at_100(void)
{
    /* Large sustained charge must not push SoC above 100% */
    Bms_EkfState_t  ekf   = {0};
    Bms_SocState_t  state = {0};
    SocEkf_Init(&ekf, &state, &s_default_ecm, 99.0f);
    float v_meas_mv = SocOcv_GetOcv(100.0f);
    for (int i = 0; i < 200; i++) {
        SocEkf_Update(&ekf, &state, 200.0f, v_meas_mv, BMS_SAMPLE_TIME_S);
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
