#include "lte_uart_stm32.h"
#include "main.h"

#define TX_TIMEOUT_MS 200u

lte_uart_t g_lte_uart;

static int io_write(void *ctx, const char *s, size_t n)
{
    lte_uart_t *u = ctx;
    if (!u->up) {
        return -1;
    }
    return HAL_UART_Transmit(&u->h, (const uint8_t *)s, (uint16_t)n, TX_TIMEOUT_MS) == HAL_OK ? 0 : -1;
}

static int io_getc(void *ctx)
{
    lte_uart_t *u = ctx;
    if (u->tail == u->head) {
        return -1;
    }
    const uint8_t c = u->buf[u->tail];
    u->tail = (uint16_t)((u->tail + 1u) & (LTE_RX_BUF - 1u));
    return c;
}

static uint32_t io_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

static void io_pwrkey(void *ctx, bool press)
{
    (void)ctx;
    HAL_GPIO_WritePin(LTE_PWRKEY_PORT, LTE_PWRKEY_PIN, press ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void io_rst(void *ctx, bool assert)
{
    (void)ctx;
    HAL_GPIO_WritePin(LTE_RST_PORT, LTE_RST_PIN, assert ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static bool io_status(void *ctx)
{
    (void)ctx;
    /* VDDIO2 가 없으면 이 핀은 전원이 없어 0 으로 읽힘 — 모듈이 꺼졌다는 뜻과 같음 */
    if ((PWR->VMSR & PWR_VMSR_VDDIO2RDY) == 0u) {
        return false;
    }
    return HAL_GPIO_ReadPin(LTE_STATUS_PORT, LTE_STATUS_PIN) == GPIO_PIN_SET;
}

static void pins_analog(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = LTE_TXD_PIN | LTE_RXD_PIN;
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOG, &g);
}

static bool io_on(void *ctx, bool on)
{
    lte_uart_t *u = ctx;
    if (!on) {
        if (u->up) {
            HAL_NVIC_DisableIRQ(USART6_IRQn);
            (void)HAL_UART_DeInit(&u->h);
            __HAL_RCC_USART6_CLK_DISABLE();
            u->up = false;
        }
        pins_analog();                   /* 꺼지는 모듈 쪽으로 전압을 안 냄 (펌웨어 주의 4절) */
        return true;
    }
    if (u->up) {
        return true;
    }
    /* 모듈 VDD_EXT(1.8 V) → MCU VDDIO2 가 올라왔는지 (PWR VMSR 20번 비트, 펌웨어 주의 4절) */
    if ((PWR->VMSR & PWR_VMSR_VDDIO2RDY) == 0u) {
        return false;
    }
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_USART6_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = LTE_TXD_PIN | LTE_RXD_PIN;   /* PG14 TX · PG9 RX */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;                /* 1.8 V 쪽으로 당김 — 모듈이 아직 말 안 할 때 RX 가 뜨지 않게 */
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF7_USART6;
    HAL_GPIO_Init(GPIOG, &g);

    u->head = u->tail = 0;
    u->h.Instance = USART6;
    u->h.Init.BaudRate = LTE_BAUD;
    u->h.Init.WordLength = UART_WORDLENGTH_8B;
    u->h.Init.StopBits = UART_STOPBITS_1;
    u->h.Init.Parity = UART_PARITY_NONE;
    u->h.Init.Mode = UART_MODE_TX_RX;
    u->h.Init.HwFlowCtl = UART_HWCONTROL_NONE;   /* 모듈 공장값 흐름 제어 없음 (AT+IFC=0,0) [가정 — 보드에서 확인] */
    u->h.Init.OverSampling = UART_OVERSAMPLING_16;
    u->h.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    u->h.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&u->h) != HAL_OK) {
        pins_analog();
        __HAL_RCC_USART6_CLK_DISABLE();
        return false;
    }
    SET_BIT(u->h.Instance->CR1, USART_CR1_RXNEIE_RXFNEIE);
    HAL_NVIC_SetPriority(USART6_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART6_IRQn);
    u->up = true;
    return true;
}

void lte_board_init(lte_uart_t *u)
{
    /* STATUS(PB9): 입력, 풀 없음 (모듈이 1.8 V 로 밂). PWRKEY · RST 는 main.c gpio_init 에서 낮음 출력 */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = LTE_STATUS_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(LTE_STATUS_PORT, &g);

    u->up = false;
    u->io.write = io_write;
    u->io.getc = io_getc;
    u->io.ms = io_ms;
    u->io.pwrkey = io_pwrkey;
    u->io.rst = io_rst;
    u->io.status = io_status;
    u->io.io_on = io_on;
    u->io.ctx = u;
}

void lte_uart_irq(lte_uart_t *u)
{
    USART_TypeDef *r = u->h.Instance;
    const uint32_t isr = r->ISR;
    if (isr & USART_ISR_ORE) {
        r->ICR = USART_ICR_ORECF;
        u->overruns++;
    }
    if (isr & (USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) {
        r->ICR = USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
    }
    while (r->ISR & USART_ISR_RXNE_RXFNE) {
        const uint8_t c = (uint8_t)r->RDR;
        const uint16_t next = (uint16_t)((u->head + 1u) & (LTE_RX_BUF - 1u));
        if (next != u->tail) {
            u->buf[u->head] = c;
            u->head = next;
        } else {
            u->overruns++;
        }
    }
}
