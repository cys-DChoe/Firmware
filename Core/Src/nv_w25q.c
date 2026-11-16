#include "nv_w25q.h"

static int rd(void *ctx, uint32_t a, uint8_t *b, size_t n) { return w25q_read(ctx, a, b, n) == W25Q_OK ? 0 : -1; }
static int pg(void *ctx, uint32_t a, const uint8_t *b, size_t n) { return w25q_program(ctx, a, b, n) == W25Q_OK ? 0 : -1; }
static int er(void *ctx, uint32_t a) { return w25q_erase_sector(ctx, a) == W25Q_OK ? 0 : -1; }

void nv_w25q_ops(flash_ops_t *ops, const spi_dev_t *dev)
{
    ops->read = rd;
    ops->program = pg;
    ops->erase = er;
    ops->sector_size = W25Q_SECTOR;
    ops->ctx = (void *)dev;
}
