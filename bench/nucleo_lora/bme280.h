/* Bosch BME280 기압 · 온도 (브레드보드 모듈, I2C4 0x76) — 습도는 안 씀
 * 데이터시트 BST-BME280-DS002: 칩 ID 0xD0 = 0x60 · 보정값 0x88–0x9F · 강제 모드 한 번 재기 · 4.2.3 실수 보정식
 * 새 PCB 는 BMP390(Core/Src/bmp390.c) 이고, 이건 Nucleo 벤치 전용.
 */
#pragma once

#include "i2c_bus.h"

#define BME280_ADDR     0x76u
#define BME280_CHIP_ID  0x60u

typedef struct {
    uint16_t t1;
    int16_t t2, t3;
    uint16_t p1;
    int16_t p2, p3, p4, p5, p6, p7, p8, p9;
} bme280_cal_t;

/* 칩 ID 확인 + 보정값 읽기 */
sens_err_t bme280_init(const i2c_bus_t *bus, bme280_cal_t *cal);
/* 강제 모드로 한 번 재고 Pa · °C 로 */
sens_err_t bme280_measure(const i2c_bus_t *bus, const bme280_cal_t *cal, double *p_pa, double *t_c);
