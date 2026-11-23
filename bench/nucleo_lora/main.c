/* Nucleo-H563ZI 브레드보드 LoRa 벤치 — 센서 값을 Wio-E5 mini 로 1분마다 보냄
 *
 * 배선(센서 브레드보드 배선 문서 · "LoRa 모듈 직결" 탭):
 *   Wio-E5 TX → D0(PB7) · D1(PB6) → Wio-E5 RX  = LPUART1 9600 (Core/Src/lora_uart_stm32.c 그대로)
 *   HSC 차압  I2C1 PB8 SCL · PB9 SDA 0x28
 *   ABP2 정압 I2C4 PD12 SCL · PD13 SDA 0x28
 *   BME280 기압 I2C4 0x76
 *   SDP810 차압 I2C1 0x25 (HSC 와 같은 버스)
 *   DS18B20 온도 1-Wire = D2(PG14)
 *   로그 USART3 PD8/PD9 = ST-LINK 가상 COM 포트(맥 /dev/cu.usbmodem…) 115200
 *
 * 흐름: 켬 → 센서 확인 → 모듈 AT 확인 → 가입(실패면 30 초 뒤 다시) → 1초마다 재기 · 10초마다 로그 → 60초마다 24바이트(report.h 형식 v1) 송신
 * 보고 칸: 정압 = ABP2, 평균 차압 = HSC 60초 평균(영점 안 뺌), 가스 온도 = ABP2 온도, 대기압 = BME280.
 *          누적 · 유량 · 시각 · 배터리는 벤치에 없어서 0 (배터리 칸은 범위 밑이라 CLIPPED 표시가 같이 켜짐).
 * 벤치 꼬리 4바이트(형식 v1 24바이트 뒤에 붙임 → 모두 28바이트, 풀기는 bench/nucleo_lora/decode.py):
 *   24  2  SDP810 차압 60초 평균, 0.02 Pa, 부호 있음 (±655 Pa)   없으면 0x8000
 *   26  2  DS18B20 온도, 0.01 °C, 부호 있음                      없으면 0x8000
 * 새 PCB 에는 이 두 센서가 없어서 보드 펌웨어 형식(report.h)은 안 건드림.
 * 모듈 대역 · 키 · OTAA 는 모듈에 저장된 값(11/19 맥에서 넣음)을 씀 — 여기선 안 바꿈.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "stm32h5xx_hal.h"
#include "abp2.h"
#include "bme280.h"
#include "ds18b20.h"
#include "hsc.h"
#include "i2c_stm32.h"
#include "log.h"
#include "lora_uart_stm32.h"
#include "report.h"
#include "sdp810.h"
#include "wioe5.h"

#define FW "nucleo-lora-bench 0.2"
#define REPORT_PERIOD_MS   60000u
#define JOIN_RETRY_MS      30000u
#define JOIN_TIMEOUT_MS    30000u
#define SEND_TIMEOUT_MS    20000u
#define LORA_BAUD          9600u
#define BENCH_LEN           (REPORT_LEN + 4u)
#define BENCH_NONE          ((int16_t)0x8000)

static i2c_stm32_t s_i2c1, s_i2c4;
static hsc_cfg_t s_hsc_cfg;
static abp2_cfg_t s_abp2_cfg;
static bme280_cal_t s_bme_cal;
static bool s_bme_ok;
static bool s_sdp_ok;
static bool s_ds_started;

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
    }
}

static void on_line(const char *line)
{
    printf("[LoRa] %s\n", line);
}

static HAL_StatusTypeDef lse_start(void)
{
    HAL_PWR_EnableBkUpAccess();
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState = RCC_LSE_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;
    return HAL_RCC_OscConfig(&osc);
}

typedef struct {
    bool hsc_ok, abp2_ok, bme_ok, sdp_ok, ds_ok;
    hsc_reading_t hsc;
    abp2_reading_t abp2;
    double patm_pa, bme_t_c;
    double sdp_pa, sdp_t_c;
    double ds_t_c;
} sample_t;

static void read_all(sample_t *s)
{
    const sens_err_t eh = hsc_read(&s_i2c1.bus, &s_hsc_cfg, &s->hsc);
    s->hsc_ok = (eh == SENS_OK || eh == SENS_STALE);
    s->abp2_ok = abp2_measure(&s_i2c4.bus, &s_abp2_cfg, &s->abp2) == SENS_OK;
    s->bme_ok = false;
    if (!s_bme_ok) {
        s_bme_ok = bme280_init(&s_i2c4.bus, &s_bme_cal) == SENS_OK;
    }
    if (s_bme_ok) {
        s->bme_ok = bme280_measure(&s_i2c4.bus, &s_bme_cal, &s->patm_pa, &s->bme_t_c) == SENS_OK;
        if (!s->bme_ok) {
            s_bme_ok = false;    /* 다음에 칩 ID 부터 다시 */
        }
    }

    if (!s_sdp_ok) {
        s_sdp_ok = sdp810_start(&s_i2c1.bus) == SENS_OK;
    }
    s->sdp_ok = s_sdp_ok && sdp810_read(&s_i2c1.bus, &s->sdp_pa, &s->sdp_t_c) == SENS_OK;
    if (s_sdp_ok && !s->sdp_ok) {
        s_sdp_ok = false;        /* 다음에 시작 명령부터 다시 */
    }

    /* DS18B20: 지난 초에 시작한 변환을 읽고(1 s > 750 ms) 다음 변환을 시작 */
    s->ds_ok = s_ds_started && ds18b20_read(&s->ds_t_c);
    s_ds_started = ds18b20_start();
}

static void print_sample(uint32_t n, const sample_t *s)
{
    printf("#%lu  HSC %s %.2f Pa %.1f C  |  ABP2 %s %.1f Pa %.2f C  |  SDP810 %s %.2f Pa %.1f C  |  BME280 %s %.1f Pa %.2f C  |  DS18B20 %s %.2f C\n",
           (unsigned long)n,
           s->hsc_ok ? "정상" : "실패", s->hsc.p_pa, s->hsc.t_c,
           s->abp2_ok ? "정상" : "실패", s->abp2.p_pa, s->abp2.t_c,
           s->sdp_ok ? "정상" : "실패", s->sdp_pa, s->sdp_t_c,
           s->bme_ok ? "정상" : "실패", s->patm_pa, s->bme_t_c,
           s->ds_ok ? "정상" : "실패", s->ds_t_c);
}

/* 반올림해서 int16 범위로 (0x8000 은 "없음" 표시라 피함) */
static int16_t q16(double x)
{
    const double r = round(x);
    if (r > 32767.0) {
        return 32767;
    }
    if (r < -32767.0) {
        return -32767;
    }
    return (int16_t)r;
}

static bool join_once(void)
{
    printf("[LoRa] 가입 시도 (AT+JOIN)\n");
    const wio_err_t e = wio_join(JOIN_TIMEOUT_MS);
    printf("[LoRa] 가입 %s (%d)\n", e == WIO_OK ? "성공" : "실패", (int)e);
    return e == WIO_OK;
}

/* 스택: 링커 스크립트(board/…ld)의 _Min_Stack_Size 가 1 KB 이고 시작 코드가 그걸 MSPLIM(스택 한계)으로 걸어 둠.
 * 소수 printf 한 줄이 1 KB 를 넘겨 HardFault(CFSR STKOF)가 났음(11/23) → 벤치는 한계를 16 KB 로 내림.
 * RAM 640 KB 중 쓰는 건 수 KB 라 힙과 닿지 않음. */
#define BENCH_STACK_BYTES  0x4000u
extern uint8_t _estack;

int main(void)
{
    __set_MSPLIM((uint32_t)&_estack - BENCH_STACK_BYTES);
    HAL_Init();
    if (log_init() != HAL_OK) {
        Error_Handler();
    }
    printf("\n\n=== %s · SYSCLK %lu Hz ===\n", FW, (unsigned long)HAL_RCC_GetSysClockFreq());

    /* 센서 버스 */
    if (i2c_stm32_init(&s_i2c1, I2C1) != HAL_OK || i2c_stm32_init(&s_i2c4, I2C4) != HAL_OK) {
        printf("I2C 초기화 실패\n");
    }
    hsc_default_cfg(&s_hsc_cfg);
    abp2_default_cfg(&s_abp2_cfg);
    s_bme_ok = bme280_init(&s_i2c4.bus, &s_bme_cal) == SENS_OK;
    printf("BME280 %s\n", s_bme_ok ? "칩 ID 0x60 확인" : "응답 없음 (나중에 다시)");
    s_sdp_ok = sdp810_start(&s_i2c1.bus) == SENS_OK;
    printf("SDP810 %s\n", s_sdp_ok ? "연속 측정 시작" : "응답 없음 (나중에 다시)");
    ds18b20_init();
    s_ds_started = ds18b20_start();
    printf("DS18B20 %s\n", s_ds_started ? "응답함" : "응답 없음 (나중에 다시)");

    /* LoRa: LPUART1 9600 은 LSE 로 돎 */
    if (lse_start() != HAL_OK) {
        printf("LSE(32.768 kHz) 시작 실패 — LoRa UART 못 씀\n");
        Error_Handler();
    }
    if (lora_uart_init(&g_lora_uart, LORA_BAUD) != HAL_OK) {
        printf("LPUART1 초기화 실패\n");
        Error_Handler();
    }
    wio_init(&g_lora_uart.io, on_line);
    HAL_Delay(200);
    for (int i = 0; i < 5; i++) {
        if (wio_ping() == WIO_OK) {
            printf("[LoRa] 모듈 응답 OK\n");
            break;
        }
        printf("[LoRa] 모듈 응답 없음 (%d/5)\n", i + 1);
        HAL_Delay(500);
    }
    (void)wio_query("AT+VER", 1000);
    (void)wio_query("AT+ID=DevEui", 1000);
    (void)wio_query("AT+DR", 1000);
    wio_dump("AT+CH", 1500);          /* 쓰는 채널 목록 (수용 대수 계산용) */
    wio_dump("AT+ADR", 500);
    wio_dump("AT+POWER", 500);

    bool joined = join_once();
    uint32_t t_join = HAL_GetTick();
    uint32_t t_report = HAL_GetTick();
    uint32_t t_tick = HAL_GetTick();
    uint32_t n = 0;
    uint16_t seq = 0;
    bool first = true;

    /* 1분 평균 */
    double dp_sum = 0.0, sdp_sum = 0.0;
    uint32_t dp_n = 0, sdp_n = 0;
    sample_t s = {0};

    for (;;) {
        const uint32_t now = HAL_GetTick();
        if ((uint32_t)(now - t_tick) >= 1000u) {
            t_tick = now;
            read_all(&s);
            n++;
            if (s.hsc_ok) {
                dp_sum += s.hsc.p_pa;
                dp_n++;
            }
            if (s.sdp_ok) {
                sdp_sum += s.sdp_pa;
                sdp_n++;
            }
            if (n % 10u == 0u) {
                print_sample(n, &s);
            }
        }

        if (!joined && (uint32_t)(now - t_join) >= JOIN_RETRY_MS) {
            joined = join_once();
            t_join = HAL_GetTick();
        }

        if ((uint32_t)(now - t_report) >= REPORT_PERIOD_MS) {
            t_report = now;
            report_t r = {0};
            r.seq = seq;
            r.unix_s = 0;
            r.pg_pa = s.abp2_ok ? s.abp2.p_pa : NAN;
            r.dp_pa = dp_n > 0u ? dp_sum / (double)dp_n : NAN;
            r.t_c = s.abp2_ok ? s.abp2.t_c : NAN;
            r.patm_pa = s.bme_ok ? s.patm_pa : NAN;
            r.batt_mv = 0;
            r.flags = REPORT_F_ZERO_UNSET | REPORT_F_TIME_UNSYNCED | (first ? REPORT_F_BOOT : 0u);
            if (dp_n < 54u) {
                r.flags |= REPORT_F_INCOMPLETE;
            }
            uint8_t buf[BENCH_LEN];
            (void)report_encode(&r, buf);
            /* 벤치 꼬리: SDP810 평균 0.02 Pa · DS18B20 0.01 °C */
            const double sdp_avg = sdp_n > 0u ? sdp_sum / (double)sdp_n : NAN;
            const int16_t sdp_q = sdp_n > 0u ? q16(sdp_avg * 50.0) : BENCH_NONE;
            const int16_t ds_q = s.ds_ok ? q16(s.ds_t_c * 100.0) : BENCH_NONE;
            buf[24] = (uint8_t)((uint16_t)sdp_q >> 8);
            buf[25] = (uint8_t)sdp_q;
            buf[26] = (uint8_t)((uint16_t)ds_q >> 8);
            buf[27] = (uint8_t)ds_q;
            const size_t len = BENCH_LEN;
            printf("[보고] #%u  정압 %.1f Pa · 차압 평균 %.3f Pa (%lu개) · 온도 %.2f C · 대기압 %.1f Pa · SDP810 평균 %.3f Pa (%lu개) · DS18B20 %.2f C\n",
                   (unsigned)seq, r.pg_pa, r.dp_pa, (unsigned long)dp_n, r.t_c, r.patm_pa,
                   sdp_avg, (unsigned long)sdp_n, s.ds_ok ? s.ds_t_c : NAN);
            dp_sum = 0.0;
            dp_n = 0;
            sdp_sum = 0.0;
            sdp_n = 0;
            if (!joined) {
                printf("[보고] 가입 전이라 안 보냄\n");
            } else {
                const wio_err_t e = wio_send_hex(buf, len, SEND_TIMEOUT_MS);
                printf("[보고] 송신 %s (%d)\n", e == WIO_OK ? "완료" : "실패", (int)e);
                if (e == WIO_E_NOT_JOINED) {
                    joined = false;
                    t_join = HAL_GetTick() - JOIN_RETRY_MS;   /* 바로 다시 가입 */
                }
                if (e == WIO_OK) {
                    first = false;
                }
            }
            seq++;
        }
    }
}
