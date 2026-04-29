/**
 * @file    soc_coulomb.h
 * @brief   Coulomb Counting SoC Estimator API
 *
 * @author  Kamal Kadakara
 */
#ifndef SOC_COULOMB_H
#define SOC_COULOMB_H

#include "bms_types.h"

/**
 * @brief  Initialise Coulomb Counting state.
 *         Sets soc_pct and soc_prev_pct to initial_soc_pct and marks the
 *         state as initialised.
 *
 * @param  state            Pointer to SoC state structure
 * @param  initial_soc_pct  Starting SoC [0.0–100.0 %]
 */
void      SocCoulomb_Init(Bms_SocState_t *state, float initial_soc_pct);

/**
 * @brief  Update SoC via Coulomb Counting.
 *         Integrates current over dt_s with coulombic efficiency and clamps
 *         the result to [BMS_SOC_MIN_PCT, BMS_SOC_MAX_PCT].
 *
 * @param  state      Pointer to SoC state structure
 * @param  current_a  Measured pack current [A] (positive = charge)
 * @param  dt_s       Elapsed time since last call [s]
 * @return BMS_OK on success; BMS_ERR_NOT_INITIALISED if state is NULL or uninitialised
 */
Bms_Error_t SocCoulomb_Update(Bms_SocState_t *state,
                              float current_a,
                              float dt_s);

#endif /* SOC_COULOMB_H */
