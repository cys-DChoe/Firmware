/* 가스 계량기 펌웨어 — 공통 헤더
 * 보드: GasMeter_19 PCB3_2 · PCB3_3 (boards/ 폴더, CMake 가 판마다 따로 빌드) · MCU STM32H563ZGT6 (1 MB 플래시 중 은행 1 = 512 KB 만 씀)
 * 설계서: https://claude.ai/artifact/4pURFGYAHrpZGocGMBt1hs
 */
#pragma once

#include "stm32h5xx_hal.h"
#include "board.h"      /* boards/<판>/board.h — 판 이름 · 있는 센서 · 핀(pins.h) */

void Error_Handler(void);
