/**
 * @file    soc_ocv.h
 * @brief   OCV Lookup Table SoC Estimator API
 *
 * @author  Kamal Kadakara
 */
#ifndef SOC_OCV_H
#define SOC_OCV_H

#include "bms_types.h"

Bms_Error_t SocOcv_LookupSoc(float ocv_mv, float *soc_out);
float       SocOcv_GetOcv(float soc_pct);

#endif /* SOC_OCV_H */
