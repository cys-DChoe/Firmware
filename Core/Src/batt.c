#include "batt.h"

uint32_t batt_vdda_mv(uint16_t vrefint_raw, uint16_t vrefint_cal)
{
    if (vrefint_raw == 0u) {
        return 0;
    }
    return ((uint32_t)vrefint_cal * BATT_VREFINT_CAL_MV + vrefint_raw / 2u) / vrefint_raw;
}

uint16_t batt_cell_mv(uint16_t raw, uint32_t vdda_mv, int16_t cal_offset_mv)
{
    const int32_t mv = (int32_t)(((uint64_t)raw * vdda_mv * BATT_DIVIDER_NUM + BATT_ADC_MAX / 2u) / BATT_ADC_MAX) +
                       cal_offset_mv;
    if (mv <= 0) {
        return 0;
    }
    return mv > 65535 ? 65535u : (uint16_t)mv;
}
