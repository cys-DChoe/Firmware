/* 첫 불 켜기(first light) 프로그램 — 보드 오면 하는 순서 5 · 6단계용
 *   - LED1 0.5초마다 깜빡임 → MCU 가 돈다는 뜻
 *   - 1초마다 로그(USART3) 한 줄 → STLINK 없이도 살아 있는지 봄
 *   - 32.768 kHz 시계(LSE)를 켜 보고 결과를 로그에 남김. 실패면 LED2 켬
 * 시계는 아직 기본값(내부 HSI, 리셋 직후 32 MHz)을 그대로 씀. 잠 모드 · 센서 · LoRa 는 다음 단계.
 */
#include <stdio.h>
#include "main.h"
#include "log.h"

#define FW_VERSION "0.1.0-firstlight"

static void gpio_init(void);
static HAL_StatusTypeDef lse_start(void);

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

    HAL_StatusTypeDef lse = lse_start();
    printf("LSE 32.768 kHz: %s\n", lse == HAL_OK ? "OK" : "FAIL");
    if (lse != HAL_OK) {
        HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, GPIO_PIN_SET);
    }

    uint32_t last_blink = HAL_GetTick();
    uint32_t last_log = last_blink;
    while (1) {
        uint32_t now = HAL_GetTick();
        if (now - last_blink >= 500U) {
            last_blink = now;
            HAL_GPIO_TogglePin(LED1_PORT, LED1_PIN);
        }
        if (now - last_log >= 1000U) {
            last_log = now;
            int pressed = HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET;
            printf("t=%lu s  btn=%d\n", (unsigned long)(now / 1000U), pressed);
        }
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
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BTN_PORT, &gpio);
}

/* LSE(32.768 kHz 크리스털, PC14/PC15) 켜기. 나중에 RTC · 정지 모드에서 깨우는 시계로 씀. */
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
