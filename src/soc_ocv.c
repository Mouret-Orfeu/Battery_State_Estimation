/**
 * @file    soc_ocv.c
 * @brief   OCV (Open Circuit Voltage) Lookup Table SoC estimator
 *          NMC cell OCV–SoC table, 21 points, 0–100% in 5% steps
 *          Linear interpolation between entries.
 *
 * @author  Kamal Kadakara
 */

#include "soc_ocv.h"
#include <stddef.h>

/* ---- NMC OCV Table (measured at 25°C, 1C current, after 2h rest) ----
 * Index 0 = 0% SoC (3000 mV), Index 20 = 100% SoC (4200 mV)
 * Values in millivolts.
 */
static const float s_ocv_table_mv[21] = {
    3000.0f,  /* 0%  */
    3100.0f,  /* 5%  */
    3250.0f,  /* 10% */
    3350.0f,  /* 15% */
    3420.0f,  /* 20% */
    3470.0f,  /* 25% */
    3520.0f,  /* 30% */
    3560.0f,  /* 35% */
    3600.0f,  /* 40% */
    3630.0f,  /* 45% */
    3660.0f,  /* 50% */
    3690.0f,  /* 55% */
    3720.0f,  /* 60% */
    3760.0f,  /* 65% */
    3800.0f,  /* 70% */
    3850.0f,  /* 75% */
    3920.0f,  /* 80% */
    4010.0f,  /* 85% */
    4080.0f,  /* 90% */
    4150.0f,  /* 95% */
    4200.0f,  /* 100% */
};

#define OCV_TABLE_ENTRIES   21U
#define OCV_SOC_STEP_PCT    5.0f

Bms_Error_t SocOcv_LookupSoc(float ocv_mv, float *soc_out)
{
    if (soc_out == NULL) return BMS_ERR_NOT_INITIALISED;

    /* Boundary checks */
    if (ocv_mv <= s_ocv_table_mv[0]) {
        *soc_out = 0.0f;
        return BMS_ERR_VOLTAGE_OOT;
    }
    if (ocv_mv >= s_ocv_table_mv[OCV_TABLE_ENTRIES - 1U]) {
        *soc_out = 100.0f;
        return BMS_ERR_VOLTAGE_OOT;
    }

    /* Find bracketing entries */
    for (uint8_t i = 0U; i < (OCV_TABLE_ENTRIES - 1U); i++) {
        if (ocv_mv >= s_ocv_table_mv[i] && ocv_mv < s_ocv_table_mv[i + 1U]) {
            /* Linear interpolation */
            float frac = (ocv_mv - s_ocv_table_mv[i]) /
                         (s_ocv_table_mv[i + 1U] - s_ocv_table_mv[i]);
            *soc_out = (float)i * OCV_SOC_STEP_PCT + frac * OCV_SOC_STEP_PCT;
            return BMS_OK;
        }
    }

    return BMS_ERR_VOLTAGE_OOT;
}

float SocOcv_GetOcv(float soc_pct)
{
    if (soc_pct <= 0.0f)   return s_ocv_table_mv[0];
    if (soc_pct >= 100.0f) return s_ocv_table_mv[OCV_TABLE_ENTRIES - 1U];

    uint8_t idx   = (uint8_t)(soc_pct / OCV_SOC_STEP_PCT);
    float   frac  = (soc_pct - (float)idx * OCV_SOC_STEP_PCT) / OCV_SOC_STEP_PCT;

    if (idx >= (OCV_TABLE_ENTRIES - 1U)) {
        return s_ocv_table_mv[OCV_TABLE_ENTRIES - 1U];
    }

    return s_ocv_table_mv[idx] + frac * (s_ocv_table_mv[idx + 1U] - s_ocv_table_mv[idx]);
}
