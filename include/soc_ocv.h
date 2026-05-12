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
 * @brief  Load the OCV–SoC lookup table from a CSV file (soc,ocv_mv columns).
 *         Rows whose SoC aligns with OCV_SOC_STEP_PCT intervals are extracted;
 *         returns BMS_ERR_INVALID_PARAM if the matched count != OCV_TABLE_ENTRIES,
 *         the file cannot be opened, or any row cannot be parsed.
 *
 * @param  csv_path  Path to the processed CSV (e.g. OCV_SOC_NCA_1_processed.csv)
 * @return BMS_OK on success; BMS_ERR_INVALID_PARAM on any failure
 */
Bms_Error_t SocOcv_LoadTableFromCsv(const char *csv_path);

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
