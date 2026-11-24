/* 보드 1-Wire — PB13(TEMP_DQ, pins.h) 오픈 드레인. 풀업은 단자 어댑터의 4.7 kΩ(빈 자리 R_TMP_PU 는 안 붙임)
 * µs 시간은 코어 사이클 카운터(DWT CYCCNT)로 셈 — 깨어 있을 때만 씀(잠 모드에선 안 부름).
 * PB13 은 USB-C CC1 겸용 핀(FT_c)이라 출력이 약함(2 mA 에서 0.4 V) — 1-Wire 는 0.7 mA 라 충분(PCB 검수 검5).
 */
#pragma once

#include "onewire.h"

void onewire_stm32_init(onewire_t *ow);
