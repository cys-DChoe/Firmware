/* 보드 쪽 셀 전압 재기 — ADC1: PA0(INP0, 셀 ÷ 2) · VREFINT, 잴 때만 켜고 끝나면 끔 */
#pragma once

#include <stdint.h>

/* 16번씩 재 평균. 실패하면 0. 1분 보고 직전(LoRa 송신 전, 쉬는 상태)에 부름. */
uint16_t batt_measure_mv(int16_t cal_offset_mv);
