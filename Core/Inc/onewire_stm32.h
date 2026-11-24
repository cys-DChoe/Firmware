/* 보드 1-Wire — PB13(TEMP_DQ, pins.h) 오픈 드레인. 풀업은 판의 R_TMP_PU 4.7 kΩ(11/24 붙이기로 바꿈 — 프로브만 꽂아도 됨, 어댑터 4.7 kΩ 과 겹치면 2.35 kΩ)
 * µs 시간은 코어 사이클 카운터(DWT CYCCNT)로 셈 — 깨어 있을 때만 씀(잠 모드에선 안 부름).
 * PB13 은 USB-C CC1 겸용 핀(FT_c)이라 출력이 약함(2 mA 에서 0.4 V) — 1-Wire 는 풀업 2개가 겹쳐도 1.4 mA 라 충분(PCB 검수 검5).
 * 리셋 뒤 '죽은 배터리' 5.1 kΩ 풀다운이 걸릴 수 있어 main() 시작에서 HAL_PWREx_DisableUCPDDeadBattery() 로 끔(11/24 점1).
 */
#pragma once

#include "onewire.h"

void onewire_stm32_init(onewire_t *ow);

/* µs 를 재는 사이클 카운터가 도는지. false 면 reset 이 늘 '없음'을 돌려줌(멈춘 카운터로 무한 대기 안 함) */
bool onewire_stm32_timer_ok(void);
