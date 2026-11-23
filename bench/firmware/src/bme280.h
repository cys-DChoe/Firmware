/* Bosch BME280 대기압 · 온도 · 습도 센서 (I2C 0x76) — 제품 드라이버와 같은 i2c_bus_t 를 씀
 *
 * 벤치에서는 정압 센서(ABP2)와 같은 I2C4 에 붙음. ABP2 는 게이지압(대기압 기준)이라,
 * 밀도 계산에 필요한 절대압 = 이 센서의 대기압 + ABP2 정압.
 * 온도 · 습도는 센서 둘레(관 밖) 공기 값임. 가스 온도는 관 속 DS18B20 이 잼.
 *
 * 센서가 스스로 0.5초마다 재도록(normal 모드) 한 번 걸어 두고, 읽을 때는 최근 값 8 바이트만 가져옴.
 *   대기압 ×16, 온도 ×2, 습도 ×1 겹쳐 재기 + 필터 4 (대기압 잔 흔들림 약 0.2 Pa)
 * 칩 번호 0x60 = BME280, 0x58 = BMP280(습도 없음, 나머지는 같음) 둘 다 받음.
 * 근거: Bosch BME280 데이터시트 rev 1.6 — 4.2.3 보정식(실수형), 5.4 레지스터.
 */
#pragma once

#include <stdbool.h>

#include "i2c_bus.h"

#define BME280_ADDR 0x76

typedef struct {
	uint16_t t1;
	int16_t t2, t3;
	uint16_t p1;
	int16_t p2, p3, p4, p5, p6, p7, p8, p9;
	uint8_t h1, h3;
	int16_t h2, h4, h5;
	int8_t h6;
	bool has_hum; /* BME280 이면 참, BMP280 이면 거짓 */
} bme280_cal_t;

typedef struct {
	double p_pa; /* 대기압 (Pa, 절대압) */
	double t_c;  /* 센서 둘레 온도 (°C) */
	double rh;   /* 상대 습도 (%). BMP280 이면 0 */
} bme280_reading_t;

/* 칩 번호 확인 → 보정값 읽기 → 연속 측정 걸기. 응답 없으면 SENS_E_BUS, 다른 칩이면 SENS_E_ID */
sens_err_t bme280_start(const i2c_bus_t *bus, bme280_cal_t *cal);

/* 최근 값. 아직 한 번도 안 쟀거나 센서가 꺼졌다 켜져 잠든 상태면 SENS_E_BUSY (다시 bme280_start 필요) */
sens_err_t bme280_read(const i2c_bus_t *bus, const bme280_cal_t *cal, bme280_reading_t *out);
