/* Bosch BMP280 기압 · 온도 — 11/26 BMP390 대신 (BMP390 재고 0, 리드 16주)
 *   PCB3_3: J_BME 4핀 헤더(VIN · GND · SCL · SDA)에 모듈을 납땜, I2C3 (ABP2 · SDP810 과 같은 버스)
 *   같은 핀 · 같은 레지스터인 BME280 모듈(칩 ID 0x60)도 그대로 받음 — 습도는 안 씀(다시 시작하면 습도 측정은 꺼진 채)
 *
 * BMP280 데이터시트 BST-BMP280-DS001 Rev 1.26 (10/2021):
 *   0xD0 id = 0x58 (4.3.1) · 0xE0 reset ← 0xB6 (4.3.2) · 0xF3 status: bit3 measuring · bit0 im_update (4.3.3)
 *   0xF4 ctrl_meas = osrs_t[7:5] · osrs_p[4:2] · mode[1:0], 01 = 강제(forced) (4.3.4, 표 10)
 *   0xF5 config = t_sb · filter[4:2] · spi3w — 잠(sleep)일 때만 써짐 (4.3.5)
 *   0xF7–0xFC 기압 · 온도 원값 각 20비트(msb · lsb · xlsb[7:4]), 한 번에 읽어야 같은 측정 값 (3.9 · 4.3.6–7)
 *   0x88–0x9F 보정값 dig_T1–T3 · dig_P1–P9 (16비트, 아래 바이트 먼저, T1 · P1 만 부호 없음) (3.11.2 표 17)
 *   주소 0x76(SDO = GND) · 0x77(SDO = VDDIO) (5.2) — 모듈마다 달라서 둘 다 찾아봄
 *   기압 ×4 · 온도 ×1(표 4 "standard resolution", 0.66 Pa) · IIR 끔 → 측정 최대 13.3 ms (표 13) · 시작 2 ms (표 2)
 *   보정식: 8.1 실수식. t_fine 은 32비트 정수로 자른 뒤 기압에 씀(데이터시트 · Bosch 코드 그대로).
 *   범위 밖이면 Bosch BME280_SensorAPI(c90d419) compensate_* 처럼 끝값(−40…85 °C, 300…1100 hPa)으로 자르고 SENS_E_RANGE.
 *   tests/host 에서 그 Bosch 코드로 낸 기준값 14개와 대조 (BME280 도 온도 · 기압 보정은 BMP280 과 같은 식 · 같은 보정값 자리).
 */
#pragma once

#include <stdbool.h>
#include "i2c_bus.h"

#define BMP280_ADDR_LO      0x76u   /* SDO = GND — 먼저 찾아봄 */
#define BMP280_ADDR_HI      0x77u   /* SDO = VDDIO */
#define BMP280_CHIP_ID      0x58u   /* 0x56 · 0x57 = 시제품 칩 — 이것도 받음 */
#define BME280_CHIP_ID      0x60u
#define BMP280_STARTUP_MS   2u
#define BMP280_MEAS_MAX_MS  14u     /* 표 13 standard 최대 13.3 ms → 올림 */
#define BMP280_CTRL_MEAS    0x2Du   /* osrs_t 001(×1) · osrs_p 011(×4) · mode 01(강제) */

typedef struct {
    uint16_t t1;
    int16_t t2, t3;
    uint16_t p1;
    int16_t p2, p3, p4, p5, p6, p7, p8, p9;
} bmp280_calib_t;

typedef struct {
    bmp280_calib_t cal;
    uint8_t addr;           /* 찾은 주소 0x76 · 0x77 (0 = 아직) */
    uint8_t chip_id;        /* 0x58 BMP280 · 0x60 BME280 (로그용) */
} bmp280_t;

typedef struct {
    int32_t p_raw, t_raw;   /* 20비트 원값 */
    double p_pa;
    double t_c;
} bmp280_reading_t;

/* 0x76 → 0x77 순서로 칩 찾기 · 다시 시작 · 보정값 읽기 · IIR 끔.
 * 아무도 답이 없으면 SENS_E_BUS, 답은 했는데 다른 칩이면 SENS_E_ID, 보정값이 0 이면 SENS_E_STATUS */
sens_err_t bmp280_init(const i2c_bus_t *bus, bmp280_t *dev);
/* 한 번 재기 (강제 모드, 최대 13.3 ms 기다린 뒤 measuring 비트 확인) */
sens_err_t bmp280_measure(const i2c_bus_t *bus, const bmp280_t *dev, bmp280_reading_t *out);

/* 순수 계산 (시험용으로 따로 둠) */
void bmp280_parse_calib(const uint8_t nvm[24], bmp280_calib_t *cal);
sens_err_t bmp280_compensate(const bmp280_calib_t *cal, int32_t p_raw, int32_t t_raw, double *p_pa, double *t_c);
