#include "i2c_stm32.h"
#include "i2c_timing.h"

#define I2C_TIMEOUT_MS  20u

void i2c_stm32_reopen(i2c_stm32_t *p)
{
    (void)HAL_I2C_DeInit(&p->h);
    (void)HAL_I2C_Init(&p->h);
    p->fails = 0;
    p->reopens++;
}

/* 결과 정리: 실패가 이어지면 버스를 다시 엶 (굳은 BUSY · 반쯤 간 전송 풀기) */
static int done(i2c_stm32_t *p, HAL_StatusTypeDef st)
{
    if (st == HAL_OK) {
        p->fails = 0;
        return 0;
    }
    if (++p->fails >= I2C_FAILS_REOPEN) {
        i2c_stm32_reopen(p);
    }
    return -1;
}

static int bus_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t len)
{
    i2c_stm32_t *p = ctx;
    return done(p, HAL_I2C_Master_Transmit(&p->h, (uint16_t)(addr7 << 1), (uint8_t *)data, (uint16_t)len,
                                           I2C_TIMEOUT_MS));
}

static int bus_read(void *ctx, uint8_t addr7, uint8_t *data, size_t len)
{
    i2c_stm32_t *p = ctx;
    return done(p, HAL_I2C_Master_Receive(&p->h, (uint16_t)(addr7 << 1), data, (uint16_t)len, I2C_TIMEOUT_MS));
}

static int bus_write_read(void *ctx, uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen)
{
    i2c_stm32_t *p = ctx;
    if (wlen == 1u) {
        /* 레지스터 주소 1바이트 → 다시 시작(repeated start) → 읽기 */
        return done(p, HAL_I2C_Mem_Read(&p->h, (uint16_t)(addr7 << 1), w[0], I2C_MEMADD_SIZE_8BIT, r, (uint16_t)rlen,
                                        I2C_TIMEOUT_MS));
    }
    if (bus_write(ctx, addr7, w, wlen) != 0) {
        return -1;
    }
    return bus_read(ctx, addr7, r, rlen);
}

static void bus_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    HAL_Delay(ms);
}

HAL_StatusTypeDef i2c_stm32_init(i2c_stm32_t *p, I2C_TypeDef *inst)
{
    /* I2C2 커널 클럭 = PCLK1, I2C3 = PCLK3 (리셋 기본값) */
    const uint32_t kernel = (inst == I2C3) ? HAL_RCC_GetPCLK3Freq() : HAL_RCC_GetPCLK1Freq();

    p->h.Instance = inst;
    p->h.Init.Timing = i2c_timing_100k(kernel);
    p->h.Init.OwnAddress1 = 0;
    p->h.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    p->h.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    p->h.Init.OwnAddress2 = 0;
    p->h.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    p->h.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    p->h.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    p->bus.write = bus_write;
    p->bus.read = bus_read;
    p->bus.write_read = bus_write_read;
    p->bus.delay_ms = bus_delay;
    p->bus.ctx = p;
    p->fails = 0;
    p->reopens = 0;
    return HAL_I2C_Init(&p->h);
}
