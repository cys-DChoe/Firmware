/* 판 PCB3_2 (GasMeter_19, 11/16 거버 · 11/18 JLC 주문 · 만드는 중) — 판마다 다른 것만 여기
 * 빌드: cmake --build build.nosync --target gasmeter_pcb3_2.elf */
#pragma once

#define BOARD_NAME          "PCB3_2"
#define BOARD_HAS_SDP810    0   /* J_SDP 헤더 없음 → SDP810 안 찾음, 보고 24 번 늘 0x8000 */
#define BOARD_HAS_DS18B20   0   /* J_TMP 없음 · PB13 빈 핀 → 1-Wire 안 켬, 보고 26 번 늘 0x8000 */
#define BOARD_PA9_GND       0   /* PA9 빈 핀 */

#include "pins.h"
