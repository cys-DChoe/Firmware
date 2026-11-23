/* Sensirion SDP810 차압 (브레드보드, I2C1 0x25) — Nucleo 벤치 전용
 * SDP8xx 데이터시트: 연속 측정 시작 0x3615(차압 · 온도 보상 · 읽을 때까지 평균), 멈춤 0x3FF9
 *   읽기 9바이트 = 차압 2 + CRC · 온도 2 + CRC · 눈금 2 + CRC  (CRC-8: 다항식 0x31, 처음 0xFF)
 *   차압 Pa = 값 ÷ 눈금(500 Pa 품번 60 · 125 Pa 품번 240 — 센서가 알려 줌), 온도 °C = 값 ÷ 200
 *   시작 뒤 첫 값까지 8 ms
 */
#pragma once

#include "i2c_bus.h"

#define SDP810_ADDR  0x25u

/* 멈춤 → 연속 측정 시작 (이미 돌고 있어도 다시 맞춤) */
sens_err_t sdp810_start(const i2c_bus_t *bus);
sens_err_t sdp810_read(const i2c_bus_t *bus, double *dp_pa, double *t_c);
