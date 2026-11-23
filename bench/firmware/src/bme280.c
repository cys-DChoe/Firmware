/* Bosch BME280 드라이버 — bme280.h 참고 */
#include "bme280.h"

#define REG_CALIB_TP 0x88 /* 0x88..0xA1: T1..T3, P1..P9, (빈 칸), H1 */
#define REG_ID       0xD0
#define REG_CALIB_H  0xE1 /* 0xE1..0xE7: H2..H6 */
#define REG_CTRL_HUM 0xF2
#define REG_CTRL     0xF4
#define REG_CONFIG   0xF5
#define REG_DATA     0xF7 /* 0xF7..0xFE: 압력 3, 온도 3, 습도 2 바이트 */

#define ID_BME280 0x60
#define ID_BMP280 0x58

#define CTRL_HUM_X1 0x01 /* 습도 ×1 */
#define CONFIG_RUN  0x88 /* 쉬는 시간 500 ms(100), 필터 4(010) */
#define CTRL_RUN    0x57 /* 온도 ×2(010), 압력 ×16(101), normal 모드(11) */

static int rd(const i2c_bus_t *bus, uint8_t reg, uint8_t *d, size_t n)
{
	return bus->write_read(bus->ctx, BME280_ADDR, &reg, 1, d, n);
}

static int wr(const i2c_bus_t *bus, uint8_t reg, uint8_t v)
{
	const uint8_t w[2] = {reg, v};

	return bus->write(bus->ctx, BME280_ADDR, w, sizeof(w));
}

static uint16_t u16(const uint8_t *d)
{
	return (uint16_t)(d[0] | (d[1] << 8));
}

sens_err_t bme280_start(const i2c_bus_t *bus, bme280_cal_t *cal)
{
	uint8_t id = 0, a[26], e[7] = {0};

	if (rd(bus, REG_ID, &id, 1) != 0) {
		return SENS_E_BUS;
	}
	if (id != ID_BME280 && id != ID_BMP280) {
		return SENS_E_ID;
	}
	cal->has_hum = id == ID_BME280;
	if (rd(bus, REG_CALIB_TP, a, sizeof(a)) != 0 || (cal->has_hum && rd(bus, REG_CALIB_H, e, sizeof(e)) != 0)) {
		return SENS_E_BUS;
	}
	cal->t1 = u16(&a[0]);
	cal->t2 = (int16_t)u16(&a[2]);
	cal->t3 = (int16_t)u16(&a[4]);
	cal->p1 = u16(&a[6]);
	cal->p2 = (int16_t)u16(&a[8]);
	cal->p3 = (int16_t)u16(&a[10]);
	cal->p4 = (int16_t)u16(&a[12]);
	cal->p5 = (int16_t)u16(&a[14]);
	cal->p6 = (int16_t)u16(&a[16]);
	cal->p7 = (int16_t)u16(&a[18]);
	cal->p8 = (int16_t)u16(&a[20]);
	cal->p9 = (int16_t)u16(&a[22]);
	cal->h1 = a[25];
	cal->h2 = (int16_t)u16(&e[0]);
	cal->h3 = e[2];
	cal->h4 = (int16_t)(((int8_t)e[3] * 16) | (e[4] & 0x0F));
	cal->h5 = (int16_t)(((int8_t)e[5] * 16) | (e[4] >> 4));
	cal->h6 = (int8_t)e[6];

	/* 설정은 잠든 상태에서 써야 먹음. 습도 설정은 그 뒤 ctrl_meas 를 써야 반영됨 */
	if (wr(bus, REG_CTRL, 0x00) != 0 || (cal->has_hum && wr(bus, REG_CTRL_HUM, CTRL_HUM_X1) != 0) ||
	    wr(bus, REG_CONFIG, CONFIG_RUN) != 0 || wr(bus, REG_CTRL, CTRL_RUN) != 0) {
		return SENS_E_BUS;
	}
	return SENS_OK;
}

sens_err_t bme280_read(const i2c_bus_t *bus, const bme280_cal_t *cal, bme280_reading_t *out)
{
	uint8_t d[8];

	if (rd(bus, REG_DATA, d, sizeof(d)) != 0) {
		return SENS_E_BUS;
	}
	int32_t adc_p = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
	int32_t adc_t = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
	int32_t adc_h = ((int32_t)d[6] << 8) | d[7];

	if (adc_p == 0x80000 || adc_t == 0x80000) {
		return SENS_E_BUSY; /* 켜진 뒤 한 번도 안 잰 값 그대로 */
	}

	double v1 = (adc_t / 16384.0 - cal->t1 / 1024.0) * cal->t2;
	double v2 = (adc_t / 131072.0 - cal->t1 / 8192.0) * (adc_t / 131072.0 - cal->t1 / 8192.0) * cal->t3;
	double t_fine = v1 + v2;

	out->t_c = t_fine / 5120.0;

	v1 = t_fine / 2.0 - 64000.0;
	v2 = v1 * v1 * cal->p6 / 32768.0;
	v2 = v2 + v1 * cal->p5 * 2.0;
	v2 = v2 / 4.0 + cal->p4 * 65536.0;
	v1 = (cal->p3 * v1 * v1 / 524288.0 + cal->p2 * v1) / 524288.0;
	v1 = (1.0 + v1 / 32768.0) * cal->p1;
	if (v1 == 0.0) {
		return SENS_E_STATUS; /* 보정값이 비어 있음 */
	}
	double p = 1048576.0 - adc_p;

	p = (p - v2 / 4096.0) * 6250.0 / v1;
	v1 = cal->p9 * p * p / 2147483648.0;
	v2 = p * cal->p8 / 32768.0;
	out->p_pa = p + (v1 + v2 + cal->p7) / 16.0;

	out->rh = 0.0;
	if (cal->has_hum) {
		double h = t_fine - 76800.0;

		h = (adc_h - (cal->h4 * 64.0 + cal->h5 / 16384.0 * h)) *
		    (cal->h2 / 65536.0 * (1.0 + cal->h6 / 67108864.0 * h * (1.0 + cal->h3 / 67108864.0 * h)));
		h = h * (1.0 - cal->h1 * h / 524288.0);
		out->rh = h > 100.0 ? 100.0 : (h < 0.0 ? 0.0 : h);
	}
	/* 센서 범위: 300~1100 hPa, −40~85 °C */
	if (out->p_pa < 30000.0 || out->p_pa > 110000.0 || out->t_c < -40.0 || out->t_c > 85.0) {
		return SENS_E_RANGE;
	}
	return SENS_OK;
}
