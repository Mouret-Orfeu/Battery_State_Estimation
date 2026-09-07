/**
 * @file    bms_types.h
 * @brief   BMS common types: cell parameters, SoC state, error codes
 *          Designed for ASIL C/D automotive BMS (ISO 26262)
 *
 * @author  Kamal Kadakara
 */

#ifndef BMS_TYPES_H
#define BMS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================
 * Cell / Pack Parameters (NMC Chemistry defaults)
 * ========================================================= */
#define BMS_CELL_VOLTAGE_MIN_MV     3000U   /* 3.0 V — deep discharge limit   */
#define BMS_CELL_VOLTAGE_MAX_MV     4200U   /* 4.2 V — full charge limit       */
#define BMS_CELL_VOLTAGE_NOM_MV     3700U   /* 3.7 V — nominal                 */
// #define BMS_CELL_CAPACITY_INI_AH        60.0f   /* Ah — nominal capacity (from former project)         */
#define BMS_CELL_CAPACITY_INI_AH        3.4f   /* Ah — nominal capacity for dataset UL-PUR         */

/* Bounds on the ratio of a *measured* maximum capacity (Qmax, produced by the
 * SoH estimator) to the *nominal* one above.  The ratio is SoH expressed as a
 * fraction, and it is read in three bands:
 *
 *   ratio > MAX, or ratio < MIN : implausible *measurement*, not real ageing —
 *       above the ceiling the cell would hold more charge than it ever did when
 *       new, below the floor it would be an all but empty cell still under
 *       load.  The estimate is discarded outright: it reaches neither the SoH
 *       output nor the SoC estimator, where it would scale the whole current
 *       integral, and the previous values stand.
 *   MIN <= ratio < EOL : plausible, but the cell has aged past the end of its
 *       service life.  The measurement is kept and still drives SoC — a
 *       worn-out cell is exactly the case where the nominal capacity would
 *       misestimate SoC most — and the caller is told about the wear.
 *   EOL <= ratio <= MAX : healthy cell, nothing to report.
 */
#define BMS_CAPACITY_RATIO_MIN      0.10f   /* 10 % of nominal — below: rejected  */
#define BMS_CAPACITY_RATIO_MAX      1.05f   /* 105 % of nominal — above: rejected */
#define BMS_CAPACITY_RATIO_EOL      0.60f   /* 60 % of nominal — below: worn out  */
/* Coulombic efficiencies — reference values only.  The BMS applies η itself
 * when it integrates the cell current, so the estimators must NOT re-apply it. */
#define BMS_COULOMBIC_EFF_CHG       0.999f  /* Charge efficiency               */
#define BMS_COULOMBIC_EFF_DCHG      1.000f  /* Discharge efficiency            */
#define BMS_SAMPLE_TIME_S           0.4f    /* 400 ms sampling interval        */

/* SoC limits — clamp estimated value to these bounds */
#define BMS_SOC_MIN_PCT             0.0f
#define BMS_SOC_MAX_PCT             100.0f

/* =========================================================
 * ECM (Equivalent Circuit Model) Parameters — 1-RC Randles
 * ========================================================= */
/*These are here for now, but when you will want to make them vary depending on the cell, 
T, aging etc, you'll have to move them in the Bms_EkfState_t struct*/
typedef struct {
    float R0;   /* Ohmic resistance [Ω]          */
    float R1;   /* Polarisation resistance [Ω]   */
    float C1;   /* Polarisation capacitance [F]  */
} Bms_EcmParams_t;

/* Default NMC cell ECM parameters (25 °C) */
#define BMS_ECM_DEFAULT { .R0 = 0.005f, .R1 = 0.008f, .C1 = 1500.0f }

/* =========================================================
 * SoC State Structure (shared across estimators)
 * ========================================================= */
typedef struct {
    float   soc_pct;            /* Current SoC estimate [0.0 – 100.0] */
    float   soc_prev_pct;       /* Previous cycle SoC                  */
    float   v_terminal_mv;      /* Measured terminal voltage [mV]      */
    float   current_a;          /* Measured current [A] (positive = charging) */
    float   temperature_degc;   /* Cell temperature [°C]               */
    bool    is_initialised;     /* SoC has been bootstrapped           */
} Bms_SocState_t;

/* =========================================================
 * EKF State — internal, managed by soc_ekf.c
 * ========================================================= */
typedef struct {
    float x[2];       /* State: [SoC, V_RC] */
    float P[2][2];    /* Error covariance matrix */
    float Q[2][2];    /* Process noise covariance */
    float R;          /* Measurement noise variance */
    /* Capacity the prediction step divides the current integral by [Ah].
     * Starts at BMS_CELL_CAPACITY_INI_AH and is refreshed from the SoH
     * estimator through SocEkf_SetCapacityAh(), so the filter integrates
     * against the capacity the cell has now rather than the one it had new. */
    float capacity_ah;
} Bms_EkfState_t;

/* =========================================================
 * Error / Fault Codes
 * ========================================================= */
typedef enum {
    BMS_OK                     = 0x00U,
    BMS_ERR_VOLTAGE_OOT        = 0x01U,  /* Voltage out of range        */
    BMS_ERR_CURRENT_OOT        = 0x02U,  /* Current sensor saturated    */
    BMS_ERR_SOC_DIVERGED       = 0x04U,  /* EKF/CC divergence detected  */
    BMS_ERR_NOT_INITIALISED    = 0x08U,  /* SoC not bootstrapped        */
    BMS_ERR_TEMP_OOT           = 0x10U,  /* Temperature out of range    */
    BMS_ERR_INVALID_PARAM      = 0x20U,  /* Invalid argument or file data */
    BMS_ERR_CAPACITY_IMPLAUSIBLE = 0x40U, /* Measured Qmax outside plausibility band */
    BMS_ERR_CAPACITY_EOL         = 0x80U, /* Cell aged past end of life (SoH < EOL)  */
} Bms_Error_t;

#endif /* BMS_TYPES_H */
