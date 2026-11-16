#include "w25q.h"

#define CMD_WREN   0x06u
#define CMD_RDSR1  0x05u
#define CMD_READ   0x03u
#define CMD_PP     0x02u
#define CMD_SE     0x20u
#define CMD_PD     0xB9u
#define CMD_RPD    0xABu
#define CMD_JEDEC  0x9Fu

#define PP_TIMEOUT_MS   5u      /* tPP 최대 3 ms */
#define SE_TIMEOUT_MS   500u    /* tSE 최대 400 ms */

static void cmd_addr(uint8_t *b, uint8_t cmd, uint32_t addr)
{
    b[0] = cmd;
    b[1] = (uint8_t)(addr >> 16);
    b[2] = (uint8_t)(addr >> 8);
    b[3] = (uint8_t)addr;
}

static w25q_err_t wait_ready(const spi_dev_t *d, uint32_t timeout_ms)
{
    const uint8_t c = CMD_RDSR1;
    const uint32_t t0 = d->ms(d->ctx);
    for (;;) {
        uint8_t sr = 0xFF;
        if (d->xfer(d->ctx, &c, 1, &sr, 1) != 0) {
            return W25Q_E_BUS;
        }
        if ((sr & 0x01u) == 0u) {
            return W25Q_OK;
        }
        if ((uint32_t)(d->ms(d->ctx) - t0) > timeout_ms) {
            return W25Q_E_TIMEOUT;
        }
        if (timeout_ms > PP_TIMEOUT_MS) {
            d->delay_ms(d->ctx, 1);   /* 지우기는 길어서 1 ms 씩 쉬며 봄 */
        }
    }
}

static w25q_err_t write_enable(const spi_dev_t *d)
{
    const uint8_t c = CMD_WREN;
    return d->xfer(d->ctx, &c, 1, NULL, 0) == 0 ? W25Q_OK : W25Q_E_BUS;
}

w25q_err_t w25q_init(const spi_dev_t *d)
{
    const uint8_t rpd = CMD_RPD;
    if (d->xfer(d->ctx, &rpd, 1, NULL, 0) != 0) {
        return W25Q_E_BUS;
    }
    d->delay_ms(d->ctx, 1);          /* tRES1 3 µs */
    const uint8_t c = CMD_JEDEC;
    uint8_t id[3] = {0};
    if (d->xfer(d->ctx, &c, 1, id, 3) != 0) {
        return W25Q_E_BUS;
    }
    const uint32_t jedec = (uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2];
    return jedec == W25Q_JEDEC ? W25Q_OK : W25Q_E_ID;
}

w25q_err_t w25q_read(const spi_dev_t *d, uint32_t addr, uint8_t *buf, size_t n)
{
    if (addr >= W25Q_SIZE || n > W25Q_SIZE - addr) {
        return W25Q_E_ARG;
    }
    uint8_t c[4];
    cmd_addr(c, CMD_READ, addr);
    return d->xfer(d->ctx, c, 4, buf, n) == 0 ? W25Q_OK : W25Q_E_BUS;
}

w25q_err_t w25q_program(const spi_dev_t *d, uint32_t addr, const uint8_t *buf, size_t n)
{
    /* 페이지 끝을 넘으면 칩이 페이지 앞으로 감아 덮으므로 아예 막음 */
    if (n == 0u || addr >= W25Q_SIZE || (addr % W25Q_PAGE) + n > W25Q_PAGE) {
        return W25Q_E_ARG;
    }
    w25q_err_t e = write_enable(d);
    if (e != W25Q_OK) {
        return e;
    }
    uint8_t c[4];
    cmd_addr(c, CMD_PP, addr);
    if (d->xfer2(d->ctx, c, 4, buf, n) != 0) {
        return W25Q_E_BUS;
    }
    return wait_ready(d, PP_TIMEOUT_MS);
}

w25q_err_t w25q_erase_sector(const spi_dev_t *d, uint32_t addr)
{
    if (addr >= W25Q_SIZE || addr % W25Q_SECTOR != 0u) {
        return W25Q_E_ARG;
    }
    w25q_err_t e = write_enable(d);
    if (e != W25Q_OK) {
        return e;
    }
    uint8_t c[4];
    cmd_addr(c, CMD_SE, addr);
    if (d->xfer(d->ctx, c, 4, NULL, 0) != 0) {
        return W25Q_E_BUS;
    }
    return wait_ready(d, SE_TIMEOUT_MS);
}

w25q_err_t w25q_sleep(const spi_dev_t *d)
{
    const uint8_t c = CMD_PD;
    return d->xfer(d->ctx, &c, 1, NULL, 0) == 0 ? W25Q_OK : W25Q_E_BUS;
}
