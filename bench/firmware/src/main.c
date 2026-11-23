/*
 * 브레드보드 센서 확인 펌웨어 (NUCLEO-H563ZI)
 *
 * 1) 켜지면 I2C1(HSC) · I2C4(ABP2) 를 훑어 응답하는 주소를 찍음 (둘 다 0x28 이 보이면 배선 성공)
 * 2) 그다음 CONFIG_BENCH_SAMPLE_PERIOD_MS(지금 100 ms)마다 HSC(차압) · ABP2(정압) · SDP810(차압)을 읽어 찍음
 * 3) CONFIG_BENCH_SLOW_PERIOD_MS(1초)마다 BME280(대기압 · 주변 온도 · 습도, I2C4 0x76)과
 *    DS18B20(관 속 가스 온도, D2 = PG14)을 읽고, 그 값을 측정 줄 끝에 줄마다 붙임
 *
 * 센서 드라이버(hsc.c · abp2.c)는 제품 저장소 것을 그대로 쓰고,
 * 여기서는 그 드라이버가 쓰는 i2c_bus_t 를 Zephyr I2C 로 채워 주기만 함.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <stm32_ll_gpio.h>
#include <stdarg.h>
#include <zephyr/sys/printk.h>

#include "i2c_bus.h"
#include "hsc.h"
#include "abp2.h"
#include "bench_net.h"
#include "sdp810.h"
#include "bme280.h"
#include "ds18b20.h"

static const struct device *const i2c_hsc = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct device *const i2c_abp2 = DEVICE_DT_GET(DT_NODELABEL(i2c4));

/* ---- 제품 드라이버용 버스 어댑터 ---- */

static int z_write(void *ctx, uint8_t addr, const uint8_t *d, size_t n)
{
	return i2c_write((const struct device *)ctx, d, n, addr) == 0 ? 0 : -1;
}

static int z_read(void *ctx, uint8_t addr, uint8_t *d, size_t n)
{
	return i2c_read((const struct device *)ctx, d, n, addr) == 0 ? 0 : -1;
}

static int z_write_read(void *ctx, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	return i2c_write_read((const struct device *)ctx, addr, w, wn, r, rn) == 0 ? 0 : -1;
}

static void z_delay(void *ctx, uint32_t ms)
{
	ARG_UNUSED(ctx);
	k_msleep(ms);
}

static i2c_bus_t bus_of(const struct device *dev)
{
	return (i2c_bus_t){
		.write = z_write, .read = z_read, .write_read = z_write_read,
		.delay_ms = z_delay, .ctx = (void *)dev,
	};
}

/* 시리얼에 찍고, MQTT 로도 보냄 (측정 줄 · 영점 안내처럼 페이지가 쓰는 줄만) */
static void say(const char *fmt, ...)
{
	char line[320]; /* bench_net.c 의 LINE_MAX 와 같게 */
	va_list ap;

	va_start(ap, fmt);
	vsnprintk(line, sizeof(line), fmt, ap);
	va_end(ap);
	printk("%s\n", line);
	if (IS_ENABLED(CONFIG_BENCH_MQTT)) {
		bench_net_send(line);
	}
}

static const char *err_name(sens_err_t e)
{
	switch (e) {
	case SENS_OK: return "정상";
	case SENS_STALE: return "이전 값";
	case SENS_E_BUS: return "응답 없음";
	case SENS_E_ID: return "칩 번호 다름";
	case SENS_E_BUSY: return "변환 중";
	case SENS_E_STATUS: return "센서 오류 비트";
	case SENS_E_RANGE: return "범위 밖";
	default: return "?";
	}
}

/* ---- 선 상태: 풀업이 있으면 쉬는 동안 둘 다 높음(1) 이어야 함 ---- */

/* 배선 확인: 안쪽 풀다운(약 40 kΩ)을 잠깐 걸고 읽음.
 *   1 → 바깥 10 kΩ 풀업이 이 핀까지 닿아 있음 (선이 맞는 구멍에 꽂힘)
 *   0 → 이 핀에 풀업이 안 닿음 (선이 다른 구멍이거나 빠짐)
 * 끝나면 I2C 용 설정(대체 기능 · 안쪽 풀업)으로 되돌림. 대체 기능 번호와 open-drain 은
 * 입력으로 바꿔도 레지스터에 그대로 남아 있어서 모드와 풀업만 되돌리면 됨. */
static void wiring_probe(const char *name, const struct device *port, GPIO_TypeDef *regs, int scl,
			 int sda)
{
	gpio_port_value_t v = 0;

	gpio_pin_configure(port, scl, GPIO_INPUT | GPIO_PULL_DOWN);
	gpio_pin_configure(port, sda, GPIO_INPUT | GPIO_PULL_DOWN);
	k_msleep(5);
	if (gpio_port_get_raw(port, &v) == 0) {
		printk("[배선] %s: 풀다운 걸고 SCL=%d SDA=%d  (1 = 풀업 선이 핀에 닿음, 0 = 안 닿음)\n",
		       name, (int)((v >> scl) & 1), (int)((v >> sda) & 1));
	}
	LL_GPIO_SetPinMode(regs, BIT(scl), LL_GPIO_MODE_ALTERNATE);
	LL_GPIO_SetPinMode(regs, BIT(sda), LL_GPIO_MODE_ALTERNATE);
	LL_GPIO_SetPinPull(regs, BIT(scl), LL_GPIO_PULL_UP);
	LL_GPIO_SetPinPull(regs, BIT(sda), LL_GPIO_PULL_UP);
	k_msleep(2);
}

/* 선 찾기: 모든 GPIO 핀을 잠깐 입력 + 안쪽 풀다운으로 바꿔 읽고 바로 되돌림.
 * 1 로 읽히는 핀 = 바깥 풀업(브레드보드 10 kΩ 등)이 닿아 있는 핀. 신호선이 실제로 꽂힌 핀을 찾는 용도.
 * 디버그(PA13/PA14), 로그 UART(PD8/PD9), 클럭(PC14/PC15/PH0/PH1)은 건드리지 않음. */
static void find_pullups(void)
{
	GPIO_TypeDef *const ports[] = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOI};
	const char names[] = "ABCDEFGHI";

	printk("[찾기] 바깥 풀업이 닿은 핀:");
	for (int p = 0; p < ARRAY_SIZE(ports); p++) {
		GPIO_TypeDef *g = ports[p];

		for (int pin = 0; pin < 16; pin++) {
			char c = names[p];

			if ((c == 'A' && (pin == 13 || pin == 14)) || (c == 'D' && (pin == 8 || pin == 9)) ||
			    (c == 'C' && (pin == 14 || pin == 15)) || (c == 'H' && (pin == 0 || pin == 1))) {
				continue;
			}
			uint32_t m = g->MODER, pu = g->PUPDR, sh = 2u * pin;

			g->PUPDR = (pu & ~(3u << sh)) | (2u << sh);
			g->MODER = m & ~(3u << sh);
			k_busy_wait(50);
			uint32_t hi = (g->IDR >> pin) & 1u;

			g->MODER = m;
			g->PUPDR = pu;
			if (hi) {
				printk(" P%c%d", c, pin);
			}
		}
	}
	printk("\n");
}

/* 버스 복구: 센서가 통신 도중 멈춰 SDA 를 붙잡고 있으면, SCL 을 최대 16번 쳐서 놓게 하고 STOP 을 보냄.
 * 핀을 잠깐 GPIO open-drain 출력으로 쓰고, 끝나면 I2C 대체 기능으로 되돌림. 끝난 뒤 SDA 값을 돌려줌. */
static int bus_recover(const char *name, GPIO_TypeDef *g, int scl, int sda)
{
	const uint32_t ms = 3u << (2u * scl), md = 3u << (2u * sda);
	uint32_t moder = g->MODER;
	int clocks = 0;

	g->OTYPER |= BIT(scl) | BIT(sda);
	g->BSRR = BIT(scl) | BIT(sda);
	g->MODER = (moder & ~(ms | md)) | (1u << (2u * scl)) | (1u << (2u * sda));
	k_busy_wait(100);
	while (clocks < 16 && !((g->IDR >> sda) & 1u)) {
		g->BSRR = BIT(scl) << 16;
		k_busy_wait(60);
		g->BSRR = BIT(scl);
		k_busy_wait(60);
		clocks++;
	}
	/* STOP: SCL 높음에서 SDA 를 낮음 → 높음 */
	g->BSRR = BIT(scl) << 16;
	k_busy_wait(60);
	g->BSRR = BIT(sda) << 16;
	k_busy_wait(60);
	g->BSRR = BIT(scl);
	k_busy_wait(60);
	g->BSRR = BIT(sda);
	k_busy_wait(100);
	int sda_now = (g->IDR >> sda) & 1u;

	g->MODER = (g->MODER & ~(ms | md)) | (2u << (2u * scl)) | (2u << (2u * sda));
	printk("[복구] %s: SCL 클럭 %d번 보냄 → SDA=%d (%s)\n", name, clocks, sda_now,
	       sda_now ? "풀림" : "여전히 0. 센서가 아니라 배선이 GND 로 당기는 것");
	return sda_now;
}

static bool lines_ok(const char *name, const struct device *port, int scl, int sda)
{
	gpio_port_value_t v = 0;

	if (gpio_port_get_raw(port, &v) != 0) {
		printk("[선]   %s: 포트 못 읽음\n", name);
		return true;
	}
	int l_scl = (v >> scl) & 1, l_sda = (v >> sda) & 1;

	printk("[선]   %s: SCL=%d SDA=%d", name, l_scl, l_sda);
	if (l_scl && l_sda) {
		printk("  정상\n");
		return true;
	}
	printk("  <- 0 인 선이 있음. 풀업 저항(3V3 쪽)이 빠졌거나, 선이 GND 에 붙었거나, 센서 전원이 없음\n");

	/* 원인 가르기: MCU 안쪽 풀업(약 40 kΩ)을 잠깐 켜고 다시 읽음.
	 *   1 이 되면 → 바깥 풀업 길이 끊김 (저항 · 아래 + 줄 다리 선 · 신호선이 안 닿음)
	 *   0 그대로면 → 무언가가 선을 GND 쪽으로 당김 (센서 전원 없음 · 합선 · 3V3 대신 GND 에 꽂힌 저항)
	 * 이 버스는 이후에 쓰지 않으므로 핀 설정을 되돌리지 않음. */
	gpio_pin_configure(port, scl, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure(port, sda, GPIO_INPUT | GPIO_PULL_UP);
	k_msleep(5);
	if (gpio_port_get_raw(port, &v) == 0) {
		int p_scl = (v >> scl) & 1, p_sda = (v >> sda) & 1;

		printk("[진단] %s: 안쪽 풀업 켜면 SCL=%d SDA=%d\n", name, p_scl, p_sda);
		printk("[진단] 1 인 선 = 바깥 풀업 길이 끊김(저항, 아래 + 줄 다리 선, 신호선 자리)\n");
		printk("[진단] 0 인 선 = 무언가가 GND 쪽으로 당김(센서 전원 없음, 합선)\n");
	}
	return false;
}

/* ---- 버스 훑기 ---- */

static int scan(const char *name, const struct device *dev)
{
	uint8_t b;
	int found = 0;
	char addrs[96] = "";
	size_t len = 0;

	if (!device_is_ready(dev)) {
		say("[스캔] %s: 장치 준비 안 됨", name);
		return 0;
	}
	for (uint8_t a = 0x08; a < 0x78; a++) {
		if (i2c_read(dev, &b, 1, a) == 0) {
			if (len < sizeof(addrs) - 6) {
				len += snprintk(addrs + len, sizeof(addrs) - len, " 0x%02x", a);
			}
			found++;
		}
	}
	/* MQTT 로도 가서 USB 없이도 보드 로그(board_serial.log)에서 배선 확인 가능 */
	if (found) {
		say("[스캔] %s:%s  (%d개)", name, addrs, found);
	} else {
		say("[스캔] %s: 없음", name);
	}
	return found;
}

/* ---- HSC 차압 영점 ----
 * 포트 둘 다 열려 있을 때(실제 차압 0) 읽은 평균이 센서의 영점 오차임. 그만큼 빼서 찍음.
 * 켜질 때 한 번 잡고, 파란 USER 버튼(B1)을 누르면 다시 잡음.
 */
#define HSC_FAST_MAX 20 /* 측정 줄 하나에 모아 보낼 HSC 값 최대 개수 (100 ms, 1초 주기면 10개) */
#define HSC_ZERO_MAX_PA 50.0 /* 이보다 크면 포트에 압력이 걸린 걸로 보고 거절 (만점 폭의 2.5 %) */

static const struct gpio_dt_spec user_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback user_btn_cb;
static atomic_t rezero_req;

static void user_btn_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	atomic_set(&rezero_req, 1);
}

static void user_btn_init(void)
{
	if (!gpio_is_ready_dt(&user_btn) || gpio_pin_configure_dt(&user_btn, GPIO_INPUT) != 0 ||
	    gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		printk("USER 버튼 설정 실패: 영점은 켜질 때만 잡음\n");
		return;
	}
	gpio_init_callback(&user_btn_cb, user_btn_pressed, BIT(user_btn.pin));
	gpio_add_callback(user_btn.port, &user_btn_cb);
}

/* 찍는 HSC 차압 = 기울기 × (읽은 값 − 영점) + 오프셋. 기울기 · 오프셋은 SDP810 기준으로 맞춘 값 (Kconfig) */
static double hsc_cal(double raw_pa, double zero_pa)
{
	return (raw_pa - zero_pa) * (CONFIG_BENCH_HSC_GAIN_PPM / 1e6) + CONFIG_BENCH_HSC_OFFSET_MPA / 1000.0;
}

#define HSC_TRACK_BAND_PA 1.0 /* SDP810 이 이 안쪽일 때만 '흐름 없음' 으로 보고 HSC 0점을 맞춤 */

/* HSC 를 CONFIG_BENCH_HSC_OVERSAMPLE 번 읽어 평균 (눈금 한 칸 0.15 Pa 씩 튀는 흔들림을 줄임).
 * 새 값(SENS_OK)만 평균에 넣고, 새 값이 하나도 없으면 마지막 결과를 그대로 돌려줌. */
static sens_err_t hsc_read_avg(const i2c_bus_t *bus, const hsc_cfg_t *cfg, hsc_reading_t *out)
{
	double sum = 0.0;
	int got = 0;
	sens_err_t last = SENS_E_BUS;

	for (int i = 0; i < CONFIG_BENCH_HSC_OVERSAMPLE; i++) {
		hsc_reading_t h = {0};

		if (i > 0) {
			k_msleep(CONFIG_BENCH_HSC_OVERSAMPLE_GAP_MS);
		}
		last = hsc_read(bus, cfg, &h);
		if (last == SENS_OK) {
			sum += h.p_pa;
			got++;
			*out = h;
		} else if (last == SENS_STALE && got == 0) {
			*out = h;
		}
	}
	if (got == 0) {
		return last;
	}
	out->p_pa = sum / got;
	return SENS_OK;
}

/* 20 ms 간격으로 여러 번 읽어 평균 (HSC 는 스스로 계속 재고 있어, 너무 빨리 읽으면 같은 값이 옴) */
static bool hsc_take_zero(const i2c_bus_t *bus, const hsc_cfg_t *cfg, double *zero_pa)
{
	double sum = 0.0;
	int got = 0;

	for (int i = 0; i < CONFIG_BENCH_HSC_ZERO_SAMPLES; i++) {
		hsc_reading_t h;

		if (hsc_read(bus, cfg, &h) == SENS_OK) {
			sum += h.p_pa;
			got++;
		}
		k_msleep(20);
	}
	if (got < CONFIG_BENCH_HSC_ZERO_SAMPLES / 2) {
		say("HSC 영점 못 잡음: %d/%d 번만 읽힘", got, CONFIG_BENCH_HSC_ZERO_SAMPLES);
		return false;
	}
	double z = sum / got;

	if (z > HSC_ZERO_MAX_PA || z < -HSC_ZERO_MAX_PA) {
		say("HSC 영점 거절: 평균 %.2f Pa (±%.0f Pa 넘음, 포트 둘 다 열고 다시 USER 버튼)", z,
		    HSC_ZERO_MAX_PA);
		return false;
	}
	*zero_pa = z;
	say("HSC 영점 %.2f Pa (%d번 평균). 이제 찍는 HSC 차압은 이만큼 뺀 값. "
	    "다시 잡으려면 포트 둘 다 열고 파란 USER 버튼.",
	    z, got);
	return true;
}

int main(void)
{
	i2c_bus_t hsc_bus = bus_of(i2c_hsc);
	i2c_bus_t abp2_bus = bus_of(i2c_abp2);
	hsc_cfg_t hcfg;
	abp2_cfg_t acfg;

	hsc_default_cfg(&hcfg);
	abp2_default_cfg(&acfg);
	acfg.p_max_pa = CONFIG_BENCH_ABP2_MAX_PA; /* 붙은 부품 범위와 같아야 함 (ABP2DRNT006KG = 6000) */

	k_msleep(100);
	printk("\n=== 브레드보드 센서 확인 ===\n");
	if (IS_ENABLED(CONFIG_BENCH_MQTT)) {
		bench_net_start();
	}
	printk("HSC  = I2C1 (CN7-2 SCL, CN7-4 SDA)\n");
	printk("ABP2 = I2C4 (CN10-21 SCL, CN10-19 SDA), 안쪽 풀업 + 느린 속도, 범위 0–%d Pa\n",
	       CONFIG_BENCH_ABP2_MAX_PA);
	printk("I2C4 TIMINGR=0x%08x (0xF021FFFF 면 약 14 kHz 적용됨)\n", (unsigned int)I2C4->TIMINGR);
	find_pullups();
	wiring_probe("I2C1 HSC ",DEVICE_DT_GET(DT_NODELABEL(gpiob)),
		     (GPIO_TypeDef *)DT_REG_ADDR(DT_NODELABEL(gpiob)), 8, 9);
	wiring_probe("I2C4 ABP2", DEVICE_DT_GET(DT_NODELABEL(gpiod)),
		     (GPIO_TypeDef *)DT_REG_ADDR(DT_NODELABEL(gpiod)), 12, 13);
	bool ok1 = lines_ok("I2C1 HSC ", DEVICE_DT_GET(DT_NODELABEL(gpiob)), 8, 9);
	bool ok2 = lines_ok("I2C4 ABP2", DEVICE_DT_GET(DT_NODELABEL(gpiod)), 12, 13);

	if (!ok2) {
		ok2 = bus_recover("I2C4 ABP2", (GPIO_TypeDef *)DT_REG_ADDR(DT_NODELABEL(gpiod)), 12,
				  13) != 0;
		if (ok2) {
			/* lines_ok 의 진단이 핀을 입력으로 바꿔 둔 상태라, 안쪽 풀업을 다시 켬 */
			LL_GPIO_SetPinPull((GPIO_TypeDef *)DT_REG_ADDR(DT_NODELABEL(gpiod)), BIT(12),
					   LL_GPIO_PULL_UP);
			LL_GPIO_SetPinPull((GPIO_TypeDef *)DT_REG_ADDR(DT_NODELABEL(gpiod)), BIT(13),
					   LL_GPIO_PULL_UP);
		}
	}

	if (ok1) {
		scan("I2C1 HSC ", i2c_hsc);
	}
	if (ok2) {
		scan("I2C4 ABP2", i2c_abp2);
	}
	if (!ok1 || !ok2) {
		printk("선이 막힌 버스는 건너뜀 (막힌 버스를 건드리면 멈춤). 배선 고친 뒤 리셋 버튼 누르기.\n");
	}
	const uint32_t period_ms = CONFIG_BENCH_SAMPLE_PERIOD_MS;

	printk("둘 다 0x28 이 보이면 배선 성공. 이제 %u ms 마다 읽음 (CONFIG_BENCH_SAMPLE_PERIOD_MS).\n\n",
	       period_ms);

	double hsc_zero_pa = 0.0;

	if (CONFIG_BENCH_HSC_GAIN_PPM != 1000000 || CONFIG_BENCH_HSC_OFFSET_MPA != 0) {
		say("HSC 보정: 기울기 %.6f, 오프셋 %.3f Pa (SDP810 기준)", CONFIG_BENCH_HSC_GAIN_PPM / 1e6,
		    CONFIG_BENCH_HSC_OFFSET_MPA / 1000.0);
	}

	if (IS_ENABLED(CONFIG_BENCH_HSC_ZERO) && ok1) {
		user_btn_init();
		hsc_take_zero(&hsc_bus, &hcfg, &hsc_zero_pa);
	}

	/* SDP810: HSC 와 같은 I2C1 (주소 0x25 라 안 겹침). 안 붙어 있으면 5초마다 다시 찾음 */
	bool sdp_on = false;

	if (IS_ENABLED(CONFIG_BENCH_SDP810) && ok1) {
		sdp_on = sdp810_start(&hsc_bus) == SENS_OK;
		say(sdp_on ? "SDP810 (I2C1 0x25) 연속 측정 시작"
			   : "SDP810 (I2C1 0x25) 응답 없음: 안 붙어 있으면 정상, 5초마다 다시 찾음");
	}

	/* BME280: 정압 센서와 같은 I2C4 (주소 0x76 이라 안 겹침). 대기압 → 절대압 = 대기압 + ABP2 정압 */
	bme280_cal_t bme_cal = {0};
	bme280_reading_t bme_v = {0};
	bool bme_on = false;
	sens_err_t bme_e = SENS_E_BUS;

	if (IS_ENABLED(CONFIG_BENCH_BME280) && ok2) {
		sens_err_t e = bme280_start(&abp2_bus, &bme_cal);

		bme_on = e == SENS_OK;
		bme_e = bme_on ? SENS_E_BUSY : e; /* 첫 값이 나올 때까지 "변환 중" */
		say(bme_on ? "%s (I2C4 0x76) 연속 측정 시작"
			   : "BME280 (I2C4 0x76) %s: 안 붙어 있으면 정상, 5초마다 다시 찾음",
		    bme_on ? (bme_cal.has_hum ? "BME280" : "BMP280(습도 없음)") : err_name(e));
	}

	/* DS18B20: 1-Wire, 데이터 선 = D2 (PG14). 관 속 가스 온도 */
	ds18b20_t ds = {0};
	bool ds_on = false, ds_conv = false; /* ds_conv: 변환을 걸어 둠 → 750 ms 지나면 읽을 값이 있음 */
	int64_t ds_conv_at = 0;
	sens_err_t ds_e = SENS_E_BUS;
	double ds_c = 0.0;

	if (IS_ENABLED(CONFIG_BENCH_DS18B20)) {
		uint8_t rom[8] = {0};
		sens_err_t e = ds18b20_start(&ds, DEVICE_DT_GET(DT_NODELABEL(gpiog)), 14, rom);

		ds_on = e == SENS_OK;
		ds_e = ds_on ? SENS_E_BUSY : e;
		if (ds_on) {
			say("DS18B20 (D2 = PG14) 찾음: 주소 %02x-%02x%02x%02x%02x%02x%02x%s", rom[0], rom[6], rom[5],
			    rom[4], rom[3], rom[2], rom[1],
			    rom[0] == 0x28 ? "" : rom[0] == 0x10 ? " (DS18S20)" : " (DS18B20 이 아닌 종류)");
			ds_conv = ds18b20_convert(&ds) == SENS_OK;
			ds_conv_at = k_uptime_get();
		} else {
			say("DS18B20 (D2 = PG14) %s: 안 붙어 있으면 정상, 5초마다 다시 찾음", err_name(e));
		}
	}
	/* 느린 센서 둘은 slow_n 줄에 한 번만 읽음 (서로 반 박자 어긋나게 해서 한 줄이 너무 길어지지 않게) */
	const uint32_t slow_n = MAX(1u, CONFIG_BENCH_SLOW_PERIOD_MS / period_ms);
	const uint32_t retry_n = MAX(1u, 5000u / period_ms);

	int64_t next = k_uptime_get();
	/* 측정 줄 사이에 CONFIG_BENCH_HSC_FAST_MS 마다 읽은 HSC 값 (다음 측정 줄 때 한꺼번에 보냄) */
	double fast_v[HSC_FAST_MAX];
	bool fast_ok[HSC_FAST_MAX];
	int fast_n = 0;
	double fast_t = 0.0;
	double hsc_smooth = 0.0; /* 찍는 HSC 값의 이동 평균 */
	bool hsc_smooth_on = false;

	for (uint32_t n = 1;; n++) {
		if (IS_ENABLED(CONFIG_BENCH_HSC_ZERO) && ok1 && atomic_cas(&rezero_req, 1, 0)) {
			hsc_take_zero(&hsc_bus, &hcfg, &hsc_zero_pa);
			next = k_uptime_get();
			fast_n = 0; /* 영점 잡는 동안 모인 값은 버림 */
			hsc_smooth_on = false;
		}
		hsc_reading_t h = {0};
		abp2_reading_t p = {0};
		uint32_t t0 = k_cycle_get_32();
		sens_err_t he = ok1 ? hsc_read_avg(&hsc_bus, &hcfg, &h) : SENS_E_BUS;
		sens_err_t pe = ok2 ? abp2_measure(&abp2_bus, &acfg, &p) : SENS_E_BUS;
		char ss[64] = "";
		bool sdp_ok = false; /* 이번 줄의 SDP810 값 (HSC 0점을 맞추는 기준) */
		double sdp_pa = 0.0;

		if (IS_ENABLED(CONFIG_BENCH_SDP810) && ok1) {
			if (!sdp_on && n % 5 == 0) {
				sdp_on = sdp810_start(&hsc_bus) == SENS_OK;
				if (sdp_on) {
					say("SDP810 (I2C1 0x25) 찾음, 연속 측정 시작");
				}
			}
			sdp810_reading_t s = {0};
			sens_err_t se = sdp_on ? sdp810_read(&hsc_bus, &s) : SENS_E_BUS;

			if (se == SENS_E_BUS) {
				sdp_on = false;
			}
			if (se == SENS_OK) {
				sdp_ok = true;
				sdp_pa = s.p_pa;
			}
			if (se == SENS_OK || se == SENS_E_RANGE) {
				snprintk(ss, sizeof(ss), "  |  SDP810 %s %.2f Pa %.1f C", err_name(se), s.p_pa,
					 s.t_c);
			} else {
				snprintk(ss, sizeof(ss), "  |  SDP810 %s", err_name(se));
			}
		}
		uint32_t read_us = (uint32_t)k_cyc_to_us_floor64(k_cycle_get_32() - t0);

		/* 느린 센서 (read_us 에는 안 넣음: 위 세 센서를 읽는 시간만 봄) */
		char es[112] = "";
		size_t el = 0;

		if (IS_ENABLED(CONFIG_BENCH_BME280) && ok2) {
			if (!bme_on && n % retry_n == 0) {
				bme_on = bme280_start(&abp2_bus, &bme_cal) == SENS_OK;
				if (bme_on) {
					say("BME280 (I2C4 0x76) 찾음, 연속 측정 시작");
					bme_e = SENS_E_BUSY;
				}
			} else if (bme_on && n % slow_n == 0) {
				bme_e = bme280_read(&abp2_bus, &bme_cal, &bme_v);
				if (bme_e == SENS_E_BUS || (bme_e == SENS_E_BUSY && n > 3 * slow_n)) {
					bme_on = false; /* 빠졌거나, 전원이 끊겼다 돌아와 잠든 상태 → 다시 찾아 설정 */
				}
			}
			if (bme_e == SENS_OK || bme_e == SENS_E_RANGE) {
				el += snprintk(es + el, sizeof(es) - el, "  |  BME280 %s %.1f Pa %.2f C", err_name(bme_e),
					       bme_v.p_pa, bme_v.t_c);
				if (bme_cal.has_hum) {
					el += snprintk(es + el, sizeof(es) - el, " %.1f %%", bme_v.rh);
				}
			} else {
				el += snprintk(es + el, sizeof(es) - el, "  |  BME280 %s", err_name(bme_e));
			}
		}
		if (IS_ENABLED(CONFIG_BENCH_DS18B20)) {
			if (!ds_on && n % retry_n == retry_n / 2) {
				uint8_t rom[8];

				ds_on = ds18b20_start(&ds, DEVICE_DT_GET(DT_NODELABEL(gpiog)), 14, rom) == SENS_OK;
				if (ds_on) {
					say("DS18B20 (D2 = PG14) 찾음, 측정 시작");
					ds_e = SENS_E_BUSY;
					ds_conv = ds18b20_convert(&ds) == SENS_OK;
					ds_conv_at = k_uptime_get();
				}
			} else if (ds_on && n % slow_n == slow_n / 2 &&
				   !(ds_conv && k_uptime_get() - ds_conv_at < 750)) { /* 변환이 덜 끝났으면 다음 차례에 */
				if (ds_conv) {
					ds_e = ds18b20_read(&ds, &ds_c);
				}
				ds_conv = ds18b20_convert(&ds) == SENS_OK;
				ds_conv_at = k_uptime_get();
				if (ds_e == SENS_E_BUS || !ds_conv) {
					ds_on = false;
					ds_e = SENS_E_BUS;
				}
			}
			if (el < sizeof(es)) {
				if (ds_e == SENS_OK || ds_e == SENS_STALE) {
					snprintk(es + el, sizeof(es) - el, "  |  DS18B20 %s %.2f C", err_name(ds_e), ds_c);
				} else {
					snprintk(es + el, sizeof(es) - el, "  |  DS18B20 %s", err_name(ds_e));
				}
			}
		}

		/* 맨 앞의 [...] 는 페이지와 MCP 서버가 읽어 갱신 간격을 맞추는 데 씀 */
		char hs[40] = "", ps[48] = "";

		/* HSC: 그 사이 CONFIG_BENCH_HSC_FAST_MS 마다 읽어 둔 값 + 방금 값. 측정 줄에는 평균을 찍고,
		 * 값 하나하나는 바로 뒤 "[hsc 100 ms] #n v1 v2 ..." 줄로 보냄 (못 읽은 칸은 x) */
		bool h_ok = he == SENS_OK || he == SENS_STALE;

		if (fast_n < HSC_FAST_MAX) {
			fast_v[fast_n] = h.p_pa;
			fast_ok[fast_n] = h_ok;
			fast_n++;
		}
		if (h_ok) {
			fast_t = h.t_c;
		}
		double sum = 0.0;
		int cnt = 0;

		for (int i = 0; i < fast_n; i++) {
			if (fast_ok[i]) {
				sum += fast_v[i];
				cnt++;
			}
		}
		if (cnt > 0) {
			if (!h_ok) {
				he = SENS_OK; /* 이번만 실패했고 그 사이 값은 있음 */
			}
			double hv = hsc_cal(sum / cnt, hsc_zero_pa);

			/* 부드럽게 하기: 최근 값들의 이동 평균(시정수 CONFIG_BENCH_HSC_SMOOTH_TAU_MS).
			 * 새 값이 지금 평균과 CONFIG_BENCH_HSC_SMOOTH_JUMP_MPA 넘게 다르면 진짜 압력 변화로 보고
			 * 평균을 버리고 바로 그 값으로 감 (큰 변화에는 늦지 않게). */
			double shown = hv;

			if (CONFIG_BENCH_HSC_SMOOTH_TAU_MS > 0) {
				double d = hv - hsc_smooth;

				if (!hsc_smooth_on || d > CONFIG_BENCH_HSC_SMOOTH_JUMP_MPA / 1000.0 ||
				    d < -CONFIG_BENCH_HSC_SMOOTH_JUMP_MPA / 1000.0) {
					hsc_smooth = hv;
					hsc_smooth_on = true;
				} else {
					double a = (double)period_ms / CONFIG_BENCH_HSC_SMOOTH_TAU_MS;

					hsc_smooth += d * (a > 1.0 ? 1.0 : a);
				}
				shown = hsc_smooth;
			}
			snprintk(hs, sizeof(hs), " %.2f Pa %.1f C", shown, fast_t);

			/* 흐름이 없을 때(SDP810 이 0 근처)만 HSC 0점을 SDP810 쪽으로 천천히 끌어감.
			 * HSC 0점은 켜진 뒤에도 온도에 따라 0.2 ~ 0.4 Pa 씩 흘러서, 그대로 두면 SDP810 과 어긋남.
			 * 두 값이 5 Pa 넘게 다르면 한쪽만 압력을 받는 중이라 건드리지 않음. */
			double diff = hv - sdp_pa;

			if (IS_ENABLED(CONFIG_BENCH_HSC_TRACK_SDP) && sdp_ok && h_ok &&
			    sdp_pa > -HSC_TRACK_BAND_PA && sdp_pa < HSC_TRACK_BAND_PA && diff > -5.0 && diff < 5.0) {
				double step = diff * ((double)period_ms / (CONFIG_BENCH_HSC_TRACK_TAU_S * 1000.0));

				hsc_zero_pa += step / (CONFIG_BENCH_HSC_GAIN_PPM / 1e6);
				hsc_smooth -= step; /* 0점이 움직인 만큼 평균도 같이 옮김 */
			}
		}
		if (pe == SENS_OK || pe == SENS_E_RANGE) {
			snprintk(ps, sizeof(ps), " %.1f Pa %.2f C (st 0x%02x)", p.p_pa, p.t_c, p.status);
		}
		say("[period %u ms, read %u us] #%u  HSC %s%s  |  ABP2 %s%s%s%s", period_ms, read_us, n,
		    err_name(he), hs, err_name(pe), ps, ss, es);

		if (CONFIG_BENCH_HSC_FAST_MS > 0 && ok1 && fast_n > 1) {
			char vl[200];
			int len = snprintk(vl, sizeof(vl), "[hsc %d ms] #%u ", CONFIG_BENCH_HSC_FAST_MS, n);

			for (int i = 0; i < fast_n && len < (int)sizeof(vl) - 10; i++) {
				len += fast_ok[i] ? snprintk(vl + len, sizeof(vl) - len, " %.2f",
							     hsc_cal(fast_v[i], hsc_zero_pa))
						  : snprintk(vl + len, sizeof(vl) - len, " x");
			}
			say("%s", vl);
		}
		fast_n = 0;

		/* 읽기 시작 시각 기준으로 일정한 간격 유지 (읽는 시간만큼 밀리지 않게) */
		int64_t t_start = next;

		next += period_ms;

		/* 다음 측정까지 HSC 만 CONFIG_BENCH_HSC_FAST_MS 마다 읽어 쌓아 둠 (다음 줄에서 평균) */
		if (CONFIG_BENCH_HSC_FAST_MS > 0 && ok1) {
			for (int64_t tick = t_start + CONFIG_BENCH_HSC_FAST_MS; tick < next;
			     tick += CONFIG_BENCH_HSC_FAST_MS) {
				int64_t w = tick - k_uptime_get();

				if (w > 0) {
					k_msleep((int32_t)w);
				}
				hsc_reading_t f = {0};
				sens_err_t fe = hsc_read(&hsc_bus, &hcfg, &f);

				if (fast_n < HSC_FAST_MAX) {
					fast_v[fast_n] = f.p_pa;
					fast_ok[fast_n] = fe == SENS_OK || fe == SENS_STALE;
					fast_n++;
				}
				if (fe == SENS_OK || fe == SENS_STALE) {
					fast_t = f.t_c;
				}
			}
		}
		int64_t wait = next - k_uptime_get();
		if (wait > 0) {
			k_msleep((int32_t)wait);
		} else {
			next = k_uptime_get();
		}
	}
	return 0;
}
