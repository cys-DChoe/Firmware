/* 보드 1-Wire (onewire.h 시간표) — PB13 오픈 드레인, DWT 사이클로 µs */
#include "onewire_stm32.h"
#include "main.h"

static inline void dq_low(void) { TEMP_DQ_PORT->BSRR = (uint32_t)TEMP_DQ_PIN << 16; }
static inline void dq_release(void) { TEMP_DQ_PORT->BSRR = TEMP_DQ_PIN; }
static inline bool dq_level(void) { return (TEMP_DQ_PORT->IDR & TEMP_DQ_PIN) != 0u; }

static void delay_us(uint32_t us)
{
    const uint32_t t0 = DWT->CYCCNT;
    const uint32_t n = us * (SystemCoreClock / 1000000u);
    while ((uint32_t)(DWT->CYCCNT - t0) < n) {
    }
}

static bool ow_reset(void *ctx)
{
    (void)ctx;
    dq_low();
    delay_us(480);
    __disable_irq();
    dq_release();
    delay_us(70);
    const bool present = !dq_level();
    __enable_irq();
    delay_us(410);
    return present;
}

static void ow_write_bit(void *ctx, bool bit)
{
    (void)ctx;
    __disable_irq();
    dq_low();
    if (bit) {
        delay_us(5);
        dq_release();
        delay_us(60);
    } else {
        delay_us(60);
        dq_release();
        delay_us(5);
    }
    __enable_irq();
    delay_us(5);
}

static bool ow_read_bit(void *ctx)
{
    (void)ctx;
    __disable_irq();
    dq_low();
    delay_us(3);
    dq_release();
    delay_us(9);
    const bool bit = dq_level();
    __enable_irq();
    delay_us(55);
    return bit;
}

void onewire_stm32_init(onewire_t *ow)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;     /* DWT 켜기 */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    dq_release();
    GPIO_InitTypeDef g = {0};
    g.Pin = TEMP_DQ_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;            /* 어댑터가 없는 판(빈 핀)에서 선이 뜨지 않게 — 어댑터 4.7 kΩ 과 같이 있어도 무해 */
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(TEMP_DQ_PORT, &g);

    ow->reset = ow_reset;
    ow->write_bit = ow_write_bit;
    ow->read_bit = ow_read_bit;
    ow->ctx = NULL;
}
