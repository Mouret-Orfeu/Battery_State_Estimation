/**
 * @file    soc_coulomb.c
 * @brief   Coulomb Counting SoC estimator
 *
 * Formula:
 *   SoC(k) = SoC(k-1) - [ I(k) × Δt ] / [ 3600 × Q_nom × η ]
 *
 * Sign convention: positive current = charging
 *
 * @author  Kamal Kadakara
 */

#include "soc_coulomb.h"
#include <stddef.h>

void SocCoulomb_Init(Bms_SocState_t *state, float initial_soc_pct)
{
    if (state == NULL) return;

    state->soc_pct        = initial_soc_pct;
    state->soc_prev_pct   = initial_soc_pct;
    state->is_initialised = true;
}

/**
 * @brief  Update SoC via Coulomb Counting.
 *
 * @param  state      Pointer to SoC state structure
 * @param  current_a  Measured pack current [A] (positive = charge)
 * @param  dt_s       Elapsed time since last call [seconds]
 */
Bms_Error_t SocCoulomb_Update(Bms_SocState_t *state,
                              float           current_a,
                              float           dt_s)
{
    if (state == NULL) return BMS_ERR_NOT_INITIALISED;
    if (!state->is_initialised) return BMS_ERR_NOT_INITIALISED;

    /* Select efficiency factor based on current direction */
    float eta = (current_a >= 0.0f) ? BMS_COULOMBIC_EFF_CHG
                                    : BMS_COULOMBIC_EFF_DCHG;

    /* Coulomb Counting integration */
    float delta_soc = (current_a * dt_s * eta) /
                      (3600.0f * BMS_CELL_CAPACITY_AH) * 100.0f;

    state->soc_prev_pct = state->soc_pct;
    state->soc_pct     += delta_soc;

    /* Clamp to physical limits */
    if (state->soc_pct < BMS_SOC_MIN_PCT) state->soc_pct = BMS_SOC_MIN_PCT;
    if (state->soc_pct > BMS_SOC_MAX_PCT) state->soc_pct = BMS_SOC_MAX_PCT;

    return BMS_OK;
}
