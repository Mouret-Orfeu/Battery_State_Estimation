/**
 * @file    soc_ekf.c
 * @brief   Extended Kalman Filter SoC Estimator
 *          State: x = [SoC, V_RC]  (1st order ECM / Randles model)
 *
 * State-space (discrete):
 *   SoC(k)  = SoC(k-1)  + [I(k) × Δt] / [3600 × Q_cur]       (positive I = charge)
 *   V_RC(k) = exp(−Δt/(R1×C1)) × V_RC(k-1) + R1×(1−exp(−Δt/(R1×C1))) × I(k)
 *
 * Measurement equation (terminal voltage):
 *   y(k) = OCV(SoC(k)) + V_RC(k) + R0 × I(k)
 *
 * Q_cur is the capacity the cell holds *now*, not the one it held when new:
 * SocEkf_SetCapacityAh() takes the measured Qmax published by the SoH estimator
 * so the prediction step follows capacity fade instead of assuming it away.
 * The nominal capacity remains the SoH reference and the startup fallback only.
 *
 * The coulombic efficiency is not applied here: the BMS already scales the
 * cell current integral by η before passing it to this estimator.
 *
 * @author  Kamal Kadakara
 */

#include "soc_ekf.h"
#include "soc_ocv.h"
#include <math.h>
#include <string.h>

/* ---- Default Noise Parameters (tune for target cell/sensors) ----
 * The Q terms are noise *densities* expressed per second, not per update step.
 * The prediction step multiplies them by dt, because the drift they represent
 * (current sensor bias, capacity drift, ECM mismatch) accumulates with elapsed
 * time rather than with the number of calls to SocEkf_Update().  The tuning
 * therefore stays valid whatever the update period is.
 */
#define EKF_Q11    1e-5f    /* Process noise density — SoC  [1/s]  */
#define EKF_Q22    1e-3f    /* Process noise density — V_RC [1/s]  */
#define EKF_R      1e-2f    /* Measurement noise — voltage [V²]    */

static Bms_EcmParams_t s_ecm;

void SocEkf_Init(Bms_EkfState_t    *ekf,
                 Bms_SocState_t    *state,
                 const Bms_EcmParams_t *ecm_params,
                 float              initial_soc_pct)
{
    if (!ekf || !state || !ecm_params) return;

    s_ecm = *ecm_params;

    /* Initial state */
    ekf->x[0] = initial_soc_pct / 100.0f;  /* Normalised SoC [0–1] */
    ekf->x[1] = 0.0f;                       /* V_RC = 0 (assumed rest) */

    /* Initial covariance — high uncertainty if SoC not well known */
    /* cross covariances are initially assumed to be zero */
    ekf->P[0][0] = 0.01f;  ekf->P[0][1] = 0.0f;
    ekf->P[1][0] = 0.0f;   ekf->P[1][1] = 0.01f;

    /* Process noise */
    /* noise cross covariance is assumed zero (noise sources are assumed independent) */
    ekf->Q[0][0] = EKF_Q11; ekf->Q[0][1] = 0.0f;
    ekf->Q[1][0] = 0.0f;    ekf->Q[1][1] = EKF_Q22;

    /* Measurement noise */
    ekf->R = EKF_R;

    /* No SoH measurement exists at startup, so the filter begins on the nominal
     * capacity and switches to the measured one on the first
     * SocEkf_SetCapacityAh() call the BMS makes after a SoH update */
    ekf->capacity_ah = BMS_CELL_CAPACITY_INI_AH;

    state->soc_pct        = initial_soc_pct;
    state->is_initialised = true;
}

Bms_Error_t SocEkf_SetCapacityAh(Bms_EkfState_t *ekf, float capacity_ah)
{
    if (!ekf) return BMS_ERR_NOT_INITIALISED;

    float capacity_ratio = capacity_ah / BMS_CELL_CAPACITY_INI_AH;

    /* Tested for being inside the band rather than outside it, so a non-finite
     * ratio — every comparison against a NaN being false — is rejected too */
    bool is_plausible = (capacity_ratio >= BMS_CAPACITY_RATIO_MIN)
                     && (capacity_ratio <= BMS_CAPACITY_RATIO_MAX);

    if (!is_plausible) {
        /* Previous capacity left in place: running on a slightly stale capacity
         * costs far less accuracy than integrating against a wrong one */
        return BMS_ERR_CAPACITY_IMPLAUSIBLE;
    }

    ekf->capacity_ah = capacity_ah;

    /* Applied, but worth reporting: the cell is past its service life */
    if (capacity_ratio < BMS_CAPACITY_RATIO_EOL) return BMS_ERR_CAPACITY_EOL;

    return BMS_OK;
}

Bms_Error_t SocEkf_Update(Bms_EkfState_t *ekf,
                           Bms_SocState_t *state,
                           float           current_a,
                           float           v_meas_mv,
                           float           dt_s)
{
    if (!ekf || !state) return BMS_ERR_NOT_INITIALISED;

    float I  = current_a;
    float dt = dt_s;

    /* Capacity the charge is counted against: the latest plausible measurement
     * from the SoH estimator, or the nominal one while none exists yet.  Using
     * the nominal value on an aged cell would make every Ah integrated count
     * for too little SoC, so the estimate would drift low over the cell's life. */
    float Q_current = ekf->capacity_ah;

    /* A state that never went through SocEkf_Init() holds a zero capacity, which
     * would turn B1 into a division by zero and poison every later step */
    if (!(Q_current > 0.0f)) Q_current = BMS_CELL_CAPACITY_INI_AH;

    /* ECM time constant */
    float tau   = s_ecm.R1 * s_ecm.C1;
    float A11   = 1.0f;
    float A22   = expf(-dt / tau);
    float B1    = dt / (3600.0f * Q_current);   /* current is already η-corrected */
    float B2    = s_ecm.R1 * (1.0f - A22);

    /* ---- 1. Prediction Step ---- */
    float x_pred[2];
    x_pred[0] = A11 * ekf->x[0] + B1 * I;
    x_pred[1] = A22 * ekf->x[1] + B2 * I;

    /* Clamp SoC prediction to [0, 1] */
    if (x_pred[0] < 0.0f) x_pred[0] = 0.0f;
    if (x_pred[0] > 1.0f) x_pred[0] = 1.0f;

    /* Predicted covariance: P_pred = A·P·Aᵀ + Q·dt  (diagonal A) */
    /* Q is scaled by dt so that a given amount of model drift is injected per
     * second of elapsed time rather than per call: halving the update period
     * then no longer halves the process noise the filter accumulates. */
    float P_pred[2][2];
    P_pred[0][0] = A11*A11 * ekf->P[0][0] + ekf->Q[0][0] * dt;
    P_pred[0][1] = A11*A22 * ekf->P[0][1];
    P_pred[1][0] = A22*A11 * ekf->P[1][0];
    P_pred[1][1] = A22*A22 * ekf->P[1][1] + ekf->Q[1][1] * dt;

    /* ---- 2. Measurement Update ---- */
    /* Predicted terminal voltage [mV → V conversion for noise tuning] */
    float ocv_pred_mv = SocOcv_GetOcv(x_pred[0] * 100.0f);
    float y_pred_mv   = ocv_pred_mv + x_pred[1] * 1000.0f + s_ecm.R0 * I * 1000.0f;

    /* Linearised output Jacobian H = [dOCV/dSoC, 1] (numeric dOCV/dSoC) */
    /* Jacobian explanation:
    H = [∂V/∂SoC, ∂V/∂V_RC] = [dOCV/dSoC, 1] as ∂V/∂SoC = dOCV/dSoC and ∂V/∂V_RC = 1 */
    float dsoc       = 0.001f;
    float dOCV_dSoC  = (SocOcv_GetOcv((x_pred[0] + dsoc) * 100.0f) -
                        SocOcv_GetOcv((x_pred[0] - dsoc) * 100.0f)) / (2.0f * dsoc);

    float H0 = dOCV_dSoC;   /* Jacobian wrt x[0] (normalised [0,1]): dOCV_mV/d(x[0]) */
    float H1 = 1000.0f;     /* Jacobian wrt V_RC (converting to mV) */

    /* Innovation covariance S = H·P_pred·Hᵀ + R */
    float S = H0*H0 * P_pred[0][0]
            + H0*H1 * (P_pred[0][1] + P_pred[1][0])
            + H1*H1 * P_pred[1][1]
            + ekf->R * 1e6f;  /* R in mV² */

    /* Kalman gain K = P_pred·Hᵀ / S */
    float K0 = (H0 * P_pred[0][0] + H1 * P_pred[0][1]) / S;
    float K1 = (H0 * P_pred[1][0] + H1 * P_pred[1][1]) / S;

    /* State update */
    float innovation = v_meas_mv - y_pred_mv;
    ekf->x[0] = x_pred[0] + K0 * innovation;
    ekf->x[1] = x_pred[1] + K1 * innovation;

    /* Clamp updated SoC */
    if (ekf->x[0] < 0.0f) ekf->x[0] = 0.0f;
    if (ekf->x[0] > 1.0f) ekf->x[0] = 1.0f;

    /* Covariance update: P = (I - K·H)·P_pred */
    ekf->P[0][0] = (1.0f - K0*H0) * P_pred[0][0] - K0*H1 * P_pred[1][0];
    ekf->P[0][1] = (1.0f - K0*H0) * P_pred[0][1] - K0*H1 * P_pred[1][1];
    ekf->P[1][0] = -K1*H0 * P_pred[0][0] + (1.0f - K1*H1) * P_pred[1][0];
    ekf->P[1][1] = -K1*H0 * P_pred[0][1] + (1.0f - K1*H1) * P_pred[1][1];

    /* Write result to SoC state */
    state->soc_prev_pct = state->soc_pct;
    state->soc_pct      = ekf->x[0] * 100.0f;

    return BMS_OK;
}
