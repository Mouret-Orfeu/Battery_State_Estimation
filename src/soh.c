/**
 * @file    soh.c
 * @brief   State-of-Health (SoH) Estimator — capacity-fade method
 *
 * Phase state machine:
 *
 *   SEEKING_REST ──► AT_REST ◄──────────────────────────────┐
 *                       │                                    │
 *                       │ |I| ≥ threshold                    │ rest confirmed
 *                       ▼                                    │
 *                    ACTIVE ─────────────────────────────────┘
 *
 *   SEEKING_REST : waiting for the first confirmed rest; no integral yet.
 *   AT_REST      : resting; no charge integration.
 *   ACTIVE       : integrating charge (η × I × dt).  When the next rest is
 *                  confirmed, Qmax and SoH are computed if ΔSoC ≥ minimum.
 *
 * Rest confirmation fires on the exact step where rest_timer first reaches
 * SOH_MIN_REST_DURATION_S (edge detection, not level detection).
 *
 * @author  Orfeu Mouret
 */

#include "soh.h"
#include "soc_ocv.h"
#include <math.h>
#include <stddef.h>

/**
 * @brief  Clamp a float value to [min_bound, max_bound].
 *
 * @param  value      Value to clamp
 * @param  min_bound  Lower bound (inclusive)
 * @param  max_bound  Upper bound (inclusive)
 * @return Clamped value
 */
static float _clampf(float value, float min_bound, float max_bound)
{
    if (value < min_bound) return min_bound;
    if (value > max_bound) return max_bound;
    return value;
}

void Soh_Init(Soh_State_t *soh_state)
{
    if (soh_state == NULL) return;

    soh_state->phase                  = SOH_PHASE_SEEKING_REST;
    soh_state->rest_timer_s           = 0.0f;
    soh_state->soc_at_rest_entry_pct  = 0.0f;
    soh_state->charge_integral_ah     = 0.0f;
    soh_state->qmax_ah                = 0.0f;
    soh_state->soh_pct                = 0.0f;
    soh_state->soh_update_time_s      = 0.0f;
    soh_state->soh_update_count       = 0U;
}

Bms_Error_t Soh_Update(Soh_State_t *soh_state,
                        float        current_a,
                        float        v_meas_mv,
                        float        t_s,
                        float        dt_s)
{
    if (soh_state == NULL) return BMS_ERR_NOT_INITIALISED;

    bool at_rest = (fabsf(current_a) < SOH_REST_CURRENT_THRESHOLD_A);

    /* Maintain consecutive rest timer */
    if (at_rest) {
        soh_state->rest_timer_s += dt_s;
    } else {
        soh_state->rest_timer_s = 0.0f;
    }

    /* Edge: true only on the first step where rest duration is confirmed */
    bool rest_just_confirmed = at_rest
        && (soh_state->rest_timer_s          >= SOH_MIN_REST_DURATION_S)
        && ((soh_state->rest_timer_s - dt_s)  < SOH_MIN_REST_DURATION_S);

    switch (soh_state->phase) {

        /* ---- Waiting for first rest ---- */
        case SOH_PHASE_SEEKING_REST:
            if (rest_just_confirmed) {
                SocOcv_LookupSoc(v_meas_mv, &soh_state->soc_at_rest_entry_pct);
                soh_state->phase = SOH_PHASE_AT_REST;
            }
            break;

        /* ---- Inside a confirmed rest ---- */
        case SOH_PHASE_AT_REST:
            if (!at_rest) {
                soh_state->charge_integral_ah = 0.0f;
                soh_state->phase = SOH_PHASE_ACTIVE;
            }
            break;

        /* ---- Active: integrating charge ---- */
        case SOH_PHASE_ACTIVE:
            if (rest_just_confirmed) {
                /* New rest confirmed — attempt Qmax / SoH update */
                float soc_end_pct = 0.0f;
                SocOcv_LookupSoc(v_meas_mv, &soc_end_pct);

                float delta_soc = fabsf(soc_end_pct - soh_state->soc_at_rest_entry_pct);

                if (delta_soc >= SOH_MIN_DELTA_SOC_PCT) {
                    float qmax_ah = fabsf(soh_state->charge_integral_ah)
                                  / (delta_soc / 100.0f);
                    float soh = (qmax_ah / SOH_NOM_CAPACITY_AH) * 100.0f;

                    soh_state->qmax_ah           = qmax_ah;
                    soh_state->soh_pct           = _clampf(soh, 0.0f, 100.0f);
                    soh_state->soh_update_time_s = t_s;
                    soh_state->soh_update_count++;
                }

                /* Record new rest SoC regardless of whether update was valid */
                soh_state->soc_at_rest_entry_pct = soc_end_pct;
                soh_state->phase = SOH_PHASE_AT_REST;

            } else if (!at_rest) {
                /* Accumulate charge with coulombic efficiency */
                float eta = (current_a >= 0.0f) ? BMS_COULOMBIC_EFF_CHG
                                                 : BMS_COULOMBIC_EFF_DCHG;
                soh_state->charge_integral_ah += current_a * dt_s * eta / 3600.0f;
            }
            break;

        default:
            break;
    }

    return BMS_OK;
}

float Soh_Get(const Soh_State_t *soh_state)
{
    if (soh_state == NULL || soh_state->soh_update_count == 0U) return -1.0f;
    return soh_state->soh_pct;
}

uint32_t Soh_ComputeFromTimeSeries(
    const float *current_a,
    const float *voltage_mv,
    uint32_t     n_samples,
    float        dt_s,
    float       *out_times_s,
    float       *out_soh_pct,
    uint32_t     max_updates)
{
    if (!current_a || !voltage_mv || !out_times_s || !out_soh_pct
            || n_samples == 0U || max_updates == 0U) {
        return 0U;
    }

    Soh_State_t soh_state;
    Soh_Init(&soh_state);

    uint32_t n_updates  = 0U;
    uint32_t prev_count = 0U;

    for (uint32_t it = 0U; it < n_samples && n_updates < max_updates; it++) {
        float t_s = (float)it * dt_s;
        Soh_Update(&soh_state, current_a[it], voltage_mv[it], t_s, dt_s);

        if (soh_state.soh_update_count != prev_count) {
            out_times_s[n_updates] = soh_state.soh_update_time_s;
            out_soh_pct[n_updates] = soh_state.soh_pct;
            prev_count             = soh_state.soh_update_count;
            n_updates++;
        }
    }

    return n_updates;
}
