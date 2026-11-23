/* DS18B20 온도 센서 (1-Wire, 선 3개: 전원 · GND · 데이터) — 관 속 가스 온도용
 *
 * 데이터 선 하나를 GPIO 로 직접 흔들어 읽음 (open-drain, 풀업 4.7 kΩ 는 어댑터 보드에 있음).
 * 변환(12 비트)에 최대 750 ms 가 걸려서 "변환 시작" 과 "결과 읽기" 를 나눠 부름:
 *   1초마다  ds18b20_read() 로 지난번 변환 결과를 읽고 → ds18b20_convert() 로 다음 변환을 걺.
 * 버스에 센서가 하나뿐이라고 보고 주소 없이(Skip ROM) 말함.
 * 비트 하나(약 70 µs) 동안만 인터럽트를 막음. 한 번 읽는 데 약 8 ms.
 * 근거: Maxim DS18B20 데이터시트 (19-7487 Rev 6) — 타이밍 · 스크래치패드 · CRC.
 */
#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "i2c_bus.h" /* sens_err_t */

typedef struct {
	const struct device *port;
	gpio_pin_t pin;
	uint8_t family; /* ROM 첫 바이트: 0x28 = DS18B20, 0x22 = DS1822, 0x10 = DS18S20(눈금 0.5 °C) */
} ds18b20_t;

/* 핀 설정 + 센서 찾기. rom 에 8 바이트 주소를 채움. 응답 없으면 SENS_E_BUS, CRC 틀리면 SENS_E_STATUS */
sens_err_t ds18b20_start(ds18b20_t *s, const struct device *port, gpio_pin_t pin, uint8_t rom[8]);

/* 온도 변환 시작 (끝나기까지 최대 750 ms) */
sens_err_t ds18b20_convert(const ds18b20_t *s);

/* 지난 변환 결과 (°C). 응답 없으면 SENS_E_BUS, CRC 틀리면 SENS_E_STATUS,
 * 켜진 직후 값(85 °C) 그대로면 SENS_STALE (변환이 안 된 것. 센서 전원이 잠깐 끊겼을 때도 이 값) */
sens_err_t ds18b20_read(const ds18b20_t *s, double *t_c);
