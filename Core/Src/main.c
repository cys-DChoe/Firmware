/* 가스 계량기 펌웨어 — 보드 전체를 잇는 곳 (설계서 "동작 흐름")
 *   켜기: 시계(LSE · RTC) → 센서(I2C2 · I2C3) → 외부 플래시(링) · 내장 EDATA(누적 · 설정 · 키) → 저장값 되살리기 → LoRa(9600 · 설정 · 가입)
 *   반복: app_step(재기 → 계산 → 1분마다 저장 · 보고) → 버튼 3초면 영점 → Stop 잠(1 s 또는 10 s)
 * 실제 흐름은 app.c 에 있고 맥에서 가짜 부품으로 통째로 시험함(tests/host/test_app.c). 여기는 부품 연결만.
 */
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "app.h"
#include "batt_stm32.h"
#include "flash_spi_stm32.h"
#include "i2c_stm32.h"
#include "log.h"
#include "lora_uart_stm32.h"
#include "lowpower.h"
#include "lowpower_stm32.h"
#include "nv_edata.h"
#include "nv_w25q.h"
#include "sensors.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#define FW_VERSION "0.3.0"
#define BTN_HOLD_ZERO_MS  3000u      /* 버튼 3초 이상 → 영점 */

static void gpio_init(void);
static HAL_StatusTypeDef lse_start(void);

/* ---------- 부품 ---------- */
static i2c_stm32_t s_i2c_dp, s_i2c_pg;
static sensors_t s_sens;
static flash_spi_t s_flash;
static ring_t s_ring;
static flash_ops_t s_nv_ops;
static nvlog_t s_nv_set, s_nv_ctr;
static rak_t s_rak;
static app_t s_app;
static volatile bool s_btn_irq;

/* ---------- app 이 쓰는 보드 기능 ---------- */
static void sens_power(bool on)
{
    HAL_GPIO_WritePin(SENS_EN_PORT, SENS_EN_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static bool hw_read(void *ctx, meter_sample_t *s)
{
    (void)ctx;
    sensors_result_t r;
    sensors_read(&s_sens, &r);
    *s = r.sample;
    return r.usable;
}

static uint32_t hw_now(void *ctx)
{
    (void)ctx;
    return lp_time_unix();
}

static bool hw_time_valid(void *ctx)
{
    (void)ctx;
    /* RTC 는 처음 켤 때 2000-01-01 부터 돎. 2026 년 이후면 망 시각으로 맞춘 것 (리셋해도 RTC 는 이어 감) */
    return lp_time_unix() >= 1767225600u;
}

static uint16_t hw_batt(void *ctx)
{
    (void)ctx;
    /* 1분 보고 직전 = LoRa 송신 전 쉬는 상태에서 잼 (펌웨어 주의 7번) */
    return batt_measure_mv(s_app.settings.batt_cal_mv);
}

static void hw_flash(void *ctx, bool on)
{
    (void)ctx;
    if (on) {
        (void)w25q_init(&s_flash.dev);     /* 깨우기(ABh) + ID 확인 */
    } else {
        (void)w25q_sleep(&s_flash.dev);    /* 깊은 잠 약 1 µA */
    }
}

static void hw_log(void *ctx, const char *m)
{
    (void)ctx;
    printf("%s\n", m);
}

static bool hw_set_time(void *ctx, uint32_t t)
{
    (void)ctx;
    return lp_set_time_unix(t) == HAL_OK;
}

static const app_hw_t HW = {hw_read, hw_now, hw_time_valid, hw_batt, hw_flash, hw_log, NULL, hw_set_time};

/* 버튼을 누르고 있는 동안 기다려 3초 넘으면 영점 */
static void button_check(void)
{
    if (HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) != GPIO_PIN_RESET) {
        return;
    }
    const uint32_t t0 = HAL_GetTick();
    while (HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET) {
        HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, GPIO_PIN_SET);
        if (HAL_GetTick() - t0 >= BTN_HOLD_ZERO_MS) {
            const bool ok = app_zero(&s_app);
            /* 영점 잡힘: LED1 세 번, 실패: LED2 */
            for (int i = 0; i < 6; i++) {
                HAL_GPIO_TogglePin(ok ? LED1_PORT : LED2_PORT, ok ? LED1_PIN : LED2_PIN);
                HAL_Delay(150);
            }
            while (HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET) {
            }
            break;
        }
    }
    HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_RESET);
}

int main(void)
{
    SystemCoreClockUpdate();
    HAL_Init();
    HAL_ICACHE_Enable();

    gpio_init();
    if (log_init() != HAL_OK) {
        Error_Handler();
    }
    printf("\n== gasmeter %s ==\n", FW_VERSION);
    printf("SYSCLK %lu Hz\n", (unsigned long)HAL_RCC_GetSysClockFreq());

    /* 1) 시계: LSE → RTC · 잠 설정 */
    const HAL_StatusTypeDef lse = lse_start();
    printf("LSE 32.768 kHz: %s\n", lse == HAL_OK ? "OK" : "FAIL");
    if (lse != HAL_OK || lp_init() != HAL_OK) {
        Error_Handler();                       /* LSE 없으면 잠 · LoRa 9600 이 안 됨 */
    }

    /* 2) 센서: I2C2(HSC) · I2C3(ABP2 · BMP390), 전원 켜고 BMP390 설정 */
    if (i2c_stm32_init(&s_i2c_dp, I2C2) != HAL_OK || i2c_stm32_init(&s_i2c_pg, I2C3) != HAL_OK) {
        Error_Handler();
    }
    sensors_init(&s_sens, &s_i2c_dp.bus, &s_i2c_pg.bus, sens_power);
    printf("센서 전원: BMP390 %s\n", sensors_power_on(&s_sens) == SENS_OK ? "OK" : "응답 없음");

    /* 3) 외부 플래시: 1분 기록 링 + 누적 · 설정 저장 */
    if (flash_spi_init(&s_flash) != HAL_OK) {
        Error_Handler();
    }
    const w25q_err_t fe = w25q_init(&s_flash.dev);
    printf("W25Q128: %s\n", fe == W25Q_OK ? "OK" : "ID 다름 · 응답 없음");
    if (fe != W25Q_OK || ring_mount(&s_ring, &s_flash.dev, RING_DEFAULT_BASE, RING_DEFAULT_SECTORS) != RING_OK) {
        Error_Handler();
    }
    /* 누적 · 설정 · 키: MCU 내장 고내구 데이터 플래시(EDATA, 은행 1 섹터 124–127). 처음 켤 땐 옵션 바이트로 EDATA 를 켬.
     * 안 되면 외부 플래시 위 4 MB 로 대신함 (로그로 알림) */
    uint32_t set_base = NV_EDATA_SETTINGS_BASE, ctr_base = NV_EDATA_COUNTER_BASE;
    if (nv_edata_init() == NV_EDATA_READY) {
        nv_edata_ops(&s_nv_ops);
        printf("저장: 내장 EDATA\n");
    } else {
        nv_w25q_ops(&s_nv_ops, &s_flash.dev);
        set_base = PERSIST_SETTINGS_BASE;
        ctr_base = PERSIST_COUNTER_BASE;
        printf("저장: EDATA 실패 → 외부 플래시\n");
    }
    if (nvlog_mount(&s_nv_set, &s_nv_ops, set_base) != NVLOG_OK || nvlog_mount(&s_nv_ctr, &s_nv_ops, ctr_base) != NVLOG_OK) {
        Error_Handler();
    }
    printf("링: 다음 순번 %lu\n", (unsigned long)s_ring.next_seq);

    /* 4) 계산 · 저장값 되살리기 */
    app_init(&s_app, &HW, &s_ring, &s_nv_set, &s_nv_ctr, &s_rak);
#ifdef SECRETS_HAVE_LORA_KEYS
    app_set_keys(&s_app, SECRET_DEV_EUI, SECRET_APP_EUI, SECRET_APP_KEY);
#endif
    printf("누적 %.4f Nm3 · 영점 %s\n", s_app.meter.total_nm3, s_app.settings.zero_set ? "있음" : "없음");

    /* 5) LoRa: LPUART1 9600(LSE) → 속도 맞추기 → 설정 · 가입 */
    if (lora_uart_init(&g_lora_uart, APP_LORA_BAUD) != HAL_OK) {
        Error_Handler();
    }
    rak_init(&s_rak, &g_lora_uart.io);
    printf("LoRa: %d\n", (int)app_lora_start(&s_app));
    hw_flash(NULL, false);

    /* 6) 깨고 · 재고 · 저장하고 · 보내고 · 자기 */
    while (1) {
        const uint32_t ms = app_step(&s_app);
        if (s_btn_irq) {
            s_btn_irq = false;
            button_check();
        }
        lp_sleep_ms(ms);
    }
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t pin)
{
    if (pin == BTN_PIN) {
        s_btn_irq = true;          /* 잠에서 깨움 → 주 루프에서 길게 눌렀는지 봄 */
    }
}

static void gpio_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* 켜지자마자 안전한 쪽으로 먼저 정해 둠:
     *   LED 끔 · 센서 전원 끔 · LTE PWRKEY/RST 를 누르지 않음(NPN 꺼짐 = 낮음) */
    HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(SENS_EN_PORT, SENS_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LTE_PWRKEY_PORT, LTE_PWRKEY_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LTE_RST_PORT, LTE_RST_PIN, GPIO_PIN_RESET);

    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    gpio.Pin = LED1_PIN | LED2_PIN;          /* PF3 · PF4 */
    HAL_GPIO_Init(LED1_PORT, &gpio);
    gpio.Pin = SENS_EN_PIN;                  /* PE3 */
    HAL_GPIO_Init(SENS_EN_PORT, &gpio);
    gpio.Pin = LTE_PWRKEY_PIN;               /* PB5 */
    HAL_GPIO_Init(LTE_PWRKEY_PORT, &gpio);
    gpio.Pin = LTE_RST_PIN;                  /* PG15 */
    HAL_GPIO_Init(LTE_RST_PORT, &gpio);

    /* 버튼 PC13: SW1 이 GND 로 당김, 보드에 풀업 저항 없음 → 내부 풀업, 누르면 낮음 (PCB3_2 넷 확인) */
    gpio.Pin = BTN_PIN;
    gpio.Mode = GPIO_MODE_IT_FALLING;        /* 누르면 Stop 에서 깨움 */
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BTN_PORT, &gpio);
    HAL_NVIC_SetPriority(EXTI13_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(EXTI13_IRQn);
}

/* LSE(32.768 kHz 크리스털, PC14/PC15) 켜기 — RTC · 잠에서 깨우기 · LoRa UART 9600 의 시계 */
static HAL_StatusTypeDef lse_start(void)
{
    HAL_PWR_EnableBkUpAccess();
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState = RCC_LSE_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;
    return HAL_RCC_OscConfig(&osc);
}

void Error_Handler(void)
{
    __disable_irq();
    HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_SET);
    while (1) {
    }
}
