/**
 * @file    soh.c
 * @brief   State-of-Health (SoH) Estimator — capacity-fade method
 *
 * Phase state machine:
 *
 *   SEEKING_REST ──► AT_REST ◄────────────────────────────── ┐
 *                       │                                    │
 *                       │ |I| ≥ SOH_REST_CURRENT_THRESHOLD_A │ rest confirmed
 *                       ▼                                    │
 *                    ACTIVE ─────────────────────────────────┘
 *
 *   SEEKING_REST : waiting for the first confirmed rest; no integral yet.
 *   AT_REST      : resting; no charge integration.
 *   ACTIVE       : integrating charge (I × dt).  When the next rest is confirmed,
 *                  Qmax and SoH are computed if the window was a charge spanning
 *                  ΔSoC ≥ SOH_MIN_DELTA_SOC_PCT.  Discharge windows are rejected,
 *                  but still reposition the rest SoC for the next window.
 *
 * The coulombic efficiency is not applied here: the BMS already scales the
 * cell current integral by η before passing it to this estimator.
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
                /* New rest confirmed, attempt Qmax / SoH update */
                float soc_end_pct = 0.0f;
                SocOcv_LookupSoc(v_meas_mv, &soc_end_pct);

                /* Signed on purpose: a charge window yields ΔSoC > 0, a discharge
                 * window ΔSoC < 0.  Only charge windows are accepted, charging being
                 * the controlled and reproducible direction (CC-CV), whereas a
                 * discharge window reflects whatever load the application drew. */
                float delta_soc = soc_end_pct - soh_state->soc_at_rest_entry_pct;

                /* The positive threshold therefore rejects discharge windows outright,
                 * and keeps only charges wide enough to make Qmax accurate */
                if (delta_soc >= SOH_MIN_DELTA_SOC_PCT) {
                    float qmax_ah = soh_state->charge_integral_ah
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
                /* Accumulate charge — current_a is already η-corrected upstream */
                soh_state->charge_integral_ah += current_a * dt_s / 3600.0f;
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


/**
 * @brief  Test function: Compute SoH estimates from a full time series.
 *
 * @param  current_a       Array of current samples [A] (positive = charge)
 * @param  voltage_mv      Array of voltage samples [mV]
 * @param  n_samples       Number of samples in input arrays
 * @param  dt_s            Time step duration [s]
 * @param  out_times_s     Output array for timestamps of SoH updates [s]
 * @param  out_soh_pct     Output array for SoH estimates [%]
 * @param  max_updates     Maximum number of SoH updates to produce (size of output arrays)
 * @return Number of SoH updates produced, or 0 on invalid input
 */
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
