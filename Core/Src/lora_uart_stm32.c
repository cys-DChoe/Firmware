#include "lora_uart_stm32.h"
#include "main.h"

#define TX_TIMEOUT_MS 200u

lora_uart_t g_lora_uart;

static int io_write(void *ctx, const char *s, size_t n)
{
    lora_uart_t *u = ctx;
    return HAL_UART_Transmit(&u->h, (const uint8_t *)s, (uint16_t)n, TX_TIMEOUT_MS) == HAL_OK ? 0 : -1;
}

static int io_getc(void *ctx)
{
    lora_uart_t *u = ctx;
    if (u->tail == u->head) {
        return -1;
    }
    const uint8_t c = u->buf[u->tail];
    u->tail = (uint16_t)((u->tail + 1u) & (LORA_RX_BUF - 1u));
    return c;
}

static uint32_t io_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

static void io_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    HAL_Delay(ms);
}

static void io_rst(void *ctx, bool level)
{
    (void)ctx;
    /* 열린 드레인: 높음 = 놓음(모듈 안 풀업), 낮음 = 리셋 */
    HAL_GPIO_WritePin(MODEM_RST_PORT, MODEM_RST_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

HAL_StatusTypeDef lora_uart_init(lora_uart_t *u, uint32_t baud)
{
    /* NRST 핀: 열린 드레인, 평소 놓음 */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    HAL_GPIO_WritePin(MODEM_RST_PORT, MODEM_RST_PIN, GPIO_PIN_SET);
    GPIO_InitTypeDef g = {0};
    g.Pin = MODEM_RST_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(MODEM_RST_PORT, &g);

    u->head = u->tail = 0;
    u->overruns = 0;
    u->h.Instance = LPUART1;                  /* 커널 클럭은 리셋 기본값 PCLK3 */
    u->h.Init.BaudRate = baud;
    u->h.Init.WordLength = UART_WORDLENGTH_8B;
    u->h.Init.StopBits = UART_STOPBITS_1;
    u->h.Init.Parity = UART_PARITY_NONE;
    u->h.Init.Mode = UART_MODE_TX_RX;
    u->h.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    u->h.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    u->h.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    const HAL_StatusTypeDef st = HAL_UART_Init(&u->h);
    if (st != HAL_OK) {
        return st;
    }

    /* 받기 인터럽트: HAL 의 받기 함수를 거치지 않고 한 글자씩 직접 고리 버퍼로 */
    SET_BIT(u->h.Instance->CR1, USART_CR1_RXNEIE_RXFNEIE);
    HAL_NVIC_SetPriority(LPUART1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(LPUART1_IRQn);

    u->io.write = io_write;
    u->io.getc = io_getc;
    u->io.ms = io_ms;
    u->io.delay_ms = io_delay;
    u->io.rst = io_rst;
    u->io.ctx = u;
    return HAL_OK;
}

void lora_uart_irq(lora_uart_t *u)
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
        const uint16_t next = (uint16_t)((u->head + 1u) & (LORA_RX_BUF - 1u));
        if (next != u->tail) {           /* 꽉 차면 새 글자를 버림 */
            u->buf[u->head] = c;
            u->head = next;
        } else {
            u->overruns++;
        }
    }
}
