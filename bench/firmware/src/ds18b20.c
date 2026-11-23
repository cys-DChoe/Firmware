/* DS18B20 드라이버 (1-Wire 를 GPIO 로 직접) — ds18b20.h 참고 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>

#include "ds18b20.h"

#define CMD_READ_ROM   0x33
#define CMD_SKIP_ROM   0xCC
#define CMD_CONVERT    0x44
#define CMD_READ_SPAD  0xBE

/* 선을 낮춤(0) / 놓음(1). open-drain 이라 1 은 풀업이 올림 */
static inline void line(const ds18b20_t *s, int v)
{
	gpio_pin_set_raw(s->port, s->pin, v);
}

static inline int sense(const ds18b20_t *s)
{
	gpio_port_value_t v = 0;

	(void)gpio_port_get_raw(s->port, &v);
	return (int)((v >> s->pin) & 1u);
}

/* 리셋: 480 µs 낮춤 → 놓고 70 µs 뒤 센서가 선을 낮추고 있으면(=0) 있음 */
static bool reset(const ds18b20_t *s)
{
	if (!sense(s)) {
		return false; /* 쉬는 선이 낮음: 풀업이 없거나 선이 GND 에 붙음 */
	}
	line(s, 0);
	k_busy_wait(480);
	unsigned int key = irq_lock();

	line(s, 1);
	k_busy_wait(70);
	int present = !sense(s);

	irq_unlock(key);
	k_busy_wait(410);
	return present && sense(s); /* 끝나면 선이 다시 올라와 있어야 함 */
}

static void write_bit(const ds18b20_t *s, int b)
{
	unsigned int key = irq_lock();

	line(s, 0);
	k_busy_wait(b ? 6 : 60);
	line(s, 1);
	k_busy_wait(b ? 64 : 10);
	irq_unlock(key);
}

static int read_bit(const ds18b20_t *s)
{
	unsigned int key = irq_lock();

	line(s, 0);
	k_busy_wait(3);
	line(s, 1);
	k_busy_wait(9);
	int b = sense(s);

	k_busy_wait(55);
	irq_unlock(key);
	return b;
}

static void write_byte(const ds18b20_t *s, uint8_t v)
{
	for (int i = 0; i < 8; i++) {
		write_bit(s, (v >> i) & 1);
	}
}

static uint8_t read_byte(const ds18b20_t *s)
{
	uint8_t v = 0;

	for (int i = 0; i < 8; i++) {
		v |= (uint8_t)(read_bit(s) << i);
	}
	return v;
}

/* Maxim CRC8: 다항식 x^8 + x^5 + x^4 + 1 (뒤집은 값 0x8C), 시작값 0 */
static uint8_t crc8(const uint8_t *d, size_t n)
{
	uint8_t c = 0;

	for (size_t i = 0; i < n; i++) {
		uint8_t in = d[i];

		for (int b = 0; b < 8; b++) {
			uint8_t mix = (c ^ in) & 1u;

			c >>= 1;
			if (mix) {
				c ^= 0x8C;
			}
			in >>= 1;
		}
	}
	return c;
}

static bool all_same(const uint8_t *d, size_t n, uint8_t v)
{
	for (size_t i = 0; i < n; i++) {
		if (d[i] != v) {
			return false;
		}
	}
	return true;
}

sens_err_t ds18b20_start(ds18b20_t *s, const struct device *port, gpio_pin_t pin, uint8_t rom[8])
{
	s->port = port;
	s->pin = pin;
	s->family = 0;
	if (!device_is_ready(port) ||
	    gpio_pin_configure(port, pin, GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN | GPIO_PULL_UP) != 0) {
		return SENS_E_BUS;
	}
	k_busy_wait(500);
	if (!reset(s)) {
		return SENS_E_BUS;
	}
	write_byte(s, CMD_READ_ROM);
	for (int i = 0; i < 8; i++) {
		rom[i] = read_byte(s);
	}
	if (all_same(rom, 8, 0xFF) || all_same(rom, 8, 0x00)) {
		return SENS_E_BUS;
	}
	if (crc8(rom, 7) != rom[7]) {
		return SENS_E_STATUS; /* 센서가 둘 이상이면 주소가 섞여 CRC 가 틀림 */
	}
	s->family = rom[0];
	return SENS_OK;
}

sens_err_t ds18b20_convert(const ds18b20_t *s)
{
	if (!reset(s)) {
		return SENS_E_BUS;
	}
	write_byte(s, CMD_SKIP_ROM);
	write_byte(s, CMD_CONVERT);
	return SENS_OK;
}

sens_err_t ds18b20_read(const ds18b20_t *s, double *t_c)
{
	uint8_t d[9];

	if (!reset(s)) {
		return SENS_E_BUS;
	}
	write_byte(s, CMD_SKIP_ROM);
	write_byte(s, CMD_READ_SPAD);
	for (int i = 0; i < 9; i++) {
		d[i] = read_byte(s);
	}
	if (all_same(d, 9, 0xFF) || all_same(d, 9, 0x00)) {
		return SENS_E_BUS;
	}
	if (crc8(d, 8) != d[8]) {
		return SENS_E_STATUS;
	}
	int16_t raw = (int16_t)(d[0] | (d[1] << 8));

	*t_c = s->family == 0x10 ? raw / 2.0 : raw / 16.0;
	if (s->family != 0x10 && raw == 0x0550) {
		return SENS_STALE; /* 85.0 °C = 켜진 직후 기본값 */
	}
	return SENS_OK;
}
