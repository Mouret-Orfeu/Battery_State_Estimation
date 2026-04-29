/**
 * @file    soh.h
 * @brief   State-of-Health (SoH) Estimator API — capacity-fade method
 *
 *          SoH is estimated from the ratio of the current maximum capacity
 *          (Qmax_current) to the initial nominal capacity (Qmax_nom):
 *
 *              SoH [%] = ( Qmax_current / Qmax_nom ) × 100
 *
 *          Qmax_current is derived from a charge/discharge window bounded
 *          by two confirmed rest periods:
 *
 *              Qmax_current = |η × ∫ I dt| / ( ΔSoC / 100 )
 *
 *          where ΔSoC is obtained from OCV lookups at each confirmed rest
 *          and the integral accumulates Ah between those rests.  An update
 *          is only accepted when ΔSoC ≥ SOH_MIN_DELTA_SOC_PCT.
 *
 * @author  Orfeu Mouret
 */

#ifndef SOH_H
#define SOH_H

#include "bms_types.h"
#include <stdint.h>

/* =========================================================
 * Algorithm Parameters  (tune per application)
 * ========================================================= */

/** Minimum consecutive duration with |I| < threshold to declare rest [s] */
#define SOH_MIN_REST_DURATION_S         1800   /* 7200.0f for 2 h */

/** Current magnitude below which the cell is considered at rest [A] */
#define SOH_REST_CURRENT_THRESHOLD_A    0.5f

/** Minimum ΔSoC across an active window required for a valid Qmax update [%] */
#define SOH_MIN_DELTA_SOC_PCT           80.0f

/** Nominal capacity used as SoH reference (mirrors bms_types.h) [Ah] */
#define SOH_NOM_CAPACITY_AH             BMS_CELL_CAPACITY_INI_AH

/* =========================================================
 * Internal Phase
 * ========================================================= */

typedef enum {
    SOH_PHASE_SEEKING_REST = 0, /* Waiting for the first confirmed rest period   */
    SOH_PHASE_AT_REST,          /* Inside a confirmed rest period                */
    SOH_PHASE_ACTIVE,           /* Between rests: integrating charge             */
} Soh_Phase_t;

/* =========================================================
 * Estimator State
 * ========================================================= */

typedef struct {
    Soh_Phase_t phase;
    float       rest_timer_s;          /* Consecutive time |I| < threshold [s]    */
    float       soc_at_rest_entry_pct; /* OCV-derived SoC at last confirmed rest   */
    float       charge_integral_ah;    /* Charge accumulated since last rest exit  */
    float       qmax_ah;               /* Latest measured maximum capacity [Ah]   */
    float       soh_pct;               /* Latest SoH estimate [0–100 %]           */
    float       soh_update_time_s;     /* Simulation time of latest SoH update [s]*/
    uint32_t    soh_update_count;      /* Number of valid SoH estimates produced   */
} Soh_State_t;

/* =========================================================
 * API
 * ========================================================= */

/** Initialise estimator state. Must be called before Soh_Update. */
void Soh_Init(Soh_State_t *s);

/**
 * @brief  Process one sample.
 *
 * @param  s           Estimator state
 * @param  current_a   Measured current [A]  (positive = charge)
 * @param  v_meas_mv   Terminal voltage [mV] (used for OCV SoC lookup at rest)
 * @param  t_s         Timestamp of this sample to be processed [s]
 * @param  dt_s        Time step duration [s]
 * @return BMS_OK, or BMS_ERR_NOT_INITIALISED if s == NULL
 */
Bms_Error_t Soh_Update(Soh_State_t *s,
                        float        current_a,
                        float        v_meas_mv,
                        float        t_s,
                        float        dt_s);

/**
 * @brief  Return the latest SoH estimate [0–100 %].
 * @return SoH percentage, or -1.0f if no valid estimate has been produced yet.
 */
float Soh_Get(const Soh_State_t *s);

/**
 * @brief  Batch convenience: scan a full time-series and collect all SoH
 *         updates produced.
 *
 * @param  current_a    Input: current array [A]  (n_samples long)
 * @param  voltage_mv   Input: voltage array [mV] (n_samples long)
 * @param  n_samples    Length of input arrays
 * @param  dt_s         Uniform time step [s]
 * @param  out_times_s  Output: simulation time of each SoH update [s]
 * @param  out_soh_pct  Output: SoH [0–100 %] at each update
 * @param  max_updates  Maximum number of entries the output arrays can hold
 * @return Number of SoH updates written.
 */
uint32_t Soh_ComputeFromTimeSeries(
    const float *current_a,
    const float *voltage_mv,
    uint32_t     n_samples,
    float        dt_s,
    float       *out_times_s,
    float       *out_soh_pct,
    uint32_t     max_updates);

#endif /* SOH_H */
