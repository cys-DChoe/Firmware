/* I2C 100 kHz 타이밍 값 계산 — 하드웨어와 무관해서 맥에서도 시험함 */
#pragma once

#include <stdint.h>

/* 100 kHz TIMINGR — RM0481 표의 예(I2CCLK 8 MHz: PRESC 1 · SCLL 0x13 · SCLH 0x0F · SDADEL 2 · SCLDEL 4)를
 * 같은 250 ns 눈금이 되게 PRESC 만 바꿔 씀: SCL 낮음 5.0 µs · 높음 4.0 µs → 약 100 kHz */
uint32_t i2c_timing_100k(uint32_t kernel_hz);
