/* DS18B20 온도 (3선 어댑터, 1-Wire = Arduino D2 = PG14, 어댑터에 4.7 kΩ 풀업) — Nucleo 벤치 전용
 * 데이터시트: 리셋(480 µs 낮음) → 존재 펄스 · Skip ROM 0xCC · Convert T 0x44(12비트 최대 750 ms) · Read Scratchpad 0xBE 9바이트
 *   온도 °C = 부호 있는 16비트 ÷ 16, CRC-8(다항식 x⁸+x⁵+x⁴+1)
 * 시간은 TIM6 을 1 MHz 로 돌려 맞춤. 비트 하나(약 70 µs) 동안만 인터럽트를 막음.
 * 센서 하나만 물린 것으로 봄(Skip ROM).
 */
#pragma once

#include <stdbool.h>

void ds18b20_init(void);
/* 변환 시작. 센서가 답(존재 펄스)하면 true */
bool ds18b20_start(void);
/* 시작하고 750 ms 지난 뒤 부름. CRC 까지 맞으면 true */
bool ds18b20_read(double *t_c);
