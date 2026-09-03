/**
 * @file    soc_coulomb.c
 * @brief   Coulomb Counting SoC estimator
 *
 * Formula:
 *   SoC(k) = SoC(k-1) + 100 × [ I(k) × Δt ] / [ 3600 × Q_nom ]
 *
 * Sign convention: positive current = charging
 *
 * The coulombic efficiency is not applied here: the BMS already scales the
 * cell current integral by η before passing it to this estimator.
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

Bms_Error_t SocCoulomb_Update(Bms_SocState_t *state,
                              float           current_a,
                              float           dt_s)
{
    if (state == NULL) return BMS_ERR_NOT_INITIALISED;
    if (!state->is_initialised) return BMS_ERR_NOT_INITIALISED;

    /* Coulomb Counting integration — current_a is already η-corrected upstream.
     * Deliberately still on the nominal capacity: unlike the EKF, this module is
     * a comparison baseline rather than a deployed estimator, and its job is to
     * show the drift of raw integration, capacity fade included. */
    float delta_soc = (current_a * dt_s) /
                      (3600.0f * BMS_CELL_CAPACITY_INI_AH) * 100.0f;

    state->soc_prev_pct = state->soc_pct;
    state->soc_pct     += delta_soc;

    /* Clamp to physical limits */
    if (state->soc_pct < BMS_SOC_MIN_PCT) state->soc_pct = BMS_SOC_MIN_PCT;
    if (state->soc_pct > BMS_SOC_MAX_PCT) state->soc_pct = BMS_SOC_MAX_PCT;

    return BMS_OK;
}
