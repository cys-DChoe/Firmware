/* Bosch BMP390 기압 · 온도 센서 (I2C 0x76, SDO = GND) — U-S3, I2C3 (ABP2 와 같은 버스)
 *
 * BMP390 데이터시트 4절 레지스터:
 *   0x00 CHIP_ID = 0x60 · 0x02 ERR · 0x03 STATUS(4 cmd_rdy · 5 drdy_press · 6 drdy_temp)
 *   0x04–0x09 압력 · 온도 원값(각 24비트, 아래 바이트 먼저) · 0x1B PWR_CTRL · 0x1C OSR
 *   0x31–0x45 보정값 21바이트 · 0x7E CMD(0xB6 = 다시 시작)
 * 강제(forced) 모드로 한 번씩 잼 — 쓰지 않을 땐 잠(sleep) 상태라 전류가 거의 없음.
 * 보정식은 Bosch BMP3_SensorAPI 의 실수 계산(bmp3.c parse_calib_data · compensate_*)과 같음 —
 * tests/host 에서 Bosch 코드로 낸 값과 대조함.
 * 변환 시간 (3.9.2): 234 µs + 압력(392 + 2^osr_p × 2020) + 온도(163 + 2^osr_t × 2020)
 *   기압 ×4 · 온도 ×1 → 약 10.9 ms
 */
#pragma once

#include "i2c_bus.h"

#define BMP390_ADDR         0x76u
#define BMP390_CHIP_ID      0x60u
#define BMP390_STARTUP_MS   2u

typedef struct {
    double t1, t2, t3;
    double p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
} bmp390_calib_t;

typedef struct {
    bmp390_calib_t cal;
    uint8_t osr_p;          /* 2 = ×4 */
    uint8_t osr_t;          /* 0 = ×1 */
} bmp390_t;

typedef struct {
    uint32_t p_raw, t_raw;
    double p_pa;
    double t_c;
} bmp390_reading_t;

/* 칩 확인 · 다시 시작 · 보정값 읽기 · 측정 설정 */
sens_err_t bmp390_init(const i2c_bus_t *bus, bmp390_t *dev);
/* 한 번 재기 (강제 모드, 약 11 ms) */
sens_err_t bmp390_measure(const i2c_bus_t *bus, const bmp390_t *dev, bmp390_reading_t *out);

/* 순수 계산 (시험용으로 따로 둠) */
void bmp390_parse_calib(const uint8_t nvm[21], bmp390_calib_t *cal);
/* 범위 밖이면 Bosch 와 같게 끝값(−40…85 °C, 30…125 kPa)으로 자르고 SENS_E_RANGE */
sens_err_t bmp390_compensate(const bmp390_calib_t *cal, uint32_t p_raw, uint32_t t_raw, double *p_pa, double *t_c);
uint32_t bmp390_conv_time_us(uint8_t osr_p, uint8_t osr_t);
