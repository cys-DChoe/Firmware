/* 판 PCB3_3 (GasMeter_19 다음 판, 11/24 거버 · JLC 견적만, 주문 전) — 판마다 다른 것만 여기
 * PCB3_2 에 더한 것: J_SDP(SDP810, I2C3 0x25, 선으로) · J_TMP(DS18B20, PB13 1-Wire, R_TMP 100 Ω · D_TMP · R_TMP_PU 4.7 kΩ) · PA9 → GND
 * 빌드: cmake --build build.nosync --target gasmeter_pcb3_3.elf */
#pragma once

#define BOARD_NAME          "PCB3_3"
#define BOARD_HAS_SDP810    1   /* I2C3 에서 찾음, 없으면 SDP810_RETRY_READS 번마다 한 번 다시 찾음 */
#define BOARD_HAS_DS18B20   1   /* PB13 오픈 드레인, 존재 펄스 없으면 "없음" */
#define BOARD_PA9_GND       1   /* PA9 를 출력으로 쓰면 단락 — 펌웨어는 켤 때 풀업 입력으로 한 번 읽어 판 확인만 함(board_check.h, 읽은 뒤 아날로그) */

#include "pins.h"
