/* Sensirion SDP810-500Pa 차압 센서 (I2C 0x25) — 제품 드라이버와 같은 i2c_bus_t 를 씀
 *
 * 연속 측정(0x3615, 읽을 때까지 평균)을 한 번 걸어 두고, 읽을 때마다 그 사이 평균을 받음.
 * 9 바이트 = 차압 · 온도 · 배율이 2 바이트 + CRC 씩. 차압 = 값 / 배율(500 Pa 형은 60), 온도 = 값 / 200.
 * 근거: Sensirion SDP8xx-Digital 데이터시트 v1.1 (2019-04) 5장 · 6장.
 */
#pragma once

#include "i2c_bus.h"

#define SDP810_ADDR 0x25

typedef struct {
	double p_pa;      /* 차압 (Pa), + 포트가 높으면 양수 */
	double t_c;       /* 센서 온도 (°C) */
	uint16_t scale;   /* 센서가 알려 준 배율 (500 Pa 형 = 60) */
} sdp810_reading_t;

/* 혹시 돌던 측정을 멈추고 연속 측정을 새로 걺. 응답 없으면 SENS_E_BUS */
sens_err_t sdp810_start(const i2c_bus_t *bus);

/* 마지막 읽기 이후 평균값. CRC 틀리면 SENS_E_STATUS, 끝값(포화)이면 SENS_E_RANGE(값은 채움) */
sens_err_t sdp810_read(const i2c_bus_t *bus, sdp810_reading_t *out);
