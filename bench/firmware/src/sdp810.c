/* Sensirion SDP810-500Pa 드라이버 — sdp810.h 참고 */
#include "sdp810.h"

#define CMD_START_AVG 0x3615 /* 연속 측정, 차압 온도 보정, 읽을 때까지 평균 */
#define CMD_STOP      0x3FF9 /* 연속 측정 멈춤 (멈춘 뒤 0.5 ms 이상 기다려야 다음 명령) */

/* 데이터시트 CRC: 다항식 0x31, 시작값 0xFF, 2 바이트마다 하나 */
static uint8_t crc8(const uint8_t *d, size_t n)
{
	uint8_t c = 0xFF;

	for (size_t i = 0; i < n; i++) {
		c ^= d[i];
		for (int b = 0; b < 8; b++) {
			c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
		}
	}
	return c;
}

static int cmd(const i2c_bus_t *bus, uint16_t code)
{
	const uint8_t w[2] = {(uint8_t)(code >> 8), (uint8_t)code};

	return bus->write(bus->ctx, SDP810_ADDR, w, sizeof(w));
}

sens_err_t sdp810_start(const i2c_bus_t *bus)
{
	(void)cmd(bus, CMD_STOP); /* 이미 멈춰 있으면 응답이 없을 수 있어 결과는 안 봄 */
	bus->delay_ms(bus->ctx, 2);
	if (cmd(bus, CMD_START_AVG) != 0) {
		return SENS_E_BUS;
	}
	bus->delay_ms(bus->ctx, 10); /* 첫 값이 나오기까지 몇 ms */
	return SENS_OK;
}

sens_err_t sdp810_read(const i2c_bus_t *bus, sdp810_reading_t *out)
{
	uint8_t r[9];

	if (bus->read(bus->ctx, SDP810_ADDR, r, sizeof(r)) != 0) {
		return SENS_E_BUS;
	}
	for (int i = 0; i < 9; i += 3) {
		if (crc8(&r[i], 2) != r[i + 2]) {
			return SENS_E_STATUS;
		}
	}
	int16_t dp = (int16_t)((r[0] << 8) | r[1]);
	int16_t t = (int16_t)((r[3] << 8) | r[4]);
	uint16_t sf = (uint16_t)((r[6] << 8) | r[7]);

	if (sf == 0) {
		return SENS_E_STATUS;
	}
	out->scale = sf;
	out->p_pa = (double)dp / sf;
	out->t_c = (double)t / 200.0;
	return (dp == INT16_MAX || dp == INT16_MIN) ? SENS_E_RANGE : SENS_OK;
}
