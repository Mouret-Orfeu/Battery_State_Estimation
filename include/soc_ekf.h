/**
 * @file    soc_ekf.h
 * @brief   Extended Kalman Filter SoC Estimator API
 *
 * @author  Kamal Kadakara
 */
#ifndef SOC_EKF_H
#define SOC_EKF_H

#include "bms_types.h"

/**
 * @brief  Initialise EKF state, covariance matrices, and noise parameters.
 *         Sets the initial SoC estimate and marks the state as initialised.
 *         V_RC is assumed zero (cell at rest at startup).
 *
 * @param  ekf              EKF internal state
 * @param  state            SoC output state (soc_pct is set to initial_soc_pct)
 * @param  ecm_params       ECM parameters (R0, R1, C1) for this cell
 * @param  initial_soc_pct  Starting SoC estimate [0.0–100.0 %]
 */
void        SocEkf_Init(Bms_EkfState_t *ekf,
                        Bms_SocState_t *state,
                        const Bms_EcmParams_t *ecm_params,
                        float initial_soc_pct);

/**
 * @brief  Run one EKF prediction + measurement update step.
 *         Updates ekf->x, ekf->P, and writes the new SoC estimate to state->soc_pct.
 *
 * @param  ekf        EKF internal state
 * @param  state      SoC output state (updated with new estimate)
 * @param  current_a  Measured pack current [A] (positive = charge)
 * @param  v_meas_mv  Measured terminal voltage [mV]
 * @param  dt_s       Time step [s]
 * @return BMS_OK on success; BMS_ERR_NOT_INITIALISED if ekf or state is NULL
 */
Bms_Error_t SocEkf_Update(Bms_EkfState_t *ekf,
                           Bms_SocState_t *state,
                           float current_a,
                           float v_meas_mv,
                           float dt_s);

#endif /* SOC_EKF_H */
