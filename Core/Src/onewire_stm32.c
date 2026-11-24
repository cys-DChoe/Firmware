/* 보드 1-Wire (onewire.h 시간표) — PB13 오픈 드레인, DWT 사이클로 µs */
#include "onewire_stm32.h"
#include "main.h"

static inline void dq_low(void) { TEMP_DQ_PORT->BSRR = (uint32_t)TEMP_DQ_PIN << 16; }
static inline void dq_release(void) { TEMP_DQ_PORT->BSRR = TEMP_DQ_PIN; }
static inline bool dq_level(void) { return (TEMP_DQ_PORT->IDR & TEMP_DQ_PIN) != 0u; }

/* 사이클 카운터가 실제로 도는지 (켤 때 확인, 지연 중 멈춘 게 보이면 false 로 내림).
 * false 면 1-Wire 를 '센서 없음'으로 둠 — 멈춘 카운터를 기다리다 워치독 재시작을 되풀이하지 않게 (11/24 검수 점2) */
static bool s_cyc_ok;

static void delay_us(uint32_t us)
{
    if (!s_cyc_ok) {
        return;
    }
    const uint32_t t0 = DWT->CYCCNT;
    const uint32_t n = us * (SystemCoreClock / 1000000u);
    /* 한 바퀴는 최소 몇 사이클이 걸리므로, 카운터가 돌면 바퀴 수가 n 에 닿기 전에 끝남.
     * 바퀴 수가 먼저 n 에 닿으면 카운터가 멈춘 것 → 표시하고 빠져나옴 (지연은 대략 맞게 끝남) */
    for (uint32_t spins = 0u; (uint32_t)(DWT->CYCCNT - t0) < n; spins++) {
        if (spins >= n) {
            s_cyc_ok = false;
            return;
        }
    }
}

bool onewire_stm32_timer_ok(void) { return s_cyc_ok; }

static bool ow_reset(void *ctx)
{
    (void)ctx;
    if (!s_cyc_ok) {
        return false;                   /* 시간을 못 재면 버스를 건드리지 않고 '없음' */
    }
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
    /* 카운터가 있고(NOCYCCNT = 0) 실제로 올라가는지 확인 */
    const uint32_t c0 = DWT->CYCCNT;
    for (volatile int i = 0; i < 8; i++) {
    }
    s_cyc_ok = ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0u) && (DWT->CYCCNT != c0);

    __HAL_RCC_GPIOB_CLK_ENABLE();
    dq_release();
    GPIO_InitTypeDef g = {0};
    g.Pin = TEMP_DQ_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;            /* 센서 자리가 없는 판(PCB3_2, 빈 핀)에서 선이 뜨지 않게 — PCB3_3 은 R_TMP_PU 4.7 kΩ 가 있어 같이 있어도 무해 */
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(TEMP_DQ_PORT, &g);

    ow->reset = ow_reset;
    ow->write_bit = ow_write_bit;
    ow->read_bit = ow_read_bit;
    ow->ctx = NULL;
}
