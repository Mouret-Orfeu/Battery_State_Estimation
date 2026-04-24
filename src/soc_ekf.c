/**
 * @file    soc_ekf.c
 * @brief   Extended Kalman Filter SoC Estimator
 *          State: x = [SoC, V_RC]  (1st order ECM / Randles model)
 *
 * State-space (discrete, Δt = 0.1 s):
 *   SoC(k)  = SoC(k-1)  + [I(k) × Δt] / [3600 × Q_nom × η]   (positive I = charge)
 *   V_RC(k) = exp(−Δt/(R1×C1)) × V_RC(k-1) + R1×(1−exp(−Δt/(R1×C1))) × I(k)
 *
 * Measurement equation (terminal voltage):
 *   y(k) = OCV(SoC(k)) + V_RC(k) + R0 × I(k)
 *
 * @author  Kamal Kadakara
 */

#include "soc_ekf.h"
#include "soc_ocv.h"
#include <math.h>
#include <string.h>

/* ---- Default Noise Parameters (tune for target cell/sensors) ---- */
#define EKF_Q11    1e-6f    /* Process noise — SoC              */
#define EKF_Q22    1e-4f    /* Process noise — V_RC             */
#define EKF_R      1e-2f    /* Measurement noise — voltage [V²] */

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

    state->soc_pct        = initial_soc_pct;
    state->is_initialised = true;
}

/**
 * @brief  One EKF prediction + update step.
 *
 * @param  ekf        EKF state
 * @param  state      SoC output state (updated with new estimate)
 * @param  current_a  Pack current [A] (positive = charge)
 * @param  v_meas_mv  Measured terminal voltage [mV]
 * @param  dt_s       Time step [s]
 */
Bms_Error_t SocEkf_Update(Bms_EkfState_t *ekf,
                           Bms_SocState_t *state,
                           float           current_a,
                           float           v_meas_mv,
                           float           dt_s)
{
    if (!ekf || !state) return BMS_ERR_NOT_INITIALISED;

    float I  = current_a;
    float dt = dt_s;
    float Q_nom = BMS_CELL_CAPACITY_AH;
    float eta   = (I >= 0.0f) ? BMS_COULOMBIC_EFF_CHG : BMS_COULOMBIC_EFF_DCHG;

    /* ECM time constant */
    float tau   = s_ecm.R1 * s_ecm.C1;
    float A11   = 1.0f;
    float A22   = expf(-dt / tau);
    float B1    = (dt * eta) / (3600.0f * Q_nom);
    float B2    = s_ecm.R1 * (1.0f - A22);

    /* ---- 1. Prediction Step ---- */
    float x_pred[2];
    x_pred[0] = A11 * ekf->x[0] + B1 * I;
    x_pred[1] = A22 * ekf->x[1] + B2 * I;

    /* Clamp SoC prediction to [0, 1] */
    if (x_pred[0] < 0.0f) x_pred[0] = 0.0f;
    if (x_pred[0] > 1.0f) x_pred[0] = 1.0f;

    /* Predicted covariance: P_pred = A·P·Aᵀ + Q  (diagonal A) */
    float P_pred[2][2];
    P_pred[0][0] = A11*A11 * ekf->P[0][0] + ekf->Q[0][0];
    P_pred[0][1] = A11*A22 * ekf->P[0][1];
    P_pred[1][0] = A22*A11 * ekf->P[1][0];
    P_pred[1][1] = A22*A22 * ekf->P[1][1] + ekf->Q[1][1];

    /* ---- 2. Measurement Update ---- */
    /* Predicted terminal voltage [mV → V conversion for noise tuning] */
    float ocv_pred_mv = SocOcv_GetOcv(x_pred[0] * 100.0f);
    float y_pred_mv   = ocv_pred_mv + x_pred[1] * 1000.0f + s_ecm.R0 * I * 1000.0f;

    /* Linearised output Jacobian C = [dOCV/dSoC, 1] (numeric dOCV/dSoC) */
    /* Jacobian explanation: 
    C = [∂V/∂SoC, ∂V/∂V_RC] = [dOCV/dSoC, 1] as ∂V/∂SoC = dOCV/dSoC and ∂V/∂V_RC = 1 */
    float dsoc       = 0.001f;
    float dOCV_dSoC  = (SocOcv_GetOcv((x_pred[0] + dsoc) * 100.0f) -
                        SocOcv_GetOcv((x_pred[0] - dsoc) * 100.0f)) / (2.0f * dsoc);

    float C0 = dOCV_dSoC;   /* Jacobian wrt x[0] (normalised [0,1]): dOCV_mV/d(x[0]) */
    float C1_j = 1000.0f;   /* Jacobian wrt V_RC (converting to mV) */

    /* Innovation covariance S = C·P_pred·Cᵀ + R */
    float S = C0*C0 * P_pred[0][0]
            + C0*C1_j * (P_pred[0][1] + P_pred[1][0])
            + C1_j*C1_j * P_pred[1][1]
            + ekf->R * 1e6f;  /* R in mV² */

    /* Kalman gain K = P_pred·Cᵀ / S */
    float K0 = (C0 * P_pred[0][0] + C1_j * P_pred[0][1]) / S;
    float K1 = (C0 * P_pred[1][0] + C1_j * P_pred[1][1]) / S;

    /* State update */
    float innovation = v_meas_mv - y_pred_mv;
    ekf->x[0] = x_pred[0] + K0 * innovation;
    ekf->x[1] = x_pred[1] + K1 * innovation;

    /* Clamp updated SoC */
    if (ekf->x[0] < 0.0f) ekf->x[0] = 0.0f;
    if (ekf->x[0] > 1.0f) ekf->x[0] = 1.0f;

    /* Covariance update: P = (I - K·C)·P_pred */
    ekf->P[0][0] = (1.0f - K0*C0) * P_pred[0][0] - K0*C1_j * P_pred[1][0];
    ekf->P[0][1] = (1.0f - K0*C0) * P_pred[0][1] - K0*C1_j * P_pred[1][1];
    ekf->P[1][0] = -K1*C0 * P_pred[0][0] + (1.0f - K1*C1_j) * P_pred[1][0];
    ekf->P[1][1] = -K1*C0 * P_pred[0][1] + (1.0f - K1*C1_j) * P_pred[1][1];

    /* Write result to SoC state */
    state->soc_prev_pct = state->soc_pct;
    state->soc_pct      = ekf->x[0] * 100.0f;

    return BMS_OK;
}
