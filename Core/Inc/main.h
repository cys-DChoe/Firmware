/* 가스 계량기 펌웨어 — 공통 헤더
 * 보드: GasMeter_19 PCB3_2 · MCU STM32H563ZGT6 (1 MB 플래시 중 은행 1 = 512 KB 만 씀)
 * 설계서: https://claude.ai/artifact/4pURFGYAHrpZGocGMBt1hs
 */
#pragma once

#include "stm32h5xx_hal.h"
#include "pins.h"

void Error_Handler(void);
