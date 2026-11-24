/* DS18B20 가스 온도 — 다음 판(PCB3_3) J_TMP 3핀(GND · 3V3 · DAT)에 방수 프로브 + 단자 어댑터(4.7 kΩ 풀업)
 * DAT = TEMP_DQ = PB13 (R_TMP 100 Ω · D_TMP ESD 거쳐 J_TMP 3번)
 *
 * 데이터시트(Dallas 050400): 외부 전원 3.0–5.5 V, 풀업 약 5 kΩ, 12비트 변환 최대 750 ms(1.5 mA), 쉴 때 최대 1 µA
 *   Skip ROM 0xCC(센서 하나) · Convert T 0x44 · Read Scratchpad 0xBE(9바이트, 마지막이 CRC-8)
 *   온도 °C = 부호 있는 16비트 ÷ 16 (12비트 0.0625 °C), 켠 뒤 처음 값 85.0 °C 는 아직 안 잰 값
 *
 * 쓰는 순서 (sensors.c): 60 초마다(SENSORS_PROBE_PERIOD_MS) ds18b20_convert → 750 ms 넘게 지난 다음 읽기에서 ds18b20_read
 * 센서가 없는 판(PCB3_2: PB13 은 빈 핀)에선 존재 펄스가 없어 바로 "없음"
 */
#pragma once

#include <stdbool.h>
#include "i2c_bus.h"     /* sens_err_t */
#include "onewire.h"

#define DS18B20_CONV_MS   750u
#define DS18B20_POWERON_C 85.0

/* 변환 시작. 센서가 답하면 SENS_OK, 없으면 SENS_E_BUS */
sens_err_t ds18b20_convert(const onewire_t *ow);

/* 변환 시작 후 DS18B20_CONV_MS 지나서 부름. CRC 가 맞으면 SENS_OK.
 * 켠 뒤 아직 안 잰 값(85.0 °C, 스크래치패드 기본값)이면 SENS_STALE */
sens_err_t ds18b20_read(const onewire_t *ow, double *t_c);
