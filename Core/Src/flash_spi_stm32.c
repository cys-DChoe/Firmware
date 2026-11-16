#include <string.h>
#include "flash_spi_stm32.h"
#include "main.h"

#define SPI_TIMEOUT_MS 100u
#define CHUNK          64u

static void cs(bool low)
{
    HAL_GPIO_WritePin(FLASH_CS_N_PORT, FLASH_CS_N_PIN, low ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/* 받기는 FFh 를 보내며 받음 (전이중) */
static int rx_bytes(flash_spi_t *f, uint8_t *rx, size_t n)
{
    static uint8_t ff[CHUNK];
    if (ff[0] != 0xFFu) {
        memset(ff, 0xFF, sizeof(ff));
    }
    while (n > 0u) {
        const uint16_t k = (uint16_t)(n > CHUNK ? CHUNK : n);
        if (HAL_SPI_TransmitReceive(&f->h, ff, rx, k, SPI_TIMEOUT_MS) != HAL_OK) {
            return -1;
        }
        rx += k;
        n -= k;
    }
    return 0;
}

static int io_xfer(void *ctx, const uint8_t *tx, size_t ntx, uint8_t *rx, size_t nrx)
{
    flash_spi_t *f = ctx;
    cs(true);
    int e = (ntx > 0u && HAL_SPI_Transmit(&f->h, tx, (uint16_t)ntx, SPI_TIMEOUT_MS) != HAL_OK) ? -1 : 0;
    if (e == 0 && nrx > 0u) {
        e = rx_bytes(f, rx, nrx);
    }
    cs(false);
    return e;
}

static int io_xfer2(void *ctx, const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    flash_spi_t *f = ctx;
    cs(true);
    int e = HAL_SPI_Transmit(&f->h, a, (uint16_t)na, SPI_TIMEOUT_MS) == HAL_OK ? 0 : -1;
    if (e == 0 && nb > 0u) {
        e = HAL_SPI_Transmit(&f->h, b, (uint16_t)nb, SPI_TIMEOUT_MS) == HAL_OK ? 0 : -1;
    }
    cs(false);
    return e;
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

HAL_StatusTypeDef flash_spi_init(flash_spi_t *f)
{
    memset(f, 0, sizeof(*f));

    /* CS 먼저 높게(선택 안 함) — 보드에 풀업도 있음 */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    HAL_GPIO_WritePin(FLASH_CS_N_PORT, FLASH_CS_N_PIN, GPIO_PIN_SET);
    GPIO_InitTypeDef g = {0};
    g.Pin = FLASH_CS_N_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(FLASH_CS_N_PORT, &g);

    RCC_PeriphCLKInitTypeDef clk = {0};
    clk.PeriphClockSelection = RCC_PERIPHCLK_SPI1;
    clk.Spi1ClockSelection = RCC_SPI1CLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK) {
        return HAL_ERROR;
    }

    f->h.Instance = SPI1;
    f->h.Init.Mode = SPI_MODE_MASTER;
    f->h.Init.Direction = SPI_DIRECTION_2LINES;
    f->h.Init.DataSize = SPI_DATASIZE_8BIT;
    f->h.Init.CLKPolarity = SPI_POLARITY_LOW;          /* 모드 0 */
    f->h.Init.CLKPhase = SPI_PHASE_1EDGE;
    f->h.Init.NSS = SPI_NSS_SOFT;
    f->h.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
    f->h.Init.FirstBit = SPI_FIRSTBIT_MSB;
    f->h.Init.TIMode = SPI_TIMODE_DISABLE;
    f->h.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    f->h.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    f->h.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;   /* 쉬는 동안 SCK 를 낮게 붙잡음 */
    const HAL_StatusTypeDef st = HAL_SPI_Init(&f->h);
    if (st != HAL_OK) {
        return st;
    }

    f->dev.xfer = io_xfer;
    f->dev.xfer2 = io_xfer2;
    f->dev.ms = io_ms;
    f->dev.delay_ms = io_delay;
    f->dev.ctx = f;
    return HAL_OK;
}
