#include "ds18b20.h"
#include "stm32h5xx_hal.h"

#define OW_PORT  GPIOG
#define OW_PIN   GPIO_PIN_14

static inline void ow_low(void) { OW_PORT->BSRR = (uint32_t)OW_PIN << 16; }
static inline void ow_release(void) { OW_PORT->BSRR = OW_PIN; }
static inline bool ow_level(void) { return (OW_PORT->IDR & OW_PIN) != 0u; }

static void delay_us(uint16_t us)
{
    const uint16_t t0 = (uint16_t)TIM6->CNT;
    while ((uint16_t)((uint16_t)TIM6->CNT - t0) < us) {
    }
}

void ds18b20_init(void)
{
    /* TIM6: 1 µs 눈금 (APB1 분주 1 이라 타이머 클럭 = PCLK1) */
    __HAL_RCC_TIM6_CLK_ENABLE();
    TIM6->PSC = HAL_RCC_GetPCLK1Freq() / 1000000u - 1u;
    TIM6->ARR = 0xFFFFu;
    TIM6->EGR = TIM_EGR_UG;
    TIM6->CR1 = TIM_CR1_CEN;

    __HAL_RCC_GPIOG_CLK_ENABLE();
    ow_release();
    GPIO_InitTypeDef g = {0};
    g.Pin = OW_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(OW_PORT, &g);
}

static bool ow_reset(void)
{
    ow_low();
    delay_us(480);
    __disable_irq();
    ow_release();
    delay_us(70);
    const bool present = !ow_level();
    __enable_irq();
    delay_us(410);
    return present;
}

static void ow_write_bit(bool bit)
{
    __disable_irq();
    ow_low();
    if (bit) {
        delay_us(5);
        ow_release();
        delay_us(60);
    } else {
        delay_us(60);
        ow_release();
        delay_us(5);
    }
    __enable_irq();
    delay_us(5);
}

static bool ow_read_bit(void)
{
    __disable_irq();
    ow_low();
    delay_us(3);
    ow_release();
    delay_us(9);
    const bool bit = ow_level();
    __enable_irq();
    delay_us(55);
    return bit;
}

static void ow_write(uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit((v >> i) & 1u);
    }
}

static uint8_t ow_read(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) {
            v |= (uint8_t)(1u << i);
        }
    }
    return v;
}

static uint8_t crc8(const uint8_t *p, int n)
{
    uint8_t crc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = p[i];
        for (int k = 0; k < 8; k++) {
            const uint8_t mix = (uint8_t)((crc ^ b) & 1u);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8Cu;
            }
            b >>= 1;
        }
    }
    return crc;
}

bool ds18b20_start(void)
{
    if (!ow_reset()) {
        return false;
    }
    ow_write(0xCCu);
    ow_write(0x44u);
    return true;
}

bool ds18b20_read(double *t_c)
{
    if (!ow_reset()) {
        return false;
    }
    ow_write(0xCCu);
    ow_write(0xBEu);
    uint8_t d[9];
    for (int i = 0; i < 9; i++) {
        d[i] = ow_read();
    }
    if (crc8(d, 8) != d[8]) {
        return false;
    }
    /* 전부 0 이면 CRC 도 0 이라 통과함 — 선이 낮게 붙은 경우를 걸러 냄 */
    bool all_zero = true;
    for (int i = 0; i < 9; i++) {
        if (d[i] != 0u) {
            all_zero = false;
        }
    }
    if (all_zero) {
        return false;
    }
    *t_c = (double)(int16_t)((d[1] << 8) | d[0]) / 16.0;
    return true;
}
