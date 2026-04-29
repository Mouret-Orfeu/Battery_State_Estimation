/**
 * @file    soc_ocv.h
 * @brief   OCV Lookup Table SoC Estimator API
 *
 * @author  Kamal Kadakara
 */
#ifndef SOC_OCV_H
#define SOC_OCV_H

#include "bms_types.h"

/**
 * @brief  Estimate SoC from OCV via linear interpolation.
 *         Only valid when the cell is at electrochemical equilibrium (≥ 2 h rest).
 *
 * @param  ocv_mv   Measured open-circuit voltage [mV]
 * @param  soc_out  Output SoC estimate [0.0–100.0 %]
 * @return BMS_OK on success; BMS_ERR_VOLTAGE_OOT if OCV is outside the table range
 */
Bms_Error_t SocOcv_LookupSoc(float ocv_mv, float *soc_out);

/**
 * @brief  Get OCV for a given SoC via linear interpolation (inverse lookup).
 *         Used by the EKF output equation to compute the predicted terminal voltage.
 *
 * @param  soc_pct  SoC [0.0–100.0 %]
 * @return OCV [mV]
 */
float       SocOcv_GetOcv(float soc_pct);

#endif /* SOC_OCV_H */
