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
#define BMS_CELL_CAPACITY_INI_AH        60.0f   /* Ah — nominal capacity           */
#define BMS_COULOMBIC_EFF_CHG       0.999f  /* Charge efficiency               */
#define BMS_COULOMBIC_EFF_DCHG      1.000f  /* Discharge efficiency            */
#define BMS_SAMPLE_TIME_S           0.1f    /* 100 ms sampling interval        */

/* SoC limits — clamp estimated value to these bounds */
#define BMS_SOC_MIN_PCT             0.0f
#define BMS_SOC_MAX_PCT             100.0f

/* =========================================================
 * ECM (Equivalent Circuit Model) Parameters — 1-RC Randles
 * ========================================================= */
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
    float   current_a;          /* Measured current [A] (+ve = charge) */
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
} Bms_Error_t;

#endif /* BMS_TYPES_H */
