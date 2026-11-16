/* 셀 전압 계산 (보드 없이 맥에서 시험)
 *
 * 회로: 셀 → 100 kΩ / 100 kΩ 나눔 → PA0 (ADC12_INP0), 100 nF (Docs/GasMeter_19_펌웨어_주의.html 7번)
 * 3V3 자체가 ±1.5 % 흔들려서, 내부 기준 VREFINT 를 같이 재 VDDA 를 구하고 그 값으로 환산함
 *   VDDA = VREFINT_CAL × 3300 mV / VREFINT 원값   (VREFINT_CAL: 공장에서 3.3 V 로 잰 값, 0x08FFF810)
 *   셀   = 원값 × VDDA / 4095 × 2 + 보정값     (보정값: 생산 때 1점 보정, 설정에 저장)
 */
#pragma once

#include <stdint.h>

#define BATT_ADC_MAX        4095u
#define BATT_DIVIDER_NUM    2u          /* (100 k + 100 k) / 100 k */
#define BATT_VREFINT_CAL_MV 3300u

uint32_t batt_vdda_mv(uint16_t vrefint_raw, uint16_t vrefint_cal);
/* 원값 · VDDA → 셀 mV (0 이하면 0, 65535 넘으면 65535) */
uint16_t batt_cell_mv(uint16_t raw, uint32_t vdda_mv, int16_t cal_offset_mv);
