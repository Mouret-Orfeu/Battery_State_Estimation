/**
 * @file    soc_coulomb.h
 * @brief   Coulomb Counting SoC Estimator API
 *
 * @author  Kamal Kadakara
 */
#ifndef SOC_COULOMB_H
#define SOC_COULOMB_H

#include "bms_types.h"

void      SocCoulomb_Init(Bms_SocState_t *state, float initial_soc_pct);
Bms_Error_t SocCoulomb_Update(Bms_SocState_t *state,
                              float current_a,
                              float dt_s);

#endif /* SOC_COULOMB_H */
