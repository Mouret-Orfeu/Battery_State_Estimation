/**
 * @file    soc_ekf.h
 * @brief   Extended Kalman Filter SoC Estimator API
 *
 * @author  Kamal Kadakara
 */
#ifndef SOC_EKF_H
#define SOC_EKF_H

#include "bms_types.h"

void        SocEkf_Init(Bms_EkfState_t *ekf,
                        Bms_SocState_t *state,
                        const Bms_EcmParams_t *ecm_params,
                        float initial_soc_pct);

Bms_Error_t SocEkf_Update(Bms_EkfState_t *ekf,
                           Bms_SocState_t *state,
                           float current_a,
                           float v_meas_mv,
                           float dt_s);

#endif /* SOC_EKF_H */
