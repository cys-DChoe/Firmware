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
#include "lte_uart_stm32.h"
#include "lowpower.h"
#include "lowpower_stm32.h"
#include "nv_edata.h"
#include "nv_w25q.h"
#include "ds18b20.h"
#include "onewire_stm32.h"
#include "sensors.h"
#include "wdg.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#define FW_VERSION "0.4.0"           /* 11/24: 보고 v2(28 B) · 링 칸 64 B · SDP810 · DS18B20 */
#define BTN_HOLD_ZERO_MS  3000u      /* 버튼 3초 이상 → 영점 */
#define BTN_STUCK_MS      10000u     /* 이보다 오래 눌려 있으면 고장으로 보고 그만 봄 (떨어지는 순간에만 깨우므로 다시 안 걸림) */

static void mpu_init(void);
static void gpio_init(void);
static HAL_StatusTypeDef lse_start(void);

/* ---------- 부품 ---------- */
static i2c_stm32_t s_i2c_dp, s_i2c_pg;
static onewire_t s_ow;               /* DS18B20 (PB13) */
static sensors_t s_sens;
static flash_spi_t s_flash;
static ring_t s_ring;
static flash_ops_t s_nv_ops;
static nvlog_t s_nv_set, s_nv_ctr;
static rak_t s_rak;
static lte_t s_lte;
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

/* 정압 센서 만점 (설정 · 내려받기로 바꿈 — 센서를 다른 범위 품번으로 갈았을 때) */
static void hw_set_pg_fs(void *ctx, double pa)
{
    (void)ctx;
    s_sens.abp2.p_max_pa = pa;
    printf("정압 센서 만점: %.0f Pa\n", pa);
}

/* 셀이 낮아 쉴 때: 센서 전원 · LED 끔. 다시 켤 때 센서 전원 · BMP390 설정 다시
 * 센서 전원(SENS_EN)은 차압 센서(HSC) 레일만 끔 — 정압(ABP2) · 기압(BMP390)은 3V3 상시라 대기 전류(수십 nA · µA)로 남음
 * (LTE 를 넣으면 여기서 먼저 꺼야 함 — 펌웨어 주의 2절, 지금은 켜지 않으므로 없음) */
static void hw_power_save(void *ctx, bool off)
{
    (void)ctx;
    if (off) {
        sensors_power_off(&s_sens);
        HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_RESET);
    } else {
        /* 차압 센서 레일(풀업도 같은 레일)이 올라온 뒤 I2C2 를 다시 엶 — 선이 내려갔다 오면서 굳은 BUSY 를 풂 */
        HAL_GPIO_WritePin(SENS_EN_PORT, SENS_EN_PIN, GPIO_PIN_SET);
        HAL_Delay(SENSORS_POWER_ON_MS);
        i2c_stm32_reopen(&s_i2c_dp);
        (void)sensors_power_on(&s_sens);
    }
}

/* SDP810 켜기 · 끄기 (설정 04) — 판에 J_SDP 가 없으면(board.h) 늘 끔. 끄면 다음 읽기부터 0x25 를 안 건드림(센서는 마지막 읽기 뒤 잠 1 µA) */
static void hw_set_sdp(void *ctx, bool on)
{
    (void)ctx;
    s_sens.sdp_off = !(on && BOARD_HAS_SDP810);
}

/* LED 짧게 깜빡: 30 ms 켬 · 150 ms 쉼 (깨어 있을 때라 HAL_Delay 가 됨). 빨강 = 문제, 초록 = 정상 */
static void hw_led(void *ctx, bool red, uint8_t blinks)
{
    (void)ctx;
    GPIO_TypeDef *port = red ? LED_RED_PORT : LED_GREEN_PORT;
    const uint16_t pin = red ? LED_RED_PIN : LED_GREEN_PIN;
    for (uint8_t i = 0; i < blinks; i++) {
        if (i > 0u) {
            HAL_Delay(150);
        }
        HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
        HAL_Delay(30);
        HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    }
}

static const app_hw_t HW = {hw_read,   hw_now, hw_time_valid, hw_batt,     hw_flash, hw_log, NULL,
                            hw_set_time, hw_set_pg_fs, hw_power_save, hw_led, hw_set_sdp};

/* 버튼을 누르고 있는 동안 기다려 3초 넘으면 영점 */
static void button_check(void)
{
    if (HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) != GPIO_PIN_RESET) {
        return;
    }
    const uint32_t t0 = HAL_GetTick();
    bool zeroed = false;
    while (HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET) {
        wdg_kick();
        const uint32_t held = HAL_GetTick() - t0;
        if (held >= BTN_STUCK_MS) {
            printf("버튼: %lu s 넘게 눌린 채 — 고장으로 보고 무시\n", (unsigned long)(BTN_STUCK_MS / 1000u));
            break;
        }
        if (zeroed) {
            continue;                   /* 손 뗄 때까지 (최대 BTN_STUCK_MS) */
        }
        HAL_GPIO_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, GPIO_PIN_SET);   /* 누르는 동안 초록 켬 */
        if (held >= BTN_HOLD_ZERO_MS) {
            HAL_GPIO_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, GPIO_PIN_RESET);
            const bool ok = app_zero(&s_app);
            /* 영점 잡힘: 초록 세 번, 실패: 빨강 세 번 */
            for (int i = 0; i < 6; i++) {
                HAL_GPIO_TogglePin(ok ? LED_GREEN_PORT : LED_RED_PORT, ok ? LED_GREEN_PIN : LED_RED_PIN);
                HAL_Delay(150);
            }
            HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_RESET);
            zeroed = true;
        }
    }
    HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_RESET);
}

int main(void)
{
    SystemCoreClockUpdate();
    HAL_Init();
    /* USB-C '죽은 배터리' 풀다운 끄기 — PB13(TEMP_DQ) · PB14(ABP2_EOC)는 UCPD1_CC1 · CC2 겸용(FT_c)이라
     * 리셋 뒤엔 짝 핀 PA9(DB1, 빈 핀) · PC9(DB2 = I2C3_SDA, 평소 높음)에 따라 5.1 kΩ 풀다운이 걸릴 수 있음
     * (DS14258 핀 표 · HAL 주석 "모든 경우에 끌 것" · ST 사례: PC9 높이자 PB14 3.3 → 1.5 V). UCPD 는 안 씀 (11/24 검수 점1)
     * PCB3_3 부터 PA9 는 판에서 GND 에 묶임(board.h BOARD_PA9_GND) — PA9 를 출력으로 쓰지 말 것(높이면 단락) */
    HAL_PWREx_DisableUCPDDeadBattery();
    mpu_init();                /* 캐시를 켜기 전에: 공장 보정값 · EDATA 는 캐시 안 함 */
    HAL_ICACHE_Enable();
    const wdg_boot_t boot = wdg_boot_cause();   /* 왜 켜졌나 (워치독 · 오류면 다음 보고에 표시) */
    wdg_start();                                /* 30 s — 켜는 동안도 멈추면 재시작 */

    gpio_init();
    if (log_init() != HAL_OK) {
        Error_Handler();
    }
    printf("\n== gasmeter %s · %s ==\n", FW_VERSION, BOARD_NAME);
    printf("SYSCLK %lu Hz\n", (unsigned long)HAL_RCC_GetSysClockFreq());
    printf("켠 까닭: %s", wdg_cause_name(boot.cause));
    if (boot.cause == WDG_CAUSE_FAULT) {
        printf(" (코드 %u · 주소 0x%08lx · CFSR 0x%08lx)", boot.code, (unsigned long)boot.info, (unsigned long)boot.cfsr);
    }
    printf(wdg_abnormal(&boot) ? " · 이어서 %lu번째\n" : "\n", (unsigned long)boot.streak);

    /* 1) 시계: LSE → RTC · 잠 설정 */
    const HAL_StatusTypeDef lse = lse_start();
    printf("LSE 32.768 kHz: %s\n", lse == HAL_OK ? "OK" : "FAIL");
    if (lse != HAL_OK || lp_init() != HAL_OK) {
        Error_Handler();                       /* LSE 없으면 잠 · LoRa 9600 이 안 됨 */
    }

    wdg_kick();

    /* 2) 센서: I2C2(HSC) · I2C3(ABP2 · BMP390), 전원 켜고 BMP390 설정 */
    if (i2c_stm32_init(&s_i2c_dp, I2C2) != HAL_OK || i2c_stm32_init(&s_i2c_pg, I2C3) != HAL_OK) {
        Error_Handler();
    }
    sensors_init(&s_sens, &s_i2c_dp.bus, &s_i2c_pg.bus, sens_power);
    printf("센서 전원: BMP390 %s\n", sensors_power_on(&s_sens) == SENS_OK ? "OK" : "응답 없음");
    /* PCB3_3 센서: SDP810(I2C3 0x25) · DS18B20(PB13) — 판에 자리가 없으면(board.h) 안 건드림, 보고 24 · 26 번은 0x8000 */
    s_sens.sdp_off = !BOARD_HAS_SDP810;
#if BOARD_HAS_DS18B20
    onewire_stm32_init(&s_ow);
    sensors_set_extra(&s_sens, &s_ow, HAL_GetTick, lp_time_ms);
    if (!onewire_stm32_timer_ok()) {
        printf("DS18B20: 안 씀 — µs 사이클 카운터(DWT)가 안 돎\n");
    } else {
        printf("DS18B20: %s\n", ds18b20_convert(&s_ow) == SENS_OK ? "있음" : "없음 (PB13 존재 펄스 없음)");
    }
#else
    (void)s_ow;
    sensors_set_extra(&s_sens, NULL, HAL_GetTick, lp_time_ms);
#endif
    /* 판에 자리가 있는지만 — SDP810 을 설정(04 00)으로 꺼 두었으면 app_init 이 따로 로그 */
    printf("판 %s: SDP810 %s · DS18B20 %s\n", BOARD_NAME, BOARD_HAS_SDP810 ? "자리 있음" : "자리 없음",
           BOARD_HAS_DS18B20 ? "자리 있음" : "자리 없음");

    wdg_kick();

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

    wdg_kick();

    /* 4) 계산 · 저장값 되살리기 */
    app_init(&s_app, &HW, &s_ring, &s_nv_set, &s_nv_ctr, &s_rak);
    app_set_boot(&s_app, wdg_abnormal(&boot));
#ifdef SECRETS_HAVE_LORA_KEYS
    app_set_keys(&s_app, SECRET_DEV_EUI, SECRET_APP_EUI, SECRET_APP_KEY);
#endif
    printf("누적 %.4f Nm3 · 영점 %s · 셀 보정 %+d mV · 방출 기준 %.0f Pa\n", s_app.meter.total_nm3,
           s_app.settings.zero_set ? "있음" : "없음", (int)s_app.settings.batt_cal_mv, app_vent_p_pa(&s_app));
#if defined(BATT_CAL_ACTUAL_MV) && (BATT_CAL_ACTUAL_MV > 0)
    /* 셀 보정 (보드 오면 4단계): -DBATT_CAL_ACTUAL_MV=멀티미터 값(mV) 로 빌드해 한 번 켜면 저장됨. 끝나면 0 으로 다시 빌드 */
    (void)app_batt_cal_once(&s_app, (uint16_t)BATT_CAL_ACTUAL_MV);
#endif

    /* 5) LoRa: LPUART1 9600(LSE) → 속도 맞추기 → 설정 · 가입 */
    if (lora_uart_init(&g_lora_uart, APP_LORA_BAUD) != HAL_OK) {
        Error_Handler();
    }
    rak_init(&s_rak, &g_lora_uart.io);
    wdg_kick();
    printf("LoRa: %d\n", (int)app_lora_start(&s_app));

    /* LTE (1차: 켜고 · 망 확인 · 끄기만). 평소엔 꺼 둠 — LoRa 명령 02 로 한 번 시험.
     * 벤치에서 서버 없이 볼 때는 -DLTE_TEST_AT_BOOT=ON 으로 빌드하면 켤 때 한 번 함 (셀 3.65 V 이상일 때) */
    lte_board_init(&g_lte_uart);
    lte_init(&s_lte, &g_lte_uart.io);
    app_set_lte(&s_app, &s_lte);
#ifdef LTE_TEST_AT_BOOT
    printf("LTE 시험: %s\n", app_lte_test(&s_app) ? "시작" : "안 함");
#endif
    hw_flash(NULL, false);

    /* 6) 깨고 · 재고 · 저장하고 · 보내고 · 자기 — 깰 때마다(1 · 10 s) 워치독을 밀어 줌 */
    uint32_t slept_ms = 0;          /* 제때 깬 잠의 합 — 시계(RTC)는 망 시각으로 맞출 때 뛰므로 안 씀 */
    bool healthy = false;
    while (1) {
        wdg_kick();
        if (!healthy && slept_ms >= 600000u) {
            wdg_healthy();          /* 10분 정상 → 이어진 이상 재시작 횟수 지움 */
            healthy = true;
        }
        const uint32_t ms = app_step(&s_app);
        if (s_btn_irq) {
            s_btn_irq = false;
            button_check();
        }
        if (lp_sleep_ms(ms) == LP_WAKE_TIMER && !healthy) {
            slept_ms += ms;
        }
    }
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t pin)
{
    if (pin == BTN_PIN) {
        s_btn_irq = true;          /* 잠에서 깨움 → 주 루프에서 길게 눌렀는지 봄 */
    }
}

/* 캐시(ICACHE)를 거치면 안 되는 곳 두 군데를 MPU 로 "캐시 안 함"으로 묶음 (ST 예제 FLASH_EDATA_EraseProgram 과 같은 방식)
 *   0: 0x08FFF000–0x08FFFFFF  OTP · 읽기 전용 영역 — VREFINT_CAL(배터리) · 플래시 크기(HAL 지우기가 읽음) · UID.
 *      캐시를 켠 채 읽으면 HardFault (ST 안내). 첫 배터리 측정에서 멈추던 원인.
 *   1: 0x09000000–0x0901FFFF  EDATA — 쓴 뒤 옛 값 · 줄 단위로 읽다 안 쓴 칸 ECC 오류를 막음 */
static void mpu_init(void)
{
    HAL_MPU_Disable();
    MPU_Attributes_InitTypeDef at = {0};
    at.Number = MPU_ATTRIBUTES_NUMBER0;
    at.Attributes = MPU_NOT_CACHEABLE;
    HAL_MPU_ConfigMemoryAttributes(&at);

    MPU_Region_InitTypeDef rg = {0};
    rg.Enable = MPU_REGION_ENABLE;
    rg.AttributesIndex = MPU_ATTRIBUTES_NUMBER0;
    rg.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    rg.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

    rg.Number = MPU_REGION_NUMBER0;
    rg.BaseAddress = 0x08FFF000u;
    rg.LimitAddress = 0x08FFFFFFu;
    rg.AccessPermission = MPU_REGION_ALL_RO;
    HAL_MPU_ConfigRegion(&rg);

    rg.Number = MPU_REGION_NUMBER1;
    rg.BaseAddress = 0x09000000u;
    rg.LimitAddress = 0x0901FFFFu;
    rg.AccessPermission = MPU_REGION_ALL_RW;
    HAL_MPU_ConfigRegion(&rg);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);       /* 나머지 주소는 기본 지도 그대로 */
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
    /* 원인(부른 곳)을 백업 칸에 적고 재시작 — 디버거가 붙어 있으면 빨강 LED 켜고 멈춤 (wdg.h) */
    wdg_fault(WDG_FAULT_ERROR, (uint32_t)__builtin_return_address(0), 0u);
}
